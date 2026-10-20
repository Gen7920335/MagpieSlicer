# The resin support mod (mod.json): PrusaSlicer 2.9.6's SLA support points and support trees.
#
# Unlike the other official mods it is built against the core's library: the SLA code (prusa/, PrusaSlicer
# 2.9.6 as line 1 ported it) works on the core's geometry types (TriangleMesh, AABBMesh, ExPolygon,
# ClipperUtils, Eigen), so the mod links libslic3r statically and carries its own copy of what it uses. It
# still talks to the slicer only through the C slots; the copy is compiled with libslic3r's own include
# directories, definitions and options, so its numbers equal those the same code gave inside the core.
#
# The core keeps OrcaSlicer's own SLA library (namespace Slic3r::sla). This mod's copy is compiled with
# sla=resin_sla, so its symbols cannot collide with the core's inside this library: the two differ, and
# the linker would otherwise merge same-named inline functions of both.
set(_prusa ${CMAKE_CURRENT_SOURCE_DIR}/prusa/libslic3r)
set(MAGPIE_RESIN_SUPPORT_SOURCES
    ${_prusa}/BranchingTree/BranchingTree.cpp
    ${_prusa}/BranchingTree/PointCloud.cpp
    ${_prusa}/MeshNormals.cpp
    ${_prusa}/SLA/BranchingTreeSLA.cpp
    ${_prusa}/SLA/Clustering.cpp
    ${_prusa}/SLA/ConcaveHull.cpp
    ${_prusa}/SLA/DefaultSupportTree.cpp
    ${_prusa}/SLA/IndexedMesh.cpp
    ${_prusa}/SLA/Pad.cpp
    ${_prusa}/SLA/SpatIndex.cpp
    ${_prusa}/SLA/SupportPointGenerator.cpp
    ${_prusa}/SLA/SupportTree.cpp
    ${_prusa}/SLA/SupportTreeBuilder.cpp
    ${_prusa}/SLA/SupportTreeMesher.cpp
    ${_prusa}/SLA/SupportTreeSlicer.cpp
    ${_prusa}/SLA/SupportIslands/EvaluateNeighbor.cpp
    ${_prusa}/SLA/SupportIslands/ExpandNeighbor.cpp
    ${_prusa}/SLA/SupportIslands/LineUtils.cpp
    ${_prusa}/SLA/SupportIslands/ParabolaUtils.cpp
    ${_prusa}/SLA/SupportIslands/PointUtils.cpp
    ${_prusa}/SLA/SupportIslands/PolygonUtils.cpp
    ${_prusa}/SLA/SupportIslands/PostProcessNeighbor.cpp
    ${_prusa}/SLA/SupportIslands/PostProcessNeighbors.cpp
    ${_prusa}/SLA/SupportIslands/SampleConfigFactory.cpp
    ${_prusa}/SLA/SupportIslands/SupportIslandPoint.cpp
    ${_prusa}/SLA/SupportIslands/UniformSupportIsland.cpp
    ${_prusa}/SLA/SupportIslands/VoronoiDiagramCGAL.cpp
    ${_prusa}/SLA/SupportIslands/VoronoiGraphUtils.cpp
    CACHE INTERNAL "The PrusaSlicer SLA sources of the resin support mod")
set(MAGPIE_RESIN_SUPPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")

# magpie_resin_sla_settings(<target>): what a target compiling the mod's SLA copy needs, the mod and its
# unit tests alike.
function(magpie_resin_sla_settings target)
    target_include_directories(${target} BEFORE PRIVATE ${MAGPIE_RESIN_SUPPORT_DIR}/prusa)
    target_include_directories(${target} PRIVATE $<TARGET_PROPERTY:libslic3r,INCLUDE_DIRECTORIES>)
    target_compile_definitions(${target} PRIVATE $<TARGET_PROPERTY:libslic3r,COMPILE_DEFINITIONS> sla=resin_sla)
    target_compile_options(${target} PRIVATE $<TARGET_PROPERTY:libslic3r,COMPILE_OPTIONS>)
    target_link_libraries(${target} PRIVATE libslic3r libslic3r_cgal boost_libs TBB::tbb)
    if (MSVC)
        target_compile_options(${target} PRIVATE /bigobj)
    endif ()
endfunction()

magpie_official_mod(resin_support SOURCES resin_support.cpp ${MAGPIE_RESIN_SUPPORT_SOURCES})
# The SDK headers stay first-class (magpie_add_mod set them); the core's come after the mod's own copy.
target_include_directories(resin_support PRIVATE ${MAGPIE_MOD_SDK_DIR}/include)
magpie_resin_sla_settings(resin_support)
