# A CPU-only first gate for the production schedule calculations. No slicer,
# GUI dependency is necessary for these interval tests.
add_executable(half_layer_plan_tests test_half_layer_plan.cpp)
target_include_directories(half_layer_plan_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(half_layer_plan_tests PRIVATE Catch2::Catch2WithMain)
set_property(TARGET half_layer_plan_tests PROPERTY FOLDER "tests")
orcaslicer_discover_tests(half_layer_plan_tests)

# Native geometry gate, without building the GUI or all integration suites.
add_executable(half_layer_geometry_tests test_half_layer_geometry.cpp test_runtime.cpp ../fff_print/test_helpers.cpp)
target_link_libraries(half_layer_geometry_tests PRIVATE test_common libslic3r Catch2::Catch2WithMain)
# MAGPIE: the multi-nozzle wall cases plan with the multi-nozzle walls mod.
magpie_use_official_mods(half_layer_geometry_tests)
set_property(TARGET half_layer_geometry_tests PROPERTY FOLDER "tests")
orcaslicer_copy_test_dlls()
orcaslicer_discover_tests(half_layer_geometry_tests)

add_executable(half_layer_source_tests test_half_layer_sources.cpp test_half_layer_assembly.cpp test_half_layer_travel.cpp test_runtime.cpp ../fff_print/test_helpers.cpp)
target_link_libraries(half_layer_source_tests PRIVATE test_common libslic3r Catch2::Catch2WithMain)
set_property(TARGET half_layer_source_tests PROPERTY FOLDER "tests")
magpie_use_official_mods(half_layer_source_tests)
orcaslicer_discover_tests(half_layer_source_tests)
