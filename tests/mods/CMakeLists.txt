# MAGPIE: unit tests of the bundled native mods. Each executable compiles its mod's own sources with
# Catch2 and nothing of the slicer core, so it tests the mod as the mod sees itself; the core's side of
# a slot is tested through the built library (tests/libslic3r, tests/fff_print).
set(_ltb ${CMAKE_SOURCE_DIR}/mods/layer_time_blend)
add_executable(mod_layer_time_blend_tests ${_ltb}/unit/test_planner.cpp ${_ltb}/planner.cpp)
set_property(TARGET mod_layer_time_blend_tests PROPERTY INCLUDE_DIRECTORIES "${_ltb}")
target_link_libraries(mod_layer_time_blend_tests Catch2::Catch2WithMain)
set_property(TARGET mod_layer_time_blend_tests PROPERTY FOLDER "tests")
orcaslicer_discover_tests(mod_layer_time_blend_tests)

# The resin support mod's copy of PrusaSlicer's SLA library (mods/resin_support/prusa): PrusaSlicer 2.9.6's
# support-point, island, tree and pad tests, compiled the way the mod compiles that copy. Unlike the test
# above it links the core's library, as the mod itself does (mods/resin_support/CMakeLists.txt).
set(_resin ${MAGPIE_RESIN_SUPPORT_DIR}/unit)
add_executable(mod_resin_support_tests
    ${_resin}/sla_tests_main.cpp
    ${_resin}/sla_print_tests.cpp
    ${_resin}/sla_supptgen_tests.cpp
    ${_resin}/sla_test_utils.cpp
    ${_resin}/sla_test_utils.hpp
    # The raster and hollowing tests of PrusaSlicer's suite run on the copy's raster and hollowing too.
    ${MAGPIE_RESIN_SUPPORT_DIR}/prusa/libslic3r/SLA/RasterBase.cpp
    ${MAGPIE_RESIN_SUPPORT_DIR}/prusa/libslic3r/SLA/Hollowing.cpp
    ${MAGPIE_RESIN_SUPPORT_SOURCES})
magpie_resin_sla_settings(mod_resin_support_tests)
file(TO_NATIVE_PATH "${_resin}/data" _resin_data)
target_compile_definitions(mod_resin_support_tests PRIVATE RESIN_UNIT_DATA_DIR=R"\(${_resin_data}\)")
target_link_libraries(mod_resin_support_tests PRIVATE test_common Catch2::Catch2WithMain)
set_property(TARGET mod_resin_support_tests PROPERTY FOLDER "tests")
set(_TEST_NAME mod_resin_support)
orcaslicer_copy_test_dlls()
orcaslicer_discover_tests(mod_resin_support_tests)

