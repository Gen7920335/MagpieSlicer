#pragma once
#include <stdint.h>

// Fixed-width host/device ABI. The host proves every signed intermediate fits.
struct MagpieCudaIntersectionInput {
    int64_t ax, ay, bx, by, scan_x;
    uint64_t stable_id;
};
struct MagpieCudaIntersectionOutput {
    int64_t numerator, denominator;
    uint64_t stable_id, valid;
};
static_assert(sizeof(MagpieCudaIntersectionInput) == 48, "CUDA input ABI");
static_assert(sizeof(MagpieCudaIntersectionOutput) == 32, "CUDA output ABI");

struct MagpieCudaAabbInput {
    int64_t qminx, qminy, qmaxx, qmaxy, tminx, tminy, tmaxx, tmaxy;
    uint64_t stable_id;
};
struct MagpieCudaPredicateOutput { uint64_t stable_id, value; };
static_assert(sizeof(MagpieCudaAabbInput) == 72, "CUDA AABB input ABI");
static_assert(sizeof(MagpieCudaPredicateOutput) == 16, "CUDA predicate output ABI");

struct MagpieCudaDistancePoint { int64_t x, y; uint32_t edge_count, reserved; uint64_t stable_id; };
struct MagpieCudaDistanceEdge { int64_t ax, ay, bx, by; };
struct MagpieCudaDistanceOutput { double distance_squared; uint64_t stable_id; };
struct MagpieCudaMeshZInput { float z0, z1, z2; uint32_t layer_count; };
struct MagpieCudaMeshZOutput { uint32_t first, last, lowest_vertex, stable_id; };
struct MagpieCudaPolygonEdge { int64_t ax, ay, bx, by; uint32_t polygon, flags; uint64_t reserved; };
static_assert(sizeof(MagpieCudaPolygonEdge) == 48, "CUDA polygon ABI");
static_assert(sizeof(MagpieCudaDistancePoint) == 32 && sizeof(MagpieCudaDistanceEdge) == 32, "CUDA distance ABI");
static_assert(sizeof(MagpieCudaDistanceOutput) == 16, "CUDA distance output ABI");
static_assert(sizeof(MagpieCudaMeshZInput) == 16 && sizeof(MagpieCudaMeshZOutput) == 16, "CUDA mesh ABI");
