# The CUDA compute mod (mod.json). The kernels are PTX text the NVIDIA driver compiles at run time, so
# the build needs no CUDA toolkit; the text is embedded into the library.
set(_ptx "${CMAKE_CURRENT_SOURCE_DIR}/CudaKernels.ptx")
set(_ptx_header "${CMAKE_CURRENT_BINARY_DIR}/generated/CudaKernelsPtx.hpp")
add_custom_command(OUTPUT "${_ptx_header}"
    COMMAND "${CMAKE_COMMAND}" -DINPUT=${_ptx} -DOUTPUT=${_ptx_header} -DNAME=magpie_cuda_kernels_ptx
            -P "${MAGPIE_MOD_SDK_DIR}/EmbedText.cmake"
    DEPENDS "${_ptx}" "${MAGPIE_MOD_SDK_DIR}/EmbedText.cmake"
    VERBATIM)
magpie_official_mod(cuda_compute SOURCES cuda_compute.cpp "${_ptx_header}")
target_include_directories(cuda_compute PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
