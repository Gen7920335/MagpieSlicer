# MAGPIE: native mods built in this repository. Each is its own target built with the SDK template
# (mods/sdk/MagpieMod.cmake), which keeps the slicer core out of its reach; the one exception is the resin
# support mod, whose PrusaSlicer SLA code links the core's geometry library statically (its CMakeLists.txt).

include(${CMAKE_CURRENT_SOURCE_DIR}/sdk/MagpieMod.cmake)

# Official mods: each builds into a package folder <build>/official_mods/<id>/ (mod.json, bin/<platform>/, tests/),
# the layout an installed mod has. Signing (--mod-sign with the official key, kept off the repository)
# and installing into a data directory are done on the built folder. Tests load these folders directly.
set(MAGPIE_OFFICIAL_MODS_DIR "${CMAKE_BINARY_DIR}/official_mods" CACHE INTERNAL "Package folders of the official mods")
set(MAGPIE_OFFICIAL_MOD_TARGETS "" CACHE INTERNAL "")
# magpie_official_mod(<id> SOURCES <files...>): called from mods/<id>/CMakeLists.txt.
function(magpie_official_mod id)
    cmake_parse_arguments(MOD "" "" "SOURCES" ${ARGN})
    set(_dir "${MAGPIE_OFFICIAL_MODS_DIR}/${id}")
    magpie_add_mod(${id} SOURCES ${MOD_SOURCES} OUTPUT_DIR "${_dir}")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/mod.json "${_dir}/mod.json" COPYONLY)
    file(GLOB_RECURSE _tests LIST_DIRECTORIES false RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*")
    foreach (_test IN LISTS _tests)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/tests/${_test}" "${_dir}/tests/${_test}" COPYONLY)
    endforeach ()
    # Message catalogs of the texts the mod's code shows (i18n/<language>.json).
    file(GLOB _catalogs LIST_DIRECTORIES false RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/i18n" "${CMAKE_CURRENT_SOURCE_DIR}/i18n/*.json")
    foreach (_catalog IN LISTS _catalogs)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/i18n/${_catalog}" "${_dir}/i18n/${_catalog}" COPYONLY)
    endforeach ()
    set(MAGPIE_OFFICIAL_MOD_TARGETS ${MAGPIE_OFFICIAL_MOD_TARGETS} ${id} CACHE INTERNAL "")
endfunction()
add_subdirectory(cura_support)
add_subdirectory(half_layer)
add_subdirectory(layer_time_blend)
add_subdirectory(low_temp_calibration)
add_subdirectory(low_temp_interface)
add_subdirectory(mixed_support)
add_subdirectory(multi_nozzle_walls)
add_subdirectory(resin_support)
add_subdirectory(snapmaker_u1)
add_subdirectory(support_extras)

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
