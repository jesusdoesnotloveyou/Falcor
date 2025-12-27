add_falcor_executable(Caustics)

target_sources(Caustics PRIVATE
    	Caustics.cpp
    	Caustics.h
    	AllocateTilePhoton.cs.hlsl
    	AnalyseTraceResult.cs.hlsl
    	Caustics.ps.hlsl
    	Caustics.rt.hlsl
    	Common.hlsl
    	Composite.ps.hlsl
    	CompositeRT.rt.hlsl
    	GenerateRayCountMipmap.cs.hlsl
    	GPass.ps.hlsl
    	PhotonGather.cs.hlsl
	    PhotonScatter.ps.hlsl
	    PhotonTrace.rt.hlsl
	    ResetDrawArgument.cs.hlsl
	    SmoothPhoton.cs.hlsl
	    SpacialFilter.cs.hlsl
	    TemporalFilter.cs.hlsl
	    UpdateRayDensity.cs.hlsl
)

target_copy_shaders(Caustics Samples/Caustics)

target_source_group(Caustics "Samples")
