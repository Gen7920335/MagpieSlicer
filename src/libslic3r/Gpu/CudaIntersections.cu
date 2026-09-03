#include "CudaIntersectionAbi.hpp"
extern "C" __global__ void magpie_points_in_polygons(
    const MagpieCudaDistancePoint* input, MagpieCudaPredicateOutput* output, unsigned int count)
{
    const unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const auto q = input[i];
    const auto* edges = reinterpret_cast<const MagpieCudaPolygonEdge*>(input + count);
    bool inside = false, border = false, in_polygon = false, any_polygon = false;
    for (unsigned int j = 0; j < q.edge_count; ++j) {
        const auto e = edges[j];
        // Coordinates are preflight-bounded to +/-2^29: signed cross products fit.
        const int64_t cross = (e.bx-e.ax)*(q.y-e.ay)-(e.by-e.ay)*(q.x-e.ax);
        border |= cross == 0 && q.x >= min(e.ax,e.bx) && q.x <= max(e.ax,e.bx) &&
                               q.y >= min(e.ay,e.by) && q.y <= max(e.ay,e.by);
        if ((e.ay > q.y) != (e.by > q.y) && ((cross > 0) == (e.by > e.ay))) inside = !inside;
        if (e.flags & 1) {
            if (e.flags & 2) in_polygon = inside || border;
            else if (inside && !border) in_polygon = false;
            inside = border = false;
            if (j+1 == q.edge_count || edges[j+1].polygon != e.polygon) {
                any_polygon |= in_polygon;
                in_polygon = false;
            }
        }
    }
    output[i] = {q.stable_id,uint64_t(any_polygon)};
}
__device__ double magpie_squared_sum(double x, double y) {
    return __dadd_rn(__dmul_rn(x, x), __dmul_rn(y, y));
}
extern "C" __global__ void magpie_point_segment_distances(
    const MagpieCudaDistancePoint* input, MagpieCudaDistanceOutput* output, unsigned int count)
{
    const unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const auto q = input[i];
    const auto* edges = reinterpret_cast<const MagpieCudaDistanceEdge*>(input + count);
    double best = 1.7976931348623157e308;
    for (unsigned int j = 0; j < q.edge_count; ++j) {
        const auto e = edges[j];
        // Host preflight proves signed differences fit and convert exactly.
        const double vx = double(e.bx-e.ax), vy = double(e.by-e.ay);
        const double ax = double(q.x-e.ax), ay = double(q.y-e.ay);
        const double l2 = magpie_squared_sum(vx,vy);
        double d2;
        if (l2 == 0) d2 = magpie_squared_sum(ax,ay);
        else {
            const double t = __ddiv_rn(__dadd_rn(__dmul_rn(ax,vx),__dmul_rn(ay,vy)),l2);
            if (t <= 0) d2 = magpie_squared_sum(ax,ay);
            else if (t >= 1) d2 = magpie_squared_sum(double(q.x-e.bx),double(q.y-e.by));
            else d2 = magpie_squared_sum(__dsub_rn(__dmul_rn(t,vx),ax), __dsub_rn(__dmul_rn(t,vy),ay));
        }
        if (d2 < best) best = d2;
    }
    output[i] = {best,q.stable_id};
}
extern "C" __global__ void magpie_mesh_layer_ranges(
    const MagpieCudaMeshZInput* input, MagpieCudaMeshZOutput* output, unsigned int count)
{
    const unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const auto q = input[i];
    const auto* zs = reinterpret_cast<const float*>(input + count);
    const float low = fminf(q.z0,fminf(q.z1,q.z2)), high = fmaxf(q.z0,fmaxf(q.z1,q.z2));
    unsigned int first = 0, end = q.layer_count;
    while (first < end) {
        const unsigned int mid = first + (end-first)/2;
        if (zs[mid] < low) first = mid+1; else end = mid;
    }
    unsigned int last = first; end = q.layer_count;
    while (last < end) {
        const unsigned int mid = last + (end-last)/2;
        if (high < zs[mid]) end = mid; else last = mid+1;
    }
    output[i] = {first,last,q.z1 == low ? 1u : (q.z2 == low ? 2u : 0u),i};
}

extern "C" __global__ void magpie_vertical_intersections(
    const MagpieCudaIntersectionInput* input,
    MagpieCudaIntersectionOutput* output, unsigned int count)
{
    const unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const auto r = input[i];
    const bool forward = r.bx > r.ax;
    const int64_t denominator = forward ? r.bx - r.ax : r.ax - r.bx;
    const int64_t t = forward ? r.scan_x - r.ax : r.ax - r.scan_x;
    const bool valid = denominator > 0 && t > 0 && t < denominator;
    output[i] = {valid ? t * (r.by - r.ay) + r.ay * denominator : 0,
                 denominator, r.stable_id, valid ? uint64_t(1) : uint64_t(0)};
}

extern "C" __global__ void magpie_aabb_overlap(
    const MagpieCudaAabbInput* input, MagpieCudaPredicateOutput* output, unsigned int count)
{
    const unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count) return;
    const auto r = input[i];
    output[i] = {r.stable_id, uint64_t(r.qminx <= r.tmaxx && r.tminx <= r.qmaxx &&
                                     r.qminy <= r.tmaxy && r.tminy <= r.qmaxy)};
}
