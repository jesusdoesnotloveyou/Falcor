/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/

#include "ReSTIR_FPDG.h"

#include "RenderGraph/RenderPassHelpers.h"

#include "Rendering/Lights/EmissiveUniformSampler.h"
#include "Rendering/Lights/LightBVHSampler.h"
#include "Rendering/Lights/EmissivePowerSampler.h"

namespace
{
const std::string kShaderFolder = "RenderPasses/ReSTIR_FPDG/";
const std::string kShaderTracePhotons = kShaderFolder + "TracePhotonDifferentials.rt.slang";
const std::string kShaderGenInitialSamples = kShaderFolder + "GenerateInitialSamples.rt.slang";
const std::string kShaderResamplingReservoirFG = kShaderFolder + "ResamplePhotonDifferentialsReservoirFG.cs.slang";
const std::string kShaderResamplingReservoirCaustic = kShaderFolder + "ResamplePhotonDifferentialsReservoirCaustic.cs.slang";
const std::string kShaderEvaluateReservoirs = kShaderFolder + "EvaluatePhotonDifferentialsReservoirs.cs.slang";

const ShaderModel kShaderModel = ShaderModel::SM6_5;

// Render Pass inputs and outputs
// To acquire initial hit information
const std::string kInputVBuffer = "vbuffer";
// For temporal resampling
const std::string kInputMotionVectors = "mvec";

const Falcor::ChannelList kInputChannels{
    {kInputVBuffer, "gVBuffer", "Visibility buffer in packed format"},
    {kInputMotionVectors, "gMotionVectors", "Motion vector buffer (float format)", true /* optional */},
};

// Outputs
const std::string kOutputColor = "color";

const Falcor::ChannelList kOutputChannels{{kOutputColor, "gOutColor", "HDR output color", false /*optional*/, ResourceFormat::RGBA32Float}};

}; // namespace

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, ReSTIR_FPDG>();
}

ReSTIR_FPDG::ReSTIR_FPDG(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{

    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
}

Properties ReSTIR_FPDG::getProperties() const
{
    return {};
}

RenderPassReflection ReSTIR_FPDG::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void ReSTIR_FPDG::compile(RenderContext* pRenderContext, const CompileData& compileData)
{
    mScreenRes = compileData.defaultTexDims;
}

void ReSTIR_FPDG::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mpScene) return;

    beginFrame(pRenderContext, renderData);

    const auto& motionVectors = renderData.getResource(kInputMotionVectors)->asTexture();

    // Init RTXDI if it is enabled
    if (mDirectLightMode == DirectLightingMode::RTXDI && !mpRTXDI)
    {
        mpRTXDI = std::make_unique<RTXDI>(mpScene, mRTXDIOptions);
    }
    // Delete RTXDI if it is set and the mode changed
    if (mDirectLightMode != DirectLightingMode::RTXDI && mpRTXDI) mpRTXDI = nullptr;

    if (mpRTXDI) mpRTXDI->beginFrame(pRenderContext, mScreenRes);
    if (mpRTXDI) mpRTXDI->update(pRenderContext, motionVectors);

    tracePhotonDifferentialsPass(pRenderContext, renderData, !mHasMixedLights && mHasAnalyticLights, true);
    generateInitialSamplesPass(pRenderContext, renderData);
    

    if (mpRTXDI) mpRTXDI->endFrame(pRenderContext);

    mFrameCount++;
    mCanResample = true;
}

void ReSTIR_FPDG::renderUI(Gui::Widgets& widget) {}

void ReSTIR_FPDG::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;
    mFrameCount = 0u;

    mpRTXDI.reset();
    mpEmissiveLightSampler.reset();

    mTracePhotonDifferentialsPass = RaytracingProgramHelper ::create();
    mGenerateInitialSamplesPass = RaytracingProgramHelper::create();

    mpResamplePhotonDifferentialsReservoirFGPass.reset();
    mpResamplePhotonDifferentialsReservoirCausticPass.reset();
    mpEvaluatePhotonDifferentialsReservoirsPass.reset();

    if (mpScene && mpScene->hasGeometryType(Scene::GeometryType::Custom))
    {
        logWarning("This render pass only supports triangles. Other types of geometry will be ignored.");
        mRecompile = true;
    }

    setupLights(pRenderContext);
}

void ReSTIR_FPDG::beginFrame(RenderContext* pRenderContext, const RenderData& renderData)
{
    //setupLights(pRenderContext);
    updatePrograms();
    setupResources(pRenderContext, renderData);
}

void ReSTIR_FPDG::endFrame()
{

}

void ReSTIR_FPDG::update()
{

}

void ReSTIR_FPDG::updatePrograms()
{
    if (!mRecompile) return;

    mRecompile = false;
}

void ReSTIR_FPDG::setupLights(RenderContext* pRenderContext)
{
    auto& lightCollection = mpScene->getLightCollection(pRenderContext);
    lightCollection->prepareSyncCPUData(pRenderContext);

    bool emissiveLightsUsed = mpScene->useEmissiveLights();
    bool analyticLightsUsed = mpScene->useAnalyticLights();

    mHasEnvBackground = mpScene->useEnvBackground();
    mHasAnalyticLights = analyticLightsUsed;
    mHasEmissiveLights = emissiveLightsUsed;
    mHasMixedLights = analyticLightsUsed && emissiveLightsUsed;
    mHasLights = emissiveLightsUsed || analyticLightsUsed;

    if (!mHasLights)
    {
        logError("Scene has no lights. Pass will not be executed.");
    }

    if (emissiveLightsUsed)
    {
        if (!mpEmissiveLightSampler)
        {
            switch (mEmissiveSamplerType)
            {
            case EmissiveLightSamplerType::Uniform:
                mpEmissiveLightSampler = std::make_unique<EmissiveUniformSampler>(pRenderContext, lightCollection);
                break;
            case EmissiveLightSamplerType::LightBVH:
                mpEmissiveLightSampler = std::make_unique<LightBVHSampler>(pRenderContext, lightCollection);
                break;
            case EmissiveLightSamplerType::Power:
                mpEmissiveLightSampler = std::make_unique<EmissivePowerSampler>(pRenderContext, lightCollection);
                break;
            default:
                logError("Unknown emissive light sampler type!");
            }
        }
    }
    else
    {
        if (mpEmissiveLightSampler)
        {
            mpEmissiveLightSampler = nullptr;

            // TO DO: reset programVars of light sample rt shader
        }
    }

    if (mpEmissiveLightSampler)
    {
        mpEmissiveLightSampler->update(pRenderContext, lightCollection);
    }

    mRecompile = true;
}

void ReSTIR_FPDG::setupResources(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto& screenDims = renderData.getDefaultTextureDims();
    if (mScreenRes.x != screenDims.x || mScreenRes.y != screenDims.y)
    {
        mScreenRes = screenDims;
        mResetScreenRes = true;
    }

    if (mChangePhotonLightBufferSize)
    {
        mNumMaxPhotons = mNumMaxPhotonsUI; 
        for (uint i = 0; i < 2; i++)
        {
            mpPhotonAABB[i].reset();
            mpPhotonData[i].reset();
        }
    }

    for (uint i = 0u; i < 2; i++)
    {
        if (!mpPhotonAABB[i])
        {
            mpPhotonAABB[i] = mpDevice->createStructuredBuffer(sizeof(AABB), mNumMaxPhotons[i], ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
                MemoryType::DeviceLocal, nullptr, false);
            mpPhotonAABB[i]->setName("PhotonAABB" + std::to_string(i));
        }

        if (!mpPhotonData[i])
        {
            mpPhotonData[i] = mpDevice->createStructuredBuffer(48u /*See the photonData struct in shader*/,
                mNumMaxPhotons[i], ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess, MemoryType::DeviceLocal, nullptr, false);
            mpPhotonData[i]->setName("PhotonData" + std::to_string(i));
        }

        if (!mpCausticReservoir[i] || mResetScreenRes)
        {
            mCanResample = false;
            mpCausticReservoir[i] = mpDevice->createStructuredBuffer(
                112u, mScreenRes.x * mScreenRes.y, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess, MemoryType::DeviceLocal, nullptr, false);
            mpCausticReservoir[i]->setName("CausticReservoir" + std::to_string(i));
        }

        if (!mpFinalGatherReservoir[i] || mResetScreenRes)
        {
            mCanResample = false;
            mpFinalGatherReservoir[i] = mpDevice->createStructuredBuffer(
                112u, mScreenRes.x * mScreenRes.y, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess, MemoryType::DeviceLocal, nullptr, false);
            mpFinalGatherReservoir[i]->setName("FinalGatherReservoir" + std::to_string(i));
        }
    }

    // Photon counters
    if (!mpPhotonCounter)
    {
        mpPhotonCounter = mpDevice->createStructuredBuffer(sizeof(uint), 2u, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess,
            MemoryType::DeviceLocal, nullptr, false);
        mpPhotonCounter->setName("PhotonCounter");
    }

    if (!mpPhotonCounterCPU)
    {
        mpPhotonCounterCPU = mpDevice->createStructuredBuffer(sizeof(uint), 2u, ResourceBindFlags::None, MemoryType::ReadBack, nullptr, false);
        mpPhotonCounterCPU->setName("PhotonCounterCPU");
    }

    if (!mpEmission || mResetScreenRes)
    {
        mpEmission = mpDevice->createTexture2D(mScreenRes.x, mScreenRes.y, ResourceFormat::RGBA32Float, 1u, 1u, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        mpEmission->setName("EmissionTexture");
    }

    mResetScreenRes = false;
}

void ReSTIR_FPDG::tracePhotonDifferentialsPass(RenderContext* pRenderContext, const RenderData& renderData, bool analyticOnly, bool buildAS)
{
    FALCOR_PROFILE(pRenderContext, "TracePhotonDifferentials");

    TypeConformanceList sceneTypeConformances = mpScene->getTypeConformances();

    if (!mTracePhotonDifferentialsPass.pProgram)
    {
        DefineList globalDefines;
        globalDefines.add("USE_EMISSIVE_LIGHT", mHasEmissiveLights ? "1" : "0");
        globalDefines.add(mpScene->getSceneDefines());
        globalDefines.add("PHOTON_BUFFER_SIZE_GLOBAL", std::to_string(mNumMaxPhotons[0]));
        globalDefines.add("PHOTON_BUFFER_SIZE_CAUSTIC", std::to_string(mNumMaxPhotons[1]));
        globalDefines.add("ROUGHNESS_THRESHOLD", std::to_string(mSpecularRoughnessThreshold));
        globalDefines.add(getMaterialDefines());
        if (mpEmissiveLightSampler)
            globalDefines.add(mpEmissiveLightSampler->getDefines());

        ProgramDesc desc;
        desc.addShaderModules(mpScene->getShaderModules())
            .addShaderLibrary(kShaderTracePhotons)
            .setMaxPayloadSize(sizeof(float) * 4) // PackedHitInfo is uint4 (16 byte)
            .setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize())
            .setMaxTraceRecursionDepth(1u);
        if (mpScene->hasProceduralGeometry())
            desc.setRtPipelineFlags(RtPipelineFlags::SkipProceduralPrimitives);

        mTracePhotonDifferentialsPass.pBindingTable = RtBindingTable::create(1u, 1u, mpScene->getGeometryCount());

        // Specify entry point for raygen and miss shaders.
        // The raygen shader needs type conformances for *all* materials in the scene.
        mTracePhotonDifferentialsPass.pBindingTable->setRayGen(desc.addRayGen("rayGen", sceneTypeConformances));
        // The miss shader doesn't need type conformances as it doesn't access any materials.
        mTracePhotonDifferentialsPass.pBindingTable->setMiss(0u, desc.addMiss("miss"));
        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            mTracePhotonDifferentialsPass.pBindingTable->setHitGroup(
                0u, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit")
            );
        }

        mTracePhotonDifferentialsPass.pProgram = Program::create(mpDevice, desc, globalDefines);
    }

    if (!mTracePhotonDifferentialsPass.pVars)
        mTracePhotonDifferentialsPass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);

    FALCOR_ASSERT(mTracePhotonDifferentialsPass.pVars);
    auto var = mTracePhotonDifferentialsPass.pVars->getRootVar();

    if (mpEmissiveLightSampler)
        mpEmissiveLightSampler->bindShaderData(var["Light"]["gEmissiveSampler"]);
    mpScene->bindShaderDataForRaytracing(pRenderContext, var["gScene"]);

    // Handle shader dimension
    uint dispatchedPhotons = mNumDispatchedPhotons;
    if (mHasMixedLights)
    {
        float dispatchedF = float(dispatchedPhotons);
        dispatchedF *= analyticOnly ? mPhotonAnalyticRatio : 1.f - mPhotonAnalyticRatio;
        dispatchedPhotons = uint(dispatchedF);
    }
    uint shaderDispatchDim = static_cast<uint>(std::floor(sqrt(dispatchedPhotons)));
    shaderDispatchDim = std::max(32u, shaderDispatchDim);

    // CB
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gMaxBounces"] = mPhotonMaxBounces;
    var["CB"]["gPhotonRadius"] = mPhotonRadius;
    var["CB"]["gDispatchDimension"] = shaderDispatchDim;
    var["CB"]["gGlobalRejectionProb"] = mGlobalPhotonRejection;
    var["CB"]["gUseAnalyticLights"] = analyticOnly;   

    uint3 dispatchDims(shaderDispatchDim, shaderDispatchDim, 1u);
    mpScene->raytrace(pRenderContext, mTracePhotonDifferentialsPass.pProgram.get(), mTracePhotonDifferentialsPass.pVars, dispatchDims);
}

void ReSTIR_FPDG::handlePhotonCounter(RenderContext* pRenderContext)
{
    // Try this with raw d3d12 in your framework
    pRenderContext->copyBufferRegion(mpPhotonCounterCPU.get(), 0u, mpPhotonCounter.get(), (uint64_t)0u, (uint64_t)sizeof(uint2));

    void* data = mpPhotonCounterCPU->map();
    std::memcpy(&mCurrentPhotonCount, data, sizeof(uint2));
    mpPhotonCounterCPU->unmap();

    if (mUseDynamicPhotonDispatchCount)
    {
        // Only use global photons for the dynamic dispatch count
        uint globalPhotonCount = mCurrentPhotonCount[0];
        uint globalMaxPhotons = mNumMaxPhotons[0];
        uint causticPhotonCount = mCurrentPhotonCount[1];
        uint causticMaxPhotons = mNumMaxPhotons[1];
        // If counter is invalid, reset
        if (globalPhotonCount == 0u)
        {
            mNumDispatchedPhotons = mPhotonDynamicDispatchMax * 0.5f;
        }
        uint globBufferSizeCompValue = (uint)(globalMaxPhotons * (1.f - mPhotonDynamicGuardPercentage));        // 92% of max global photons
        uint causticBufferSizeCompValue = (uint)(causticMaxPhotons * (1.f - mPhotonDynamicGuardPercentage));    // 92% of max caustic photons
        uint globChangeSize = (uint)(globalMaxPhotons * mPhotonDynamicChangePercentage);                        // 4% of current photons count
        uint causticChangeSize = (uint)(causticMaxPhotons * mPhotonDynamicChangePercentage);                    // 4% of current caustic photons count
        uint changeSize = std::max(globChangeSize, causticChangeSize);

        // If smaller, increase dispatch size
        if ((globalPhotonCount < globBufferSizeCompValue) && (causticPhotonCount < causticBufferSizeCompValue))
        {
            uint newDispatched = (uint)(mNumDispatchedPhotons + changeSize);
            mNumDispatchedPhotons = std::min(newDispatched, mPhotonDynamicDispatchMax);
        }
        // Reduce dispatch size
        else
        {
            uint newDispatched = (uint)(mNumDispatchedPhotons - changeSize);
            mNumDispatchedPhotons = std::max(newDispatched, 1024u);
        }
    }
}

// Generates the initial Samples for the reservoirs (FG) and initializes RTXDI surfaces
void ReSTIR_FPDG::generateInitialSamplesPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "GenerateInitialSamples");

    if (!mGenerateInitialSamplesPass.pProgram)
    {
        TypeConformanceList sceneTypeConformances = mpScene->getTypeConformances();

        DefineList globalDefines;
        globalDefines.add("USE_EMISSIVE_LIGHT", mHasEmissiveLights ? "1" : "0");
        globalDefines.add(mpScene->getSceneDefines());
        globalDefines.add("ROUGHNESS_THRESHOLD", std::to_string(mSpecularRoughnessThreshold));
        globalDefines.add(mpRTXDI->getDefines());
        globalDefines.add(getMaterialDefines());

        ProgramDesc desc;
        desc.addShaderModules(mpScene->getShaderModules())
            .addShaderLibrary(kShaderGenInitialSamples)
            .addTypeConformances(mpScene->getTypeConformances())
            .setMaxPayloadSize(sizeof(float) * 4) // PackedHitInfo is uint4 (16 byte)
            .setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize())
            .setMaxTraceRecursionDepth(1u);
        if (mpScene->hasProceduralGeometry())
            desc.setRtPipelineFlags(RtPipelineFlags::SkipProceduralPrimitives);

        mGenerateInitialSamplesPass.pBindingTable = RtBindingTable::create(1u, 1u, mpScene->getGeometryCount());
        mGenerateInitialSamplesPass.pBindingTable->setRayGen(desc.addRayGen("rayGen", sceneTypeConformances));
        mGenerateInitialSamplesPass.pBindingTable->setMiss(0u, desc.addMiss("miss"));
        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            mGenerateInitialSamplesPass.pBindingTable->setHitGroup(
                0u, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit")
            );
        }

        mGenerateInitialSamplesPass.pProgram = Program::create(mpDevice, desc, globalDefines);
    }

    if (!mGenerateInitialSamplesPass.pVars)
        mGenerateInitialSamplesPass.initProgramVars(mpDevice, mpScene, mpSampleGenerator);

    FALCOR_ASSERT(mGenerateInitialSamplesPass.pVars);
    auto var = mGenerateInitialSamplesPass.pVars->getRootVar();

    // Set RTXDI resources
    mpRTXDI->bindShaderData(var);

    // CB
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gFGRayMaxPathLength"] = mFGRayMaxPathLength;

    uint3 dispatchDims(mScreenRes.x, mScreenRes.y, 1u);
    mpScene->raytrace(pRenderContext, mGenerateInitialSamplesPass.pProgram.get(), mGenerateInitialSamplesPass.pVars, dispatchDims);
}

// Reservoir Resampling for Final Gather Samples
void ReSTIR_FPDG::resampleReservoirFGPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "ResampleReservoirFG");

    if (!mpResamplePhotonDifferentialsReservoirFGPass)
    {
        DefineList globalDefines;
        globalDefines.add(mpScene->getSceneDefines());
        globalDefines.add(mpSampleGenerator->getDefines());
        globalDefines.add(getMaterialDefines());
        globalDefines.add(mpRTXDI->getDefines());

        ProgramDesc desc;
        desc.addShaderModules(mpScene->getShaderModules())
            .addTypeConformances(mpScene->getTypeConformances())
            .addShaderLibrary(kShaderResamplingReservoirFG).csEntry("main").setShaderModel(ShaderModel::SM6_5);

        mpResamplePhotonDifferentialsReservoirFGPass = ComputePass::create(mpDevice, desc, globalDefines, true);
    }
    FALCOR_ASSERT(mpResamplePhotonDifferentialsReservoirFGPass);

    if (!mCanResample || !mResampleSettingsFG.enable) return;

    // Set variables
    auto var = mpResamplePhotonDifferentialsReservoirFGPass->getRootVar();

    mpScene->bindShaderDataForRaytracing(pRenderContext, var["gScene"]);
    mpSampleGenerator->bindShaderData(var);
}

// Reservoir Resampling for Caustic Samples
void ReSTIR_FPDG::resampleReservoirCausticPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "ResampleReservoirCaustic");

    if (!mpResamplePhotonDifferentialsReservoirCausticPass)    
    {
        DefineList globalDefines;
        globalDefines.add(mpScene->getSceneDefines());
        globalDefines.add(mpSampleGenerator->getDefines());
        globalDefines.add(getMaterialDefines());
        globalDefines.add(mpRTXDI->getDefines());

        ProgramDesc desc;
        desc.addShaderModules(mpScene->getShaderModules())
            .addTypeConformances(mpScene->getTypeConformances())
            .addShaderLibrary(kShaderResamplingReservoirCaustic).csEntry("main").setShaderModel(ShaderModel::SM6_5);

        mpResamplePhotonDifferentialsReservoirCausticPass = ComputePass::create(mpDevice, desc, globalDefines, true);
    }

    FALCOR_ASSERT(mpResamplePhotonDifferentialsReservoirCausticPass);

    if (!mCanResample || !mResampleSettingsCaustic.enable) return;

    // Set variables
    auto var = mpResamplePhotonDifferentialsReservoirCausticPass->getRootVar();

    mpScene->bindShaderDataForRaytracing(pRenderContext, var["gScene"]);
    mpSampleGenerator->bindShaderData(var);

}

// Evaluate Reservoirs
void ReSTIR_FPDG::evaluateReservoirsPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "EvaluateReservoirs");

    if (!mpEvaluatePhotonDifferentialsReservoirsPass)
    {
        DefineList globalDefines;
        globalDefines.add(mpScene->getSceneDefines());
        globalDefines.add(mpSampleGenerator->getDefines());
        globalDefines.add(getMaterialDefines());
        globalDefines.add(mpRTXDI->getDefines());
        globalDefines.add("USE_ENV_BACKROUND", mHasEnvBackground ? "1" : "0");

        ProgramDesc desc;
        desc.addShaderModules(mpScene->getShaderModules())
            .addTypeConformances(mpScene->getTypeConformances())
            .addShaderLibrary(kShaderEvaluateReservoirs).csEntry("main").setShaderModel(ShaderModel::SM6_5);

        mpEvaluatePhotonDifferentialsReservoirsPass = ComputePass::create(mpDevice, desc, globalDefines, true);
    }
    FALCOR_ASSERT(mpEvaluatePhotonDifferentialsReservoirsPass);
}

DefineList ReSTIR_FPDG::getMaterialDefines()
{
    DefineList defines;
    defines.add("DiffuseBrdf", mUseLambertianDiffuse ? "DiffuseBrdfLambert" : "DiffuseBrdfFrostbite");
    defines.add("enableDiffuse", "1");
    defines.add("enableSpecular", "1");
    defines.add("enableTranslucency", "1");
    return defines;
}

void ReSTIR_FPDG::RaytracingProgramHelper::initProgramVars(ref<Device>& pDevice, ref<Scene>& pScene, ref<SampleGenerator>& pSampleGenerator)
{
    FALCOR_ASSERT(pProgram);

    pProgram->addDefines(pSampleGenerator->getDefines());
    pProgram->setTypeConformances(pScene->getTypeConformances()); //?

    pVars = RtProgramVars::create(pDevice, pProgram, pBindingTable);
}
