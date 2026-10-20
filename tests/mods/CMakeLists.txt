# MAGPIE: unit tests of the bundled native mods. Each executable compiles its mod's own sources with
# Catch2 and nothing of the slicer core, so it tests the mod as the mod sees itself; the core's side of
# a slot is tested through the built library (tests/libslic3r, tests/fff_print).
set(_ltb ${CMAKE_SOURCE_DIR}/mods/layer_time_blend)
add_executable(mod_layer_time_blend_tests ${_ltb}/unit/test_planner.cpp ${_ltb}/planner.cpp)
set_property(TARGET mod_layer_time_blend_tests PROPERTY INCLUDE_DIRECTORIES "${_ltb}")
target_link_libraries(mod_layer_time_blend_tests Catch2::Catch2WithMain)
set_property(TARGET mod_layer_time_blend_tests PROPERTY FOLDER "tests")
orcaslicer_discover_tests(mod_layer_time_blend_tests)
