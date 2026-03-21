#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Scene;
import Scene.Raytracing;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.LightHelpers;
import Rendering.Lights.EmissiveLightSampler;
import Rendering.Lights.EmissiveLightSamplerHelpers;
import StructsAndHelpers;
import PhotonDifferentials;

cbuffer CB
{
    uint gMaxBounces;           // Max number of bounces allowed
    uint gFrameCount;           // Frame count since scene was loaded.
    // Remove probably 
    float2 gPhotonRadius;       // Global/Caustic radius

    uint gDispatchDimension;    // Dispatch dims (x,y identical) 
    float gGlobalRejectionProb; // Rejection Probability for global photons
    bool gUseAnalyticLights;    // true if analytic lights should be used
    uint _pad;    
}

#if USE_EMISSIVE_LIGHT // Buffer is only valid if emissive light is enabled
cbuffer Light {
    EmissiveLightSampler gEmissiveSampler;
}
#endif

RWStructuredBuffer<AABB> gPhotonAABB[2];
RWStructuredBuffer<PhotonDifferential> gPhotonData[2];
RWStructuredBuffer<uint> gPhotonCounter;

static const bool kAlphaTest = true;
static const uint2 kPhotonBufferSize = uint2(PHOTON_BUFFER_SIZE_GLOBAL, PHOTON_BUFFER_SIZE_CAUSTIC);
static const float kRoughnessThreshold = ROUGHNESS_THRESHOLD;
static const bool kRussianRoulette = true;  //TODO as option

static const uint kMIHints = (uint)MaterialInstanceHints::AdjustShadingNormal;

struct RayPayload
{
    PackedHitInfo packedHitInfo; //< Hit Info

    /** Create ray payload with default parameters.
     */
    __init()
    {
        this.packedHitInfo = {};
    }
}

//Helper struct to generate a photon form the lightSources
struct PhotonLightSample {
    float3 posW;
    float3 normal;
    float3 flux;
    float area;
};

/*  Transforms direction from local to world space
    \param[in] Face Normal
    \param[in] local direction
    \return world space direction
*/
float3 fromLocalToWorld(const in float3 normal, float3 w)
{
    // transform from local space -> random dir vector in world space
    float3 bitangent = perp_stark(normal);
    float3 tangent = cross(bitangent, normal);

    return w.x * tangent + w.y * bitangent + w.z * normal;
}

#if USE_EMISSIVE_LIGHT // Function is only valid if the emissive sampler is set
/*  Creates a photon light sample from emissive triangles weighted by emissive power
    \param[in/out] Random Number Generator
    \param[out] Photon light sample
    \return true if light sample is valid
*/
bool createLightSample(inout SampleGenerator sg, out PhotonLightSample pls) {
    pls = {};
    if (gScene.lightCollection.isEmpty())
        return false;

    // Randomly pick a triangle out of the global list with uniform probability
    const uint triangleCount = gScene.lightCollection.triangleCount;
    // Safety precaution as the result of the multiplication may be rounded to triangleCount even if uLight < 1.0 when triangleCount is large.
    uint triangleIndex = min((uint)(sampleNext1D(sg) * triangleCount), triangleCount - 1);

    uint2 packed = gEmissiveSampler._emissivePower.triangleAliasTable[triangleIndex];
    float threshold = f16tof32(packed.x >> 16u);
    uint selectAbove = ((packed.x & 0xFFFFu) << 8u) | ((packed.y >> 24u) & 0xFFu);
    uint selectBelow = packed.y & 0xFFFFFFu;

    // Test the threshold in the current table entry; pick one of the two options
    triangleIndex = (sampleNext1D(sg) >= threshold) ? selectAbove : selectBelow;

    float triangleSelectionPdf = gScene.lightCollection.fluxData[triangleIndex].flux * gEmissiveSampler._emissivePower.invWeightsSum;

    //Sample emissive triangle
    const EmissiveTriangle tri = gScene.lightCollection.getTriangle(triangleIndex);
    const float3 barycentrics = sample_triangle(sampleNext2D(sg));
    pls.posW = tri.getPosition(barycentrics);
    pls.normal = tri.normal;
    pls.area = tri.area;

    // Get Flux
    float2 uv = tri.getTexCoord(barycentrics);
    pls.flux = gScene.materials.evalEmissive(tri.materialID, uv) * tri.area;

    pls.posW = computeRayOrigin(pls.posW, pls.normal); //Offset as this light is real geometry
    pls.flux /= triangleSelectionPdf;

    return true;
}
#endif

/*  Creates a photon light sample from analytic point or spot lights
    \param[in/out] Random Number Generator
    \param[out] Photon light sample
    \param[out] Direction the photon should be dispatched
    \return true if light sample is valid
*/
bool createLightSampleAnalytic(inout SampleGenerator sg, out PhotonLightSample pls, out float3 sampledDir)
{
    pls = {};
    sampledDir = float3(0);
    const uint lightCount = gScene.getLightCount();
    if (lightCount == 0) return false;

    // Uniformly sample one of the analytic lights
    float rndLightIdx = float(lightCount) * sampleNext1D(sg);
    uint lightIdx = min(uint(floor(rndLightIdx)), lightCount - 1);
    LightData lightData = gScene.getLight(lightIdx);

    // Only Point(+ Spot) lights are supported (due to RTXDI)
    if (lightData.type != uint(LightType::Point)) return false;

    float invLightPdf;
    float3 woLocal;
    float3 rnd = sampleNext3D(sg);
    rnd.z = 0;
    float spotAngle = lightData.cosOpeningAngle;
    if (lightData.openingAngle < M_2PI) // If the opening angle is smaller than 2PI it is a spotlight
    {
        spotAngle = cos(lightData.openingAngle - lightData.penumbraAngle * rnd.z);
        woLocal = sample_cone(rnd.xy, spotAngle);
    }
    else // Pointlight
    {
        woLocal = sample_sphere(rnd.xy);
    }
    invLightPdf = M_2PI * (1.f - spotAngle);

    if (invLightPdf <= 0.f) return false;

    invLightPdf *= lightCount; // Take the select pdf into account

    // Fill the photon light struct
    pls.posW = lightData.posW;
    pls.normal = lightData.dirW;
    pls.flux = lightData.intensity * invLightPdf;
    pls.area = 0.0f; // point light have 0 area

    sampledDir = fromLocalToWorld(pls.normal, woLocal);
    return true;
}

//Add empty hit info for miss
[shader("miss")]
void miss(inout RayPayload rayData: SV_RayPayload)
{
    HitInfo hitInfo = {};
    rayData.packedHitInfo = hitInfo.pack(); // Invalid hit
}

//For alpha test
[shader("anyhit")]
void anyHit(inout RayPayload rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs: SV_IntersectionAttributes)
{
    if (kAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

//Add triangle hit to the payload
[shader("closesthit")]
void closestHit(inout RayPayload rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs: SV_IntersectionAttributes) {
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;

    rayData.packedHitInfo = HitInfo(triangleHit).pack();
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 dispatchDims = DispatchRaysDimensions().xy;

    const float invRejection = 1.f / gGlobalRejectionProb;

    SampleGenerator sg = SampleGenerator(pixel, gFrameCount * 5);
    RayDesc ray;
    PhotonLightSample pls;

    //Create a photon by sampling from emissive lights or analytic light sources
#if USE_EMISSIVE_LIGHT
    if (gUseAnalyticLights)
#endif
    { // Analytic light sample
        float3 sampleDir;
        if (!createLightSampleAnalytic(sg, pls, sampleDir))
            return;

        ray.Direction = sampleDir;
    }
#if USE_EMISSIVE_LIGHT
    else { // Emissive light sample
        if (!createLightSample(sg, pls))
            return;

        float lightDirPDF = 1.0;
        float3 woLocal = sample_cosine_hemisphere_concentric(sampleNext2D(sg), lightDirPDF);

        if (lightDirPDF <= 0.f) return;

        ray.Direction = fromLocalToWorld(pls.normal, woLocal);
        pls.flux *= M_PI; // Lambert Emitter
    }
#endif

    float totalPhotons = dispatchDims.x * dispatchDims.y;
    pls.flux /= totalPhotons; // Division through all dipatched photons
    
    //Prepare the rest of the ray
    ray.Origin = pls.posW;
    ray.TMin = 0.0;
    ray.TMax = FLT_MAX;
    RayPayload rayPayload = RayPayload();
    
    bool storeAsCaustic = false;
    float3 photonStartFlux = pls.flux;
    float3 thp = float3(1.0); //Throughput

    // Prepare photon differential
    PhotonDifferential photonDifferential = {};
    photonDifferential.flux = photonStartFlux;
    photonDifferential.dir = -ray.Direction;
    photonDifferential.normalW = pls.normal;

    // initialize differentials
    float3 tangent = perp_stark(pls.normal);
    float3 bitangent = cross(pls.normal, tangent);
    float3 dDdx = float3(0);
    float3 dDdy = float3(0);

#if USE_EMISSIVE_LIGHT
    if (!gUseAnalyticLights)
    {
        float a = sqrt(pls.area / totalPhotons);
        photonDifferential.dPdx = tangent * a;
        photonDifferential.dPdy = bitangent * a;
    }
    else
#endif
    {
        photonDifferential.dPdx = float3(0);
        photonDifferential.dPdy = float3(0);

        float solidAnglePerPhoton = (4.0f * M_PI) / totalPhotons;
        float angularStep = sqrt(solidAnglePerPhoton);
        dDdx = cross(ray.Direction, tangent) * angularStep;
        dDdy = cross(ray.Direction, bitangent) * angularStep;
    }

    // Main Raytrace Loop
    for (uint i = 0; i < gMaxBounces; i++)
    {
        TraceRay(gScene.rtAccel, 0 /*Flags*/, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayPayload);

        HitInfo hit = HitInfo(rayPayload.packedHitInfo);
        //Stop if photon leaves the scene
        if (!hit.isValid()) return;

        // Get shading infos and material properties
        let lod = ExplicitLodTextureSampler(0);
        ShadingData sd = loadShadingData(hit, ray.Direction, lod);
        let mi = gScene.materials.getMaterialInstance(sd, lod, kMIHints);
        let bsdfProperties = mi.getProperties(sd);

        float t = distance(ray.Origin, sd.posW);

        // TODO: Ensure this returns a normal facing against the ray direction
        float3 N = sd.getOrientedFaceNormal();
        if (dot(N, ray.Direction) > 0.0f)
            N = -N;

        updateTransferRayDifferential(N, photonDifferential.dPdx, dDdx, ray.Direction, t);
        updateTransferRayDifferential(N, photonDifferential.dPdy, dDdy, ray.Direction, t);

        // Store Photon if a diffuse surface was hit
        photonDifferential.normalW = N;

        bool reflectedDiffuse = bsdfProperties.roughness > kRoughnessThreshold;
        if (reflectedDiffuse)
        {
            PhotonType photonType = storeAsCaustic ? PhotonType::Caustic : PhotonType::Global;
            //Global photons can be rejected. The flux of the stored ones should be increased by the inverse of the rejection probability
            bool storePhoton = true;
            if (photonType == PhotonType::Global)
            {
                storePhoton = sampleNext1D(sg) <= gGlobalRejectionProb;
                photonDifferential.flux *= invRejection;
            }
            //Insert photon into buffer using a counter with interlocked add
            if (storePhoton)
            {
                uint photonTypeIdx = (uint)photonType;
                uint photonInsertIndex = 0;
                InterlockedAdd(gPhotonCounter[photonTypeIdx], 1u, photonInsertIndex);
                if (photonInsertIndex <= kPhotonBufferSize[photonTypeIdx]) // after InterlockedAdd PhotonInsertIndex will be equal to the number of global or caustic photons
                {
                    AABB photonAABB;
                    // Photon
                    if (photonType == PhotonType::Global)
                        photonAABB = AABB(sd.posW - gPhotonRadius[photonTypeIdx], sd.posW + gPhotonRadius[photonTypeIdx]);
                    else
                    {
                        // Photon differentials footprint
                        float3 footprintExtents = 0.5f * (abs(photonDifferential.dPdx) + abs(photonDifferential.dPdy));
                        footprintExtents = max(footprintExtents, float3(gPhotonRadius[photonTypeIdx]));
                        photonAABB = AABB(sd.posW - footprintExtents, sd.posW + footprintExtents);
                    }

                    gPhotonAABB[photonTypeIdx][photonInsertIndex] = photonAABB;
                    gPhotonData[photonTypeIdx][photonInsertIndex] = photonDifferential;
                }
            }

            photonDifferential.dPdx = float3(0);
            photonDifferential.dPdy = float3(0);
            dDdx = float3(0);
            dDdy = float3(0);

            float3 tangent = perp_stark(N);
            float3 bitangent = cross(N, tangent);
            float beamSize = 0.01f;
            photonDifferential.dPdx = tangent * beamSize;
            photonDifferential.dPdy = bitangent * beamSize;
        }

        // Sample Photon bounce for next iteration
        BSDFSample bsdfSample;
        bool validSample = mi.sample(sd, sg, bsdfSample, true /*ImportanceSampling*/);
        if (!validSample) return;

        bool transmission = bsdfSample.isLobe(LobeType::Transmission);

        if (!reflectedDiffuse) // specular surface
        {
            float3 bitangentW = perp_stark(bsdfProperties.guideNormal);
            float3 tangentW = cross(bitangentW, bsdfProperties.guideNormal);

            float uSizeX = dot(photonDifferential.dPdx, tangentW);
            float vSizeX = dot(photonDifferential.dPdx, bitangentW);

            float uSizeY = dot(photonDifferential.dPdy, tangentW);
            float vSizeY = dot(photonDifferential.dPdy, bitangentW);

            float3 dNdu = float3(0);
            float3 dNdv = float3(0);

            float3 dNdx = dNdu * uSizeX + dNdv * vSizeX; // Zero actually
            float3 dNdy = dNdu * uSizeY + dNdv * vSizeY; // Zero actually

            if (transmission)
            {
                bool entering = dot(ray.Direction, N) < 0.0f;

                float currentIOR = entering ? 1.0f : sd.IoR;
                float nextIOR = entering ? sd.IoR : 1.0f;
                float eta = currentIOR / nextIOR;

                updateRefractRayDifferential(ray.Direction, bsdfSample.wo, N, eta, dNdx, dDdx);
                updateRefractRayDifferential(ray.Direction, bsdfSample.wo, N, eta, dNdy, dDdy);
            }
            else
            {
                updateReflectRayDifferential(N, dNdx, dDdx, ray.Direction);
                updateReflectRayDifferential(N, dNdy, dDdy, ray.Direction);
            }
        }
        else
        {
            // Diffuse bounce: Scatters the beam, resetting coherent directional spread
            dDdx = float3(0);
            dDdy = float3(0);

        }

        ray.Origin = sd.computeRayOrigin(!transmission); // false is for transmissions
        ray.Direction = bsdfSample.wo;
        thp *= bsdfSample.weight;

        // Russian Roulette
        const float rrVal = (thp.x + thp.y + thp.z) / 3.0;
        const float prob = max(0.f, 1.f - rrVal);
        if (sampleNext1D(sg) < prob)
        {
            return; // Photon is absorbed
        }
        else
        {
            thp /= (1.f - prob);
        }

        // Update Photon
        storeAsCaustic = !reflectedDiffuse;
        photonDifferential.dir = -ray.Direction;
        photonDifferential.flux = photonStartFlux * thp;
        photonDifferential.pathLength++;
    }
}
