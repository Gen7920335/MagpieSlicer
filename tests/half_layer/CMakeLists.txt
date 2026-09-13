# A CPU-only first gate for the production schedule calculations. No slicer,
# GUI, CUDA or Vulkan dependency is necessary for these interval tests.
add_executable(half_layer_plan_tests test_half_layer_plan.cpp)
target_include_directories(half_layer_plan_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(half_layer_plan_tests PRIVATE Catch2::Catch2WithMain)
set_property(TARGET half_layer_plan_tests PROPERTY FOLDER "tests")
orcaslicer_discover_tests(half_layer_plan_tests)

# Native geometry gate, without building the GUI or all integration suites.
add_executable(half_layer_geometry_tests test_half_layer_geometry.cpp)
target_link_libraries(half_layer_geometry_tests PRIVATE test_common libslic3r Catch2::Catch2WithMain)
if(NOT SLIC3R_ENABLE_CUDA_SLICER AND NOT SLIC3R_ENABLE_VULKAN_SLICER)
    target_compile_definitions(half_layer_geometry_tests PRIVATE MAGPIE_HALF_LAYER_CPU_GATE)
endif()
set_property(TARGET half_layer_geometry_tests PROPERTY FOLDER "tests")
orcaslicer_copy_test_dlls()
orcaslicer_discover_tests(half_layer_geometry_tests)

# Existing wall/flow regression cases, linked to the same CPU-only library.
# Keep the large unrelated FFF suites out of this implementation gate.
add_executable(half_layer_wall_regression_tests
    test_runtime.cpp
    ../fff_print/test_helpers.cpp
    ../fff_print/test_flow.cpp
    ../fff_print/test_multifilament.cpp)
target_link_libraries(half_layer_wall_regression_tests PRIVATE test_common libslic3r Catch2::Catch2WithMain)
set_property(TARGET half_layer_wall_regression_tests PROPERTY FOLDER "tests")
orcaslicer_discover_tests(half_layer_wall_regression_tests)

add_executable(half_layer_source_tests test_half_layer_sources.cpp test_half_layer_assembly.cpp test_half_layer_travel.cpp test_runtime.cpp ../fff_print/test_helpers.cpp)
target_link_libraries(half_layer_source_tests PRIVATE test_common libslic3r Catch2::Catch2WithMain)
set_property(TARGET half_layer_source_tests PROPERTY FOLDER "tests")
orcaslicer_discover_tests(half_layer_source_tests)
