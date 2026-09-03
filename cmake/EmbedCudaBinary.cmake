file(READ "${INPUT}" binary HEX)
string(REGEX REPLACE "(..)" "0x\\1," binary "${binary}")
file(WRITE "${OUTPUT}" "// Generated CUDA fatbinary.\nalignas(8) static const unsigned char magpie_cuda_binary[] = {${binary}};\n")
