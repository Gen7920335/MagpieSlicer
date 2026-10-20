#pragma once

// MAGPIE: the batch accelerator extension point (slot "accel.batch", mods/sdk/include/magpie_accel.h).
//
// Four per-item calculations of the slicer can run on an accelerator a native mod provides (the CUDA
// mod, mods/cuda_compute): facet layer ranges (TriangleMeshSlicer), rectilinear infill crossings
// (FillRectilinear), Lightning boundary distances (DistanceField) and tree support contact points inside
// the overhang (TreeSupport). Each keeps its CPU function, which stays the reference: the core collects
// a loop's items into one batch, offers it to the accelerator, and computes it on the CPU whenever the
// accelerator declines, fails or is absent.
//
// The core, not the mod, enforces that an accelerator cannot change the G-code: in Verify mode it
// recomputes every item with the CPU reference and throws the whole batch away on any difference. The
// Trusted mode, which skips that check, is granted only to official mods; any other mod asking for it
// runs in Verify mode.
//
// Without an accelerator, or with it off, nothing here runs and every caller takes Orca's own path.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {

class DynamicPrintConfig;

namespace Accel {

// The operations, numbered as MP_ACCEL_OP_* of magpie_accel.h.
enum class Op : uint32_t { FacetLayerRanges = 0, VerticalCrossings = 1, MinDistances = 2, PointsInExPolygons = 3 };

// One slicing session (Print::process). Picks the first loaded accelerator whose begin() asks to be
// used with this print's settings, and its mode; the destructor ends it.
class Session
{
public:
    explicit Session(const DynamicPrintConfig &full_print_config);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
};

// Whether an accelerator runs in the current session.
bool active();
// Whether a batch of `items` items of `op` goes to the accelerator: one runs and accepts the batch now.
bool should_dispatch(Op op, size_t items);
// "Verify", "Trusted" or "Off": the mode of the current session (tests and logs).
const char *mode_name();
// Batches of the current (or last) session thrown away because they differed from the CPU.
uint64_t rejected_batches();
// The id of the accelerator mod that ran in the current or last session, or empty (the G-code header).
std::string session_mod();

// Timing of the current (or last) session, per operation, over the batches the accelerator computed
// and that were used: wall time of the accelerator call (packing, transfer, kernels, decoding), and in
// Verify mode the wall time the CPU reference took on the same batches. The session's end writes them
// to the log, and to stderr with MAGPIE_ACCEL_REPORT=1. Trusted mode runs no CPU reference, so its CPU
// time stays 0.
struct OpTimes
{
    uint64_t batches = 0, items = 0;
    double   accel_ms = 0.;            // ms
    uint64_t checked_batches = 0;
    double   cpu_ms = 0.;              // ms, CPU reference on the checked batches
};
OpTimes op_times(Op op);
// One line per operation, as the session end logs it.
std::string timing_report();

// The dispatchers below return true and fill `out` when the accelerator computed every item (and, in
// Verify mode, every item matched `cpu(i)`). False means the caller computes everything on the CPU.
// `cpu(i)` must be the caller's own CPU calculation of item i.

// slice_facet_at_zs(): layer range [first, last) of zs a facet spans and its lowest vertex.
struct LayerRange
{
    uint32_t first;
    uint32_t last;
    uint32_t lowest_vertex;
    bool operator==(const LayerRange &rhs) const
    { return first == rhs.first && last == rhs.last && lowest_vertex == rhs.lowest_vertex; }
};
bool mesh_layer_ranges(const std::vector<std::array<float, 3>> &facet_z, const std::vector<float> &zs,
                       const std::function<LayerRange(size_t)> &cpu, std::vector<LayerRange> &out);

// slice_region_by_vertical_lines(): where p1-p2 crosses the vertical line x, both ends off the line.
struct VerticalCrossing
{
    Point   p1;
    Point   p2;
    coord_t x;
};
struct Rational
{
    int64_t  p;
    uint32_t q;
    bool operator==(const Rational &rhs) const { return p == rhs.p && q == rhs.q; }
};
bool vertical_intersections(const std::vector<VerticalCrossing> &crossings, const std::function<Rational(size_t)> &cpu,
                            std::vector<Rational> &out);

// DistanceField: squared distance from each point to the nearest of `edges` (Line::distance_to_squared),
// std::numeric_limits<double>::max() when there are no edges.
bool min_distances_squared(const Points &points, const Lines &edges, const std::function<double(size_t)> &cpu,
                           std::vector<double> &out);

// TreeSupport is_inside_ex(): whether any of `expolygons` contains the point, borders included.
bool points_in_expolygons(const Points &points, const ExPolygons &expolygons, const std::function<bool(size_t)> &cpu,
                          std::vector<uint8_t> &out);

} // namespace Accel
} // namespace Slic3r
