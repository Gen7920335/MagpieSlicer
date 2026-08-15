#pragma once

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../Polyline.hpp"
#include "../ExPolygon.hpp"
#include "../Slicing.hpp"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace Slic3r {

class PrintObject;

namespace Tsunami {

enum class SegmentKind { StraightRib, Turn };

struct PathSegment {
    SegmentKind kind { SegmentKind::StraightRib };
    Polyline polyline;
};

using TargetId = size_t;
using IslandId = size_t;
using BranchId = size_t;
using TurnId = size_t;

enum class TargetLayout {
    SingleOrCoincident,
    SameZDifferentXY,
    SimilarXYDifferentZ,
    DifferentXYZ
};

struct SupportTargetSpec {
    TargetId id { 0 };
    size_t layer_index { 0 };
    double print_z { 0. };
    Point center;
    ExPolygons region;
};

// Deterministic target-domain samples shared by macro coverage planning and
// terminal micro trees. Physical support may use a subset, but the sampling
// lattice remains independent of currently emitted branches.
std::optional<std::vector<Point>> sample_target_contact_points(
    const SupportTargetSpec &target, double spacing, size_t maximum_points = 128);

struct MultiTargetPlanningInput {
    std::vector<SupportTargetSpec> targets;
    double layer_height { 0.2 };
    double branch_angle { 40. };
    double xy_similarity_tolerance { 0. };
    double z_similarity_tolerance { 0. };
};

struct MacroBranchPlan {
    BranchId id { 0 };
    std::optional<BranchId> parent;
    TurnId birth_turn_index { 0 };
    size_t birth_layer { 0 };
    double birth_z { 0. };
    Point attachment;
    double required_angle { 0. };
    // A lateral branch leaves and returns through its birth U-turn. Adding
    // this cycle changes the attachment vertex degree by two.
    bool closed_turn_loop { true };
    std::vector<TargetId> target_ids;
};

struct TrunkTurnCandidate {
    TurnId id { 0 };
    size_t layer_index { 0 };
    size_t source_segment_index { 0 };
    double print_z { 0. };
    Point center;
    Vec2d outward_direction { 1., 0. };
    bool convex { true };
    bool printable { true };
};

enum class TargetFailureReason {
    NoConvexTurn,
    NoPrintableTurn,
    OutsideConvexSide,
    InsufficientHeight,
    BranchAngleExceeded,
    TurnCapacityExceeded
};

struct TargetPlanFailure {
    TargetId target_id { 0 };
    TargetFailureReason reason { TargetFailureReason::NoConvexTurn };
};

struct BranchTopologyInput {
    std::vector<SupportTargetSpec> targets;
    std::vector<TrunkTurnCandidate> trunk_turns;
    // Geometry feasibility depends on both the target and source turn. Keep a
    // failed pairing isolated instead of disabling a turn for every target.
    std::vector<std::pair<TargetId, TurnId>> forbidden_assignments;
    double branch_angle { 40. };
    size_t max_branches_per_turn { 1 };
    bool prefer_earliest_birth { false };
};

struct BranchTopologyPlan {
    std::vector<MacroBranchPlan> branches;
    std::vector<TargetPlanFailure> failures;

    bool preserves_even_trunk_degree() const;
};

BranchTopologyPlan plan_macro_branches(const BranchTopologyInput &input);

enum class MacroBranchGeometryFailureReason {
    InvalidSourceTurn,
    InsufficientAnchor,
    TargetOutsideConvexSide,
    InsufficientHeight,
    BranchAngleExceeded,
    Collision,
    PrintabilityLimited
};

struct ImmutableBranchSegment {
    size_t id { 0 };
    size_t birth_layer { 0 };
    Polyline polyline;
};

struct ClosedMacroBranchLayer {
    size_t layer_index { 0 };
    double frontier_distance { 0. };
    double applied_growth { 0. };
    double support_ratio { 1. };
    std::vector<ImmutableBranchSegment> straight_wake;
    PathSegment active_turn;
    // Emit this apex-to-apex loop. The trunk source turn remains unchanged.
    Polyline detour;
    // Analysis/debug alias kept separate from the emitted path owner.
    Polyline closed_cycle;
};

struct ClosedMacroBranchInput {
    BranchId branch_id { 0 };
    TargetId target_id { 0 };
    PathSegment source_turn;
    Point target;
    size_t birth_layer { 0 };
    size_t target_layer { 0 };
    double birth_z { 0. };
    double target_z { 0. };
    double layer_height { 0.2 };
    double branch_angle { 40. };
    double extrusion_width { 0.4 };
    double minimum_layer_support_ratio { 0.5 };
    double anchor_length { 0. };
    double minimum_anchor_length { 0. };
    // Clear XY distance required between this branch's extrusion exterior
    // and any other non-connected branch's swept exterior. Not yet enforced
    // by plan_closed_macro_branch; carried through so the caller-side
    // candidate selection can apply it once implemented.
    double minimum_branch_spacing { 0. };
    // Used by terminal-ring micro branches. Reach the target as early as
    // printability permits, then keep the attained XY frontier fixed.
    bool complete_early { false };
    // Indexed by absolute object/support layer. Empty entries mean no obstacle.
    std::vector<ExPolygons> blocked_region_by_layer;
    // Optional absolute print Z values used for adaptive layer heights.
    std::vector<double> print_z_by_layer;
};

struct ClosedMacroBranchPlan {
    BranchId branch_id { 0 };
    TargetId target_id { 0 };
    Point attachment;
    Vec2d growth_direction { 1., 0. };
    double required_angle { 0. };
    // Convex source-turn subsection that becomes the propagating cap.
    PathSegment source_cap;
    std::vector<ClosedMacroBranchLayer> layers;
    size_t first_target_layer { size_t(-1) };
    bool reached_target { false };
};

struct ClosedMacroBranchResult {
    std::optional<ClosedMacroBranchPlan> plan;
    std::optional<MacroBranchGeometryFailureReason> failure;
    size_t failure_layer { size_t(-1) };
};

ClosedMacroBranchResult plan_closed_macro_branch(const ClosedMacroBranchInput &input);

struct TerminalRingPlan {
    BranchId branch_id { 0 };
    size_t source_turn_index { 0 };
    Point center;
    double radius { 0. };
    size_t base_layer { 0 };
    size_t tree_start_layer { 0 };
    // The source U-turn already extrudes one half on the base layer.
    Polyline base_complement;
    // Reused without XY changes on every riser layer above the base.
    Polyline vertical_ring;

    bool emits_base_complement(size_t layer_index) const { return layer_index == base_layer; }
    bool emits_vertical_ring(size_t layer_index) const
    {
        return layer_index > base_layer && layer_index <= tree_start_layer;
    }
};

struct TsunamiIslandPlan {
    IslandId id { 0 };
    std::vector<TargetId> target_ids;
    ExPolygons common_vertical_projection;
    bool requires_lateral_growth { false };
    std::vector<MacroBranchPlan> branches;
    std::vector<TerminalRingPlan> terminal_rings;
};

struct TsunamiObjectPlan {
    TargetLayout layout { TargetLayout::SingleOrCoincident };
    std::vector<TsunamiIslandPlan> islands;
};

double maximum_lateral_growth(double layer_height, double branch_angle);
TsunamiObjectPlan plan_target_topology(const MultiTargetPlanningInput &input);

struct VirtualRibField {
    Point origin;
    Vec2d guide_direction { 1., 0. };
    Vec2d rib_direction { 0., 1. };
    double spacing { 0. };
    double phase { 0. };

    Point rib_origin(int index) const;
    int nearest_index(const Point &point) const;
};

struct PhysicalRib {
    size_t id { 0 };
    int virtual_rib_index { 0 };
    Point origin;
    Vec2d direction { 0., 1. };
    double length { 0. };
    size_t birth_layer { 0 };
    bool active { true };
};

struct TrunkRibExtensionInput {
    PhysicalRib rib;
    Point model_reference;
    Polygon bed_region;
    ExPolygons blocked_region;
    double extrusion_width { 0.4 };
    double requested_extension { 0. };
};

struct TrunkRibExtensionPlan {
    PhysicalRib rib;
    double applied_extension { 0. };
    bool extended_negative_end { false };
};

std::optional<TrunkRibExtensionPlan> plan_trunk_rib_extension(
    const TrunkRibExtensionInput &input);

std::optional<TerminalRingPlan> plan_terminal_ring(
    const PathSegment &source_turn, size_t base_layer, size_t tree_start_layer);

struct SeededMicroTreeLayer {
    size_t layer_index { 0 };
    std::vector<Point> branch_centers;
    Polylines paths;
    double support_ratio { 1. };
};

struct SeededMicroTreeInput {
    TargetId target_id { 0 };
    PathSegment source_turn;
    size_t base_layer { 0 };
    SupportTargetSpec target;
    double branch_angle { 40. };
    double branch_distance { 2.5 };
    double tip_diameter { 0.8 };
    double extrusion_width { 0.4 };
    double minimum_layer_support_ratio { 0.5 };
    std::vector<ExPolygons> blocked_region_by_layer;
    std::vector<double> print_z_by_layer;
};

struct SeededMicroTreePlan {
    TargetId target_id { 0 };
    TerminalRingPlan seed_ring;
    std::vector<Point> contact_points;
    std::vector<SeededMicroTreeLayer> layers;
    bool reached_target { false };
};

std::optional<SeededMicroTreePlan> plan_seeded_micro_tree(const SeededMicroTreeInput &input);

struct TargetInterfaceLayer {
    size_t layer_index { 0 };
    // Counted from the model-contact layer: contact is 1, the layer below is 2.
    size_t interface_number { 1 };
    ExPolygons regions;
};

struct TargetInterfaceInput {
    SupportTargetSpec target;
    size_t interface_layer_count { 0 };
    // Physical extrusion footprint immediately below the interface stack.
    ExPolygons lower_support_footprint;
    // Maximum unsupported XY span allowed between the support footprint and
    // the first interface layer.
    double maximum_bridge_distance { 0. };
    std::vector<ExPolygons> blocked_region_by_layer;
};

struct TargetInterfacePlan {
    TargetId target_id { 0 };
    size_t support_base_layer { 0 };
    std::vector<TargetInterfaceLayer> layers;
};

std::optional<TargetInterfacePlan> plan_target_interface(const TargetInterfaceInput &input);

struct LayerPlan {
    size_t layer_index { 0 };
    std::vector<PhysicalRib> physical_ribs;
    std::vector<PathSegment> segments;
    Polyline path;
    bool has_active_turn { false };
    double applied_growth { 0. };
    double estimated_support_ratio { 1. };
};

struct StraightBranchInput {
    Point root;
    Point target;
    double layer_height { 0.2 };
    double branch_angle { 40. };
    double trunk_height { 5. };
    double rib_spacing { 2.5 };
    double rib_length { 8. };
    double minimum_physical_rib_length { 1. };
    double extrusion_width { 0.4 };
    double minimum_layer_support_ratio { 0.5 };
    double minimum_turn_radius { 0. };
    double minimum_anchor_length { 0. };
    double activation_threshold { 0. };
    double retention_threshold { 0. };
    std::vector<double> available_rib_length_by_layer;
    std::vector<ExPolygons> blocked_region_by_layer;
    size_t layer_count { 0 };
};

struct StraightBranchPlan {
    VirtualRibField rib_field;
    std::vector<LayerPlan> layers;
    bool reached_target { false };
};

StraightBranchPlan plan_straight_branch(const StraightBranchInput &input);
std::vector<TrunkTurnCandidate> collect_trunk_turn_candidates(
    const StraightBranchPlan &trunk, const std::vector<double> &print_z_by_layer);

struct RootSelectionInput {
    Polygon bed_region;
    ExPolygons blocked_region;
    // Bed area already claimed by roots planned earlier. It constrains where this
    // root may put extrusion, but never the contour it follows: the contour comes
    // from the model footprint alone, so reserving space must not move it.
    ExPolygons reserved_region;
    // Another root follows the same model contour, so this arc must not pad past
    // its target's angular extent: the neighbour occupies exactly that margin.
    bool shares_contour { false };
    ExPolygons target_region;
    Point target;
    bool allow_direct_projection { true };
    double maximum_xy_distance { 0. };
    double rib_spacing { 2.5 };
    double rib_length { 8. };
    double extrusion_width { 0.4 };
    double minimum_bed_contact_area { 0. };
    double maximum_bed_contact_area { 0. };
};

struct RootCandidate {
    // The target projection has no model interference, so this root needs no XY propagation.
    bool direct_projection { false };
    Point position;
    // Coarse reachability and runtime macro branches must measure from the
    // same printable U-turn apex, not from the frontier rib's center.
    Point approach_point;
    Polyline path;
    std::vector<PathSegment> segments;
    std::vector<PhysicalRib> physical_ribs;
    size_t frontier_rib_index { 0 };
    Point frontier_point;
    Vec2d frontier_tangent { 1., 0. };
    double target_coverage { 0. };
    double xy_distance { 0. };
    double rib_length { 0. };
    double bed_contact_area { 0. };
    double score { 0. };
};

std::optional<RootCandidate> select_root_candidate(const RootSelectionInput &input);

struct SharedTrunkInput {
    Polygon bed_region;
    ExPolygons blocked_region;
    // Forwarded to RootSelectionInput::reserved_region.
    ExPolygons reserved_region;
    // Forwarded to RootSelectionInput::shares_contour.
    bool shares_contour { false };
    std::vector<ExPolygons> blocked_region_by_layer;
    std::vector<double> print_z_by_layer;
    std::vector<SupportTargetSpec> targets;
    double layer_height { 0.2 };
    double branch_angle { 40. };
    double trunk_height { 5. };
    double rib_spacing { 2.5 };
    double rib_length { 8. };
    double extrusion_width { 0.4 };
    double minimum_layer_support_ratio { 0.5 };
    double minimum_bed_contact_area { 0. };
    double maximum_bed_contact_area { 0. };
    // Terminal-ring micro branches require a lateral Macro U-branch seed.
    bool allow_direct_projection { true };
};

struct SharedTrunkTargetPlan {
    TargetId target_id { 0 };
    Point branch_target;
    double xy_distance { 0. };
    double required_angle { 0. };
    bool direct_projection { false };
};

struct SharedTrunkPlan {
    RootCandidate root;
    std::vector<TargetId> target_ids;
    std::vector<SharedTrunkTargetPlan> target_plans;
    size_t top_layer { 0 };
    double top_z { 0. };
    double maximum_required_angle { 0. };
    bool all_targets_directly_above { false };
};

std::optional<SharedTrunkPlan> plan_shared_trunk(const SharedTrunkInput &input);

struct SharedTrunkForestPlan {
    std::vector<SharedTrunkPlan> trunks;
    std::vector<TargetId> unassigned_target_ids;
};

SharedTrunkForestPlan plan_shared_trunks(const SharedTrunkInput &input);

StraightBranchPlan plan_contour_branch(const RootCandidate &root, const StraightBranchInput &input);

} // namespace Tsunami

class TsunamiSupport
{
public:
    TsunamiSupport(PrintObject &object, const SlicingParameters &slicing_params)
        : m_object(object), m_slicing_params(slicing_params) {}

    void generate();
    std::function<void()> throw_on_cancel = []() {};

private:
    PrintObject &m_object;
    const SlicingParameters &m_slicing_params;
};

} // namespace Slic3r
