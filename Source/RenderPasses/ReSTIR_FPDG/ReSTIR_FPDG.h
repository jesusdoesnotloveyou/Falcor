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
#pragma once
#include "Falcor.h"

#include "ReSTIR_FPDG.h"
#include "RenderGraph/RenderPass.h"
#include "RenderGraph/RenderPassHelpers.h"

#include "Rendering/RTXDI/RTXDI.h"
#include "Rendering/Lights/EmissiveLightSampler.h"

using namespace Falcor;

class ReSTIR_FPDG : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(ReSTIR_FPDG, "ReSTIR_FPDG", "Insert pass description here.");

    static ref<ReSTIR_FPDG> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<ReSTIR_FPDG>(pDevice, props);
    }

    ReSTIR_FPDG(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

private:
    void setupRaytracingPrograms();

    void setupLights(RenderContext* pRenderContext);
    void setupResources(RenderContext* pRenderContext, const RenderData& renderData);

private:

    enum class DirectLightingMode
    {
        None = 0u,
        RTXDI = 1u,
        AnalyticDirect = 2u,
    };

    enum class ResamplingMode
    {
        Temporal = 0u,
        Spatial = 1u,
        SpatioTemporal = 2u,
    };

    enum class RenderMode
    {
        FinalGather = 0,
        ReSTIRFG = 1u,
        ReSTIRGI = 2u
    };

    //
    // Parameters
    //

    RenderMode mRenderMode = RenderMode::ReSTIRFG;
    DirectLightingMode mDirectLightMode = DirectLightingMode::RTXDI;
    ResamplingMode mResamplingMode = ResamplingMode::SpatioTemporal;

    uint mFrameCount = 0u;
    uint2 mScreenRes = uint2(0u, 0u);

    ref<Scene> mpScene;                                             // Scene ptr
    ref<SampleGenerator> mpSampleGenerator;                         // GPU Sample Gen
    std::unique_ptr<EmissiveLightSampler> mpEmissiveLightSampler;   // Light sampler
    EmissiveLightSamplerType mEmissiveSamplerType = EmissiveLightSamplerType::Power; 

    std::unique_ptr<RTXDI> mpRTXDI;                     // Ptr to RTXDI for direct use
    RTXDI::Options mRTXDIOptions;                       // Options for RTXDI

    bool mResetScreenRes = false;
    bool mCanResample = false;                           // Resampling is only allowed if last iterations reservoir was created

    //
    // Lights
    //

    bool mHasLights = false;
    bool mHasEmissiveLights = false;
    bool mHasAnalyticLights = false;
    bool mHasMixedLights = false;

    //
    // Resources
    //
    ref<Buffer> mpPhotonAABB[2];           // Photon AABBs for Acceleration Structure building
    ref<Buffer> mpPhotonData[2];           // Additional Photon data (flux, dir)
    ref<Buffer> mpPhotonCounter;           // Photon Counter
    ref<Buffer> mpPhotonCounterCPU;        // CPU copy of counter for readback
    ref<Buffer> mpFinalGatherReservoir[2]; // Reservoir for the Final Gather sample
    ref<Buffer> mpCausticReservoir[2];     // Reservoir for the Caustic sample
    ref<Texture> mpEmission;               // Emission for paths that travel through highly specular materials

    struct RaytracingProgramHelper
    {
        ref<Program> pProgram = nullptr;
        ref<RtBindingTable> pBindingTable = nullptr;
        ref<RtProgramVars> pVars = nullptr;

        static const RaytracingProgramHelper create()
        {
            RaytracingProgramHelper r;
            r.pProgram = nullptr;
            r.pBindingTable = nullptr;
            r.pVars = nullptr;
            return r;
        }

        void initProgramVars(ref<Device>& pDevice, ref<Scene>& pScene, ref<SampleGenerator>& pSampleGenerator);
    };

    RaytracingProgramHelper mTracePhotonDifferentialsPass;
    RaytracingProgramHelper mGenerateInitialSamplesPass;

    ref<ComputePass> mpResamplePhotonDifferentialsReservoirFGPass;
    ref<ComputePass> mpResamplePhotonDifferentialsReservoirCausticPass;
    ref<ComputePass> mpEvaluatePhotonDifferentialsReservoirsPass;
};
