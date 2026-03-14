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

    setupResources(pRenderContext, renderData);

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
    }

    setupLights(pRenderContext);

    if (!mHasLights)
    {
        logError("Scene has no lights. Pass will not be executed.");
    }
}

void ReSTIR_FPDG::setupRaytracingPrograms()
{

}

void ReSTIR_FPDG::setupLights(RenderContext* pRenderContext)
{
    auto& lightCollection = mpScene->getLightCollection(pRenderContext);
    lightCollection->prepareSyncCPUData(pRenderContext);

    bool emissiveLightsUsed = mpScene->useEmissiveLights();
    bool analyticLightsUsed = mpScene->useAnalyticLights();

    mHasAnalyticLights = analyticLightsUsed;
    mHasEmissiveLights = emissiveLightsUsed;
    mHasMixedLights = analyticLightsUsed && emissiveLightsUsed;
    mHasLights = emissiveLightsUsed || analyticLightsUsed;

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
}

void ReSTIR_FPDG::setupResources(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto& screenDims = renderData.getDefaultTextureDims();
    if (mScreenRes.x != screenDims.x || mScreenRes.y != screenDims.y)
    {
        mScreenRes = screenDims;
        mResetScreenRes = true;
        // reset all screen resolution dependent resources

    }

    mResetScreenRes = false;
}

void ReSTIR_FPDG::RaytracingProgramHelper::initProgramVars(ref<Device>& pDevice, ref<Scene>& pScene, ref<SampleGenerator>& pSampleGenerator)
{
    FALCOR_ASSERT(pProgram);

    pProgram->addDefines(pSampleGenerator->getDefines());
    pProgram->addDefines(pScene->getSceneDefines());
    pProgram->setTypeConformances(pScene->getTypeConformances());

    pVars = RtProgramVars::create(pDevice, pProgram, pBindingTable);
}
