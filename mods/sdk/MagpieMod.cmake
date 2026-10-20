# MagpieMod.cmake -- the build template for a native mod. A mod's CMakeLists.txt includes this file
# and calls magpie_add_mod(); it needs no slicer sources.
#
#   include(<sdk>/MagpieMod.cmake)
#   magpie_add_mod(my_mod SOURCES my_mod.cpp)
#
# What the template enforces:
#   - The include path holds the SDK headers only, so a mod cannot reach the core's own headers and
#     break on the next Orca merge. A mod that needs Clipper2 or Eigen links its own copy statically.
#   - The core's floating-point mode: precise, no contraction into FMA. A mod built with /fp:fast or
#     with FMA contraction gives different last digits from the same source, and the G-code differs;
#     the mod reports the mode in magpie_mod_query and the core refuses any mode but "precise".
#   - Hidden symbols except the three exports.
#   - Output name and folder: <OUTPUT_DIR>/bin/<platform>/<binary> when OUTPUT_DIR is given (the mod's
#     package folder), else the target's usual place.

get_filename_component(MAGPIE_MOD_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

function(magpie_mod_platform out_var)
    if (WIN32)
        set(_os win)
    elseif (APPLE)
        set(_os macos)
    else ()
        set(_os linux)
    endif ()
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|ARM64|aarch64)$")
        set(_arch arm64)
    else ()
        set(_arch x64)
    endif ()
    set(${out_var} "${_os}-${_arch}" PARENT_SCOPE)
endfunction()

# magpie_add_mod(<target> SOURCES <files...> [BINARY <name>] [OUTPUT_DIR <dir>] [DEFINITIONS <defs...>])
function(magpie_add_mod target)
    cmake_parse_arguments(MOD "" "BINARY;OUTPUT_DIR" "SOURCES;DEFINITIONS" ${ARGN})
    if (NOT MOD_BINARY)
        set(MOD_BINARY ${target})
    endif ()
    add_library(${target} MODULE ${MOD_SOURCES})
    # Replace, not append: a mod built inside the slicer tree would otherwise inherit the core's
    # include directories, and a core header it reached would compile today and break after a merge.
    set_property(TARGET ${target} PROPERTY INCLUDE_DIRECTORIES "${MAGPIE_MOD_SDK_DIR}/include")
    target_compile_definitions(${target} PRIVATE ${MOD_DEFINITIONS})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${MOD_BINARY}
        PREFIX ""
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    if (MSVC)
        # /fp:precise is MSVC's default and does not contract; say it, so a parent project's /fp:fast
        # cannot leak in. clang-cl contracts unless told not to.
        target_compile_options(${target} PRIVATE /fp:precise)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${target} PRIVATE -ffp-contract=off)
            target_compile_definitions(${target} PRIVATE MAGPIE_MOD_FP_CONTRACT_OFF=1)
        endif ()
    else ()
        target_compile_options(${target} PRIVATE -ffp-contract=off -fno-fast-math)
        target_compile_definitions(${target} PRIVATE MAGPIE_MOD_FP_CONTRACT_OFF=1)
    endif ()
    if (MOD_OUTPUT_DIR)
        magpie_mod_platform(_platform)
        foreach (_config IN ITEMS "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
            set_target_properties(${target} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY${_config} "${MOD_OUTPUT_DIR}/bin/${_platform}")
        endforeach ()
    endif ()
endfunction()
