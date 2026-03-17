#include "Utils/Math/MathConstants.slangh"

import Utils.Sampling.SampleGenerator;

// Struct for a photon differential. Could be packed tighter
struct PhotonDifferential {
    float3 flux;     // Incoming flux
    uint pathLenght; // Length of the photon path
    float3 dir;      // Incoming direction
    uint _pad0;
    float3 normalW; // Face normal of the photon surface
    uint _pad1;
    float3 dPdx; // Positional x derivative
    uint _pad2;
    float3 dPdy; // Positional y derivative
    uint _pad3;

    __init() {
        this.dir = float3(0);
        this.normalW = float3(0, 1, 0);
        this.flux = float3(0);
        this.pathLenght = 0;
        this.dPdx = float3(0);
        this.dPdy = float3(0);
        this._pad0 = 0;
        this._pad1 = 0;
        this._pad2 = 0;
        this._pad3 = 0;
    }
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
