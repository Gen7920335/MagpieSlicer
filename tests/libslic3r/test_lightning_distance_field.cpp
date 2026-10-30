#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Fill/Lightning/DistanceField.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

namespace {

// The layer that stalled vanilla Orca and Magpie on U1 random seed 13956, read from the stalled process: supporting
// radius 20.96 mm, infill outlines spanning +-13.729 mm, a sampled point at y = -13.973 mm (0.244 mm below them) and
// the next one up at y = -10.480 mm. The overhang here is a stand-in that samples those two rows.
const coord_t   supporting_radius = scaled<coord_t>(20.96);
const Polygon   outline {Point::new_scale(-13.729, -13.729), Point::new_scale(13.729, -13.729),
                         Point::new_scale(13.729, 13.729), Point::new_scale(-13.729, 13.729)};
const Polygon   overhang {Point::new_scale(5., -14.5), Point::new_scale(10., -14.5),
                          Point::new_scale(10., -9.), Point::new_scale(5., -9.)};

// Supports cells the way Layer::generateNewTrees() does, a leaf on each cell it is handed, starting from the same
// index, and counts the passes. Each pass must remove at least its own cell.
size_t passes_to_support_all(FillLightning::DistanceField &field, bool mark_erased, size_t max_passes)
{
    size_t passes   = 0;
    size_t cell_idx = 0;
    Point  location;
    while (passes < max_passes && field.tryGetNextPoint(&location, &cell_idx, cell_idx)) {
        field.update(Point(0, 0), location);
        if (mark_erased)
            field.markErased(cell_idx);
        ++passes;
    }
    return passes;
}

} // namespace

TEST_CASE("Lightning distance field supports a sampled point outside the infill outlines", "[Lightning][Regression]")
{
    const Polygons outlines {outline};
    FillLightning::DistanceField field(supporting_radius, outlines, get_extents(outlines), {overhang});
    // A handful of cells; 1000 passes means one cell came back after its leaf was placed on it.
    const size_t passes = passes_to_support_all(field, false, 1000);
    CHECK(passes > 0);
    CHECK(passes < 1000);
    Point  location;
    size_t cell_idx = 0;
    CHECK_FALSE(field.tryGetNextPoint(&location, &cell_idx));
}

TEST_CASE("Lightning distance field marks a supported cell erased", "[Lightning][Regression]")
{
    const Polygons outlines {outline};
    FillLightning::DistanceField field(supporting_radius, outlines, get_extents(outlines), {overhang});
    Point  location;
    size_t cell_idx = 0;
    REQUIRE(field.tryGetNextPoint(&location, &cell_idx));
    field.markErased(cell_idx);
    CHECK(field.isErased(cell_idx));
    field.markErased(cell_idx); // a second call leaves it erased
    CHECK(field.isErased(cell_idx));
    // With every cell marked as it is handed out, the loop ends in at most one pass per cell.
    CHECK(passes_to_support_all(field, true, 1000) < 1000);
}
