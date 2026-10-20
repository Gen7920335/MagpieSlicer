# The layer time blending mod (slot gcode.time_plan). Built with the SDK template only. Its package folder
# is generated in <source>/resources/mods/layer_time_blend, where the application finds bundled mods (the
# build tree's resources link to the source folder); that folder is not tracked.
set(_package "${CMAKE_SOURCE_DIR}/resources/mods/layer_time_blend")
magpie_add_mod(layer_time_blend SOURCES layer_time_blend.cpp planner.cpp planner.hpp OUTPUT_DIR "${_package}")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/mod.json "${_package}/mod.json" COPYONLY)
# The --mod-test cases.
file(GLOB _cases CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*)
foreach (_case IN LISTS _cases)
    get_filename_component(_name "${_case}" NAME)
    configure_file("${_case}" "${_package}/tests/${_name}" COPYONLY)
endforeach ()
set(MAGPIE_OFFICIAL_MOD_TARGETS ${MAGPIE_OFFICIAL_MOD_TARGETS} layer_time_blend CACHE INTERNAL "")
