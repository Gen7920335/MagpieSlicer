# Writes INPUT as a NUL-terminated byte array named NAME into the header OUTPUT.
# Used for the CUDA kernels (src/libslic3r/Gpu/CudaKernels.ptx), which the driver compiles at run time.
file(READ "${INPUT}" content HEX)
string(REGEX REPLACE "(..)" "0x\\1," content "${content}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT}; do not edit.\n#pragma once\nstatic const char ${NAME}[] = {${content}0x00};\n")
