#pragma once

// MAGPIE: CUDA test mod (resources/mods/cuda_compute.json, hidden key cuda_compute).
//
// Moves four per-item calculations of the slicer to an NVIDIA GPU so CUDA on and off can be compared:
// facet layer ranges (TriangleMeshSlicer), rectilinear infill crossings (FillRectilinear), Lightning
// boundary distances (DistanceField) and tree support contact points inside the overhang (TreeSupport).
// Each kernel reproduces its CPU function bit for bit (Gpu/CudaKernels.ptx), so the G-code is the same
// with CUDA on, off or unavailable.
//
// The kernels are PTX text that the NVIDIA driver compiles when they are first used. Neither the build
// nor the user needs the CUDA toolkit; without an NVIDIA driver every call returns false and the caller
// runs its CPU code.
//
// Modes: Off never loads the driver. On runs the GPU and also the CPU reference for every item, and
// throws the whole batch away on any difference (the contract: CUDA is optional and CPU-authoritative).
// MaxGpu uses the GPU result without the CPU reference.
//
// Environment overrides, for CLI runs: MAGPIE_CUDA_SLICER_ENABLE=0/1/max sets the mode,
// MAGPIE_CUDA_SLICER_FORCE_DISPATCH=1 sends batches of any size to the GPU (tests), and
// MAGPIE_CUDA_REPORT=1 prints the per-operation counts to stderr at the end of each slice.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r::Gpu {

enum class CudaMode { Off, On, MaxGpu };

// "on" -> On, "max" -> MaxGpu, anything else (the default "off") -> Off.
CudaMode cuda_mode_from_string(const std::string &value);
const char *cuda_mode_name(CudaMode mode);

// Mode of the running slicing session; Off outside one.
CudaMode cuda_mode();

// One slicing session (Print::process). Sets the mode from the config value, or from the
// environment override, and resets the counters; the destructor logs the counts and returns to Off.
class CudaSession
{
public:
    explicit CudaSession(const std::string &configured_mode);
    ~CudaSession();
    CudaSession(const CudaSession &) = delete;
    CudaSession &operator=(const CudaSession &) = delete;
};

// Whether a batch of `items` work items goes to the GPU: CUDA is on and the batch is at least
// cuda_minimum_batch_items long (or MAGPIE_CUDA_SLICER_FORCE_DISPATCH=1).
bool cuda_should_dispatch(size_t items);
// Smallest batch, in work items, worth a GPU round trip. A guess (line 1 used the same), not measured.
constexpr size_t cuda_minimum_batch_items = 4096;

// Loads the driver and compiles the kernels once per process. False with the reason when there is
// no usable NVIDIA GPU. Only called when CUDA is on, or by tests.
bool cuda_available(std::string *reason = nullptr);

// Counts of the current (or last) session, one line per operation.
std::string cuda_report();

// The dispatchers below return true and fill `out` when the GPU computed every item (and, in On mode,
// every item matched `cpu(i)`). False means the caller computes everything on the CPU: CUDA off, batch
// too small, the GPU busy with another batch, no GPU, a driver error, or a mismatch in On mode.
// `cpu(i)` must be the caller's own CPU calculation of item i.

// slice_facet_at_zs(): layer range [first, last) of zs a facet spans and its lowest vertex.
struct CudaLayerRange
{
    uint32_t first;
    uint32_t last;
    uint32_t lowest_vertex;
    bool operator==(const CudaLayerRange &rhs) const
    { return first == rhs.first && last == rhs.last && lowest_vertex == rhs.lowest_vertex; }
};
bool cuda_mesh_layer_ranges(const std::vector<std::array<float, 3>> &facet_z, const std::vector<float> &zs,
                            const std::function<CudaLayerRange(size_t)> &cpu, std::vector<CudaLayerRange> &out);

// slice_region_by_vertical_lines(): where p1-p2 crosses the vertical line x, both ends off the line.
struct CudaVerticalCrossing
{
    Point   p1;
    Point   p2;
    coord_t x;
};
struct CudaRational
{
    int64_t  p;
    uint32_t q;
    bool operator==(const CudaRational &rhs) const { return p == rhs.p && q == rhs.q; }
};
bool cuda_vertical_intersections(const std::vector<CudaVerticalCrossing> &crossings,
                                 const std::function<CudaRational(size_t)> &cpu, std::vector<CudaRational> &out);

// DistanceField: squared distance from each point to the nearest of `edges` (Line::distance_to_squared),
// std::numeric_limits<double>::max() when there are no edges.
bool cuda_min_distances_squared(const Points &points, const Lines &edges,
                                const std::function<double(size_t)> &cpu, std::vector<double> &out);

// TreeSupport is_inside_ex(): whether any of `expolygons` contains the point, borders included.
bool cuda_points_in_expolygons(const Points &points, const ExPolygons &expolygons,
                               const std::function<bool(size_t)> &cpu, std::vector<uint8_t> &out);

} // namespace Slic3r::Gpu
