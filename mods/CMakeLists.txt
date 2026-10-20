# MAGPIE: native mods built in this repository. Each is its own target built with the SDK template only
# (mods/sdk/MagpieMod.cmake): none links against or includes the slicer core.

include(${CMAKE_CURRENT_SOURCE_DIR}/sdk/MagpieMod.cmake)

# The official mods, bundled with the application. Each target adds itself to MAGPIE_OFFICIAL_MOD_TARGETS,
# which the application and the tests that load it depend on.
set(MAGPIE_OFFICIAL_MOD_TARGETS "" CACHE INTERNAL "Targets of the bundled native mods")
add_subdirectory(layer_time_blend)

if (BUILD_TESTS)
    # Test mods for tests/libslic3r/test_native_mods.cpp, as package folders <id>/mod.json + bin/<platform>/.
    set(MAGPIE_TEST_MODS_DIR "${CMAKE_BINARY_DIR}/test_mods" CACHE INTERNAL "Package folders of the test mods")
    set(MAGPIE_TEST_MOD_TARGETS "" CACHE INTERNAL "")
    function(magpie_test_mod id)
        magpie_add_mod(${id} SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_mod/test_mod.cpp
                       OUTPUT_DIR "${MAGPIE_TEST_MODS_DIR}/${id}"
                       DEFINITIONS MAGPIE_TEST_MOD_ID="${id}" ${ARGN})
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test_mod/${id}.json "${MAGPIE_TEST_MODS_DIR}/${id}/mod.json" COPYONLY)
        set(MAGPIE_TEST_MOD_TARGETS ${MAGPIE_TEST_MOD_TARGETS} ${id} CACHE INTERNAL "")
    endfunction()
    magpie_test_mod(magpie_test_mod)
    magpie_test_mod(magpie_test_mod_api2 MAGPIE_TEST_MOD_FAKE_MAJOR=1)
    magpie_test_mod(magpie_test_mod_fast_fp MAGPIE_TEST_MOD_FAKE_FP=1)
endif ()
