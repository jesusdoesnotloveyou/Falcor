#include "Utils/Math/MathConstants.slangh"

import Utils.Sampling.SampleGenerator;

// Struct for a photon differential 64B.
// Could be packed tighter
struct PhotonDifferential {
    float3 flux;     // Incoming flux
    uint pathLength; // Length of the photon path
    float3 dir;      // Incoming direction
    float normalX;
    float3 dPdx; // Positional x derivative
    float normalY;
    float3 dPdy; // Positional y derivative
    float normalZ;

    __init() {
        this.dir = float3(0);
        normalX = 0.0f;
        normalY = 1.0f;
        normalZ = 0.0f;
        this.flux = float3(0);
        this.pathLength = 0;
        this.dPdx = float3(0);
        this.dPdy = float3(0);
    }

    // Normal helper. Face normal of the photon surface
    property float3 normalW {
        get { return float3(normalX, normalY, normalZ); }
        set {
            normalX = newValue.x;
            normalY = newValue.y;
            normalZ = newValue.z;
        }
    }

    // Helper to check if this is a caustic photon (has differential data)
    // If sqDistance > 0.0f then Distance > 0.0f
    bool isCaustic() { return dot(dPdx, dPdx) > 0.0f || dot(dPdy, dPdy) > 0.0f; }
}

//
// Tracing Ray Differentials by Homan Igehy
//

/*  Update ray differentrial transfer
    \param[in] Current normal
    \param[in] Positional derivative
    \param[in] Directional derivative
    \param[in] World ray direction
    \param[in] Ray T current
*/
void updateTransferRayDifferential(float3 N, inout float3 dPdx, float3 dDdx, float3 D, float t)
{
    float dtdx = -1.0f * dot(dPdx + t * dDdx, N) / dot(D, N);
    dPdx = dPdx + t * dDdx + dtdx * D;
    // dDdx remains the same
}

/*  Update ray differentrial reflection
    \param[in] Current normal
    \param[in] Normal derivative
    \param[in] Directional derivative
    \param[in] World ray direction
*/
void updateReflectRayDifferential(float3 N, float3 dNdx, inout float3 dDdx, float3 D)
{
    // dPdx remains the same
    N = normalize(N);
    float dDNdx = dot(dDdx, N) + dot(D, dNdx);
    dDdx = dDdx - 2 * (dot(D, N) * dNdx + dDNdx * N);
}

/*  Update ray differentrial refraction
    \param[in] Direction
    \param[in] Refacted ray
    \param[in] Normal
    \param[in] Ratio of the incident index of refraction to thetransmitted index of refraction
    \param[in] Normal derivative
    \param[in] Directional derivative
*/
void updateRefractRayDifferential(float3 D, float3 R, float3 N, float eta, float3 dNdx, inout float3 dDdx)
{
    // dPdx remains the same
    N = normalize(N);
    float DN = dot(D, N);
    float RN = dot(R, N);
    float mu = eta * DN - RN;
    float dDNdx = dot(dDdx, N) + dot(D, dNdx);
    float dmudx = (eta - eta * eta * DN / RN) * dDNdx;
    dDdx = eta * dDdx - (mu * dNdx + dmudx * N);
}
