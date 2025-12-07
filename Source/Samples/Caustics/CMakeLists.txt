add_falcor_executable(Caustics)

target_sources(Caustics PRIVATE
    Caustics.cpp
    Caustics.h
)

target_copy_shaders(Caustics Samples/Caustics)

target_source_group(Caustics "Samples")
