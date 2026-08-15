#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "libslic3r/Support/TsunamiSupport.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

Tsunami::StraightBranchInput basic_input()
{
    Tsunami::StraightBranchInput input;
    input.root = Point(scale_(0.), scale_(0.));
    input.target = Point(scale_(12.), scale_(0.));
    input.layer_height = 0.2;
    input.branch_angle = 45.;
    input.trunk_height = 0.4;
    input.rib_spacing = 2.;
    input.rib_length = 8.;
    input.minimum_physical_rib_length = 1.;
    input.layer_count = 40;
    return input;
}

Polygon circle(double radius, int segments = 180)
{
    Polygon polygon;
    polygon.points.reserve(size_t(segments));
    for (int index = 0; index < segments; ++index) {
        const double angle = 2. * M_PI * double(index) / double(segments);
        polygon.points.emplace_back(scale_(radius * std::cos(angle)), scale_(radius * std::sin(angle)));
    }
    return polygon;
}

ExPolygon annular_sector(double inner_radius, double outer_radius, double start_angle, double end_angle)
{
    Polygon polygon;
    constexpr int samples = 24;
    for (int index = 0; index <= samples; ++index) {
        const double angle = start_angle + (end_angle - start_angle) * double(index) / double(samples);
        polygon.points.emplace_back(scale_(outer_radius * std::cos(angle)), scale_(outer_radius * std::sin(angle)));
    }
    for (int index = samples; index >= 0; --index) {
        const double angle = start_angle + (end_angle - start_angle) * double(index) / double(samples);
        polygon.points.emplace_back(scale_(inner_radius * std::cos(angle)), scale_(inner_radius * std::sin(angle)));
    }
    return ExPolygon(std::move(polygon));
}

Tsunami::SupportTargetSpec support_target(size_t id, size_t layer_index, double print_z,
                                          double center_x, double center_y,
                                          double width = 4., double height = 4.)
{
    const double half_width = 0.5 * width;
    const double half_height = 0.5 * height;
    Polygon region {
        Point(scale_(center_x - half_width), scale_(center_y - half_height)),
        Point(scale_(center_x + half_width), scale_(center_y - half_height)),
        Point(scale_(center_x + half_width), scale_(center_y + half_height)),
        Point(scale_(center_x - half_width), scale_(center_y + half_height))
    };
    Tsunami::SupportTargetSpec target;
    target.id = id;
    target.layer_index = layer_index;
    target.print_z = print_z;
    target.center = Point(scale_(center_x), scale_(center_y));
    target.region = ExPolygons { ExPolygon(std::move(region)) };
    return target;
}

Tsunami::TrunkTurnCandidate trunk_turn(size_t id, size_t layer_index, double print_z,
                                       double center_x, double center_y,
                                       double outward_x = 1., double outward_y = 0.,
                                       bool convex = true, bool printable = true)
{
    Tsunami::TrunkTurnCandidate turn;
    turn.id = id;
    turn.layer_index = layer_index;
    turn.print_z = print_z;
    turn.center = Point(scale_(center_x), scale_(center_y));
    turn.outward_direction = Vec2d(outward_x, outward_y);
    turn.convex = convex;
    turn.printable = printable;
    return turn;
}

Tsunami::PathSegment completed_convex_turn(double radius = 1.)
{
    Tsunami::PathSegment turn;
    turn.kind = Tsunami::SegmentKind::Turn;
    constexpr int steps = 16;
    turn.polyline.points.reserve(steps + 1);
    for (int step = 0; step <= steps; ++step) {
        const double angle = -0.5 * M_PI + M_PI * double(step) / double(steps);
        turn.polyline.points.emplace_back(
            scale_(radius * std::cos(angle)), scale_(radius * std::sin(angle)));
    }
    return turn;
}

Tsunami::ClosedMacroBranchInput basic_closed_macro_branch_input()
{
    Tsunami::ClosedMacroBranchInput input;
    input.branch_id = 4;
    input.target_id = 9;
    input.source_turn = completed_convex_turn();
    input.target = Point(scale_(5.), scale_(0.));
    input.birth_layer = 10;
    input.target_layer = 50;
    input.birth_z = 2.2;
    input.target_z = 10.2;
    input.layer_height = 0.2;
    input.branch_angle = 45.;
    input.extrusion_width = 0.4;
    input.minimum_layer_support_ratio = 0.5;
    input.anchor_length = 8.;
    input.minimum_anchor_length = 2.;
    input.print_z_by_layer.resize(input.target_layer + 1);
    for (size_t layer_index = 0; layer_index < input.print_z_by_layer.size(); ++layer_index)
        input.print_z_by_layer[layer_index] = 0.2 * double(layer_index + 1);
    return input;
}

Tsunami::RootSelectionInput circular_root_input()
{
    Tsunami::RootSelectionInput input;
    input.bed_region = Polygon {
        Point(scale_(-50.), scale_(-50.)), Point(scale_(50.), scale_(-50.)),
        Point(scale_(50.), scale_(50.)), Point(scale_(-50.), scale_(50.))
    };
    input.blocked_region = ExPolygons { ExPolygon(circle(20.)) };
    input.target_region = ExPolygons { annular_sector(14., 18., -1.15, -0.15) };
    input.target = Point(scale_(15.), scale_(-9.));
    input.maximum_xy_distance = 10.;
    input.rib_spacing = 2.;
    input.rib_length = 6.;
    input.extrusion_width = 0.4;
    input.minimum_bed_contact_area = 1.;
    input.maximum_bed_contact_area = 100.;
    return input;
}

} // namespace

TEST_CASE("Tsunami support type round-trips through configuration serialization",
          "[TsunamiSupport][Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("support_type", "tsunami(auto)");

    REQUIRE(config.option<ConfigOptionEnum<SupportType>>("support_type") != nullptr);
    CHECK(config.option<ConfigOptionEnum<SupportType>>("support_type")->value == stTsunamiAuto);
    CHECK(config.opt_serialize("support_type") == "tsunami(auto)");
    REQUIRE(config.option<ConfigOptionBool>("tsunami_micro_branch_enabled") != nullptr);
    CHECK_FALSE(config.option<ConfigOptionBool>("tsunami_micro_branch_enabled")->value);
    config.set_key_value("tsunami_micro_branch_enabled", new ConfigOptionBool(true));
    CHECK(config.opt_serialize("tsunami_micro_branch_enabled") == "1");
    REQUIRE(config.option<ConfigOptionFloat>("tsunami_micro_branch_angle") != nullptr);
    CHECK(config.option<ConfigOptionFloat>("tsunami_micro_branch_angle")->value == 25.);
    REQUIRE(config.option<ConfigOptionFloat>("tsunami_micro_branch_size") != nullptr);
    CHECK(config.option<ConfigOptionFloat>("tsunami_micro_branch_size")->value == 2.);
}

TEST_CASE("Tsunami classifies the three multi-target layouts deterministically",
          "[TsunamiSupport][MultiTarget]")
{
    Tsunami::MultiTargetPlanningInput input;
    input.layer_height = 0.2;
    input.branch_angle = 45.;
    input.xy_similarity_tolerance = 1.;
    input.z_similarity_tolerance = 0.1;

    input.targets = {
        support_target(2, 50, 10., 18., 0.),
        support_target(1, 50, 10., -18., 0.)
    };
    Tsunami::TsunamiObjectPlan plan = Tsunami::plan_target_topology(input);
    CHECK(plan.layout == Tsunami::TargetLayout::SameZDifferentXY);
    REQUIRE(plan.islands.size() == 1);
    CHECK(plan.islands.front().target_ids == std::vector<Tsunami::TargetId> { 1, 2 });
    CHECK(plan.islands.front().requires_lateral_growth);

    input.targets = {
        support_target(4, 80, 16., 0.4, 0.2),
        support_target(3, 30, 6., 0., 0.)
    };
    plan = Tsunami::plan_target_topology(input);
    CHECK(plan.layout == Tsunami::TargetLayout::SimilarXYDifferentZ);
    REQUIRE(plan.islands.size() == 1);
    CHECK(plan.islands.front().target_ids == std::vector<Tsunami::TargetId> { 3, 4 });
    CHECK(plan.islands.front().requires_lateral_growth);

    input.targets = {
        support_target(6, 80, 16., 15., -7.),
        support_target(5, 30, 6., -12., 9.)
    };
    plan = Tsunami::plan_target_topology(input);
    CHECK(plan.layout == Tsunami::TargetLayout::DifferentXYZ);
    REQUIRE(plan.islands.size() == 1);
    CHECK(plan.islands.front().target_ids == std::vector<Tsunami::TargetId> { 5, 6 });
    CHECK(plan.islands.front().requires_lateral_growth);
}

TEST_CASE("Tsunami zero-angle topology groups only targets with a common vertical projection",
          "[TsunamiSupport][MultiTarget][ZeroAngle]")
{
    Tsunami::MultiTargetPlanningInput input;
    input.layer_height = 0.2;
    input.branch_angle = 0.;
    input.xy_similarity_tolerance = 0.;
    input.z_similarity_tolerance = 0.1;
    input.targets = {
        support_target(12, 60, 12., 20., 0., 4., 4.),
        support_target(11, 40, 8., 1., 0., 6., 6.),
        support_target(10, 20, 4., 0., 0., 6., 6.)
    };

    const Tsunami::TsunamiObjectPlan first = Tsunami::plan_target_topology(input);
    REQUIRE(first.islands.size() == 2);
    CHECK(first.islands[0].target_ids == std::vector<Tsunami::TargetId> { 10, 11 });
    CHECK(first.islands[1].target_ids == std::vector<Tsunami::TargetId> { 12 });
    CHECK_FALSE(first.islands[0].common_vertical_projection.empty());
    CHECK_FALSE(first.islands[0].requires_lateral_growth);
    CHECK_FALSE(first.islands[1].requires_lateral_growth);

    std::reverse(input.targets.begin(), input.targets.end());
    const Tsunami::TsunamiObjectPlan second = Tsunami::plan_target_topology(input);
    REQUIRE(second.islands.size() == first.islands.size());
    for (size_t island_index = 0; island_index < first.islands.size(); ++island_index)
        CHECK(second.islands[island_index].target_ids == first.islands[island_index].target_ids);
}

TEST_CASE("Tsunami branch angle is a hard upper bound on lateral growth",
          "[TsunamiSupport][MultiTarget][Angle]")
{
    CHECK(Tsunami::maximum_lateral_growth(0.2, 0.) == 0.);
    CHECK(std::abs(Tsunami::maximum_lateral_growth(0.2, 45.) - 0.2) < 1e-12);
    CHECK(Tsunami::maximum_lateral_growth(0.2, -10.) == 0.);
    CHECK(Tsunami::maximum_lateral_growth(0., 45.) == 0.);
    CHECK(Tsunami::maximum_lateral_growth(0.2, 120.) ==
          Tsunami::maximum_lateral_growth(0.2, 89.));
}

TEST_CASE("Tsunami shares one trunk only when every target remains printable and within angle",
          "[TsunamiSupport][SharedTrunk]")
{
    Tsunami::SharedTrunkInput input;
    input.bed_region = Polygon {
        Point(scale_(-50.), scale_(-50.)), Point(scale_(50.), scale_(-50.)),
        Point(scale_(50.), scale_(50.)), Point(scale_(-50.), scale_(50.))
    };
    input.targets = {
        support_target(2, 100, 20., 5., 0.),
        support_target(1, 100, 20., -5., 0.)
    };
    input.layer_height = 0.2;
    input.branch_angle = 45.;
    input.trunk_height = 2.;
    input.rib_spacing = 2.;
    input.rib_length = 8.;
    input.extrusion_width = 0.4;
    input.minimum_layer_support_ratio = 0.5;
    input.maximum_bed_contact_area = 100.;

    const std::optional<Tsunami::SharedTrunkPlan> first = Tsunami::plan_shared_trunk(input);
    REQUIRE(first.has_value());
    CHECK(first->target_ids == std::vector<Tsunami::TargetId> { 1, 2 });
    REQUIRE(first->target_plans.size() == 2);
    CHECK(first->target_plans[0].target_id == 1);
    CHECK(input.targets[1].region.front().contains(first->target_plans[0].branch_target, true));
    CHECK(first->target_plans[1].target_id == 2);
    CHECK(input.targets[0].region.front().contains(first->target_plans[1].branch_target, true));
    CHECK(first->maximum_required_angle <= input.branch_angle + 1e-9);
    CHECK(first->top_layer == 100);
    CHECK(std::abs(first->top_z - 20.) < 1e-9);

    std::reverse(input.targets.begin(), input.targets.end());
    const std::optional<Tsunami::SharedTrunkPlan> second = Tsunami::plan_shared_trunk(input);
    REQUIRE(second.has_value());
    CHECK(second->root.position == first->root.position);
    CHECK(second->root.path.points == first->root.path.points);
    CHECK(second->target_ids == first->target_ids);

    input.targets = {
        support_target(1, 30, 6., -30., 0.),
        support_target(2, 30, 6., 30., 0.)
    };
    input.branch_angle = 10.;
    input.trunk_height = 5.;
    CHECK_FALSE(Tsunami::plan_shared_trunk(input).has_value());
}

TEST_CASE("Tsunami zero-angle shared trunk requires direct target overlap",
          "[TsunamiSupport][SharedTrunk][ZeroAngle]")
{
    Tsunami::SharedTrunkInput input;
    input.bed_region = Polygon {
        Point(scale_(-20.), scale_(-20.)), Point(scale_(20.), scale_(-20.)),
        Point(scale_(20.), scale_(20.)), Point(scale_(-20.), scale_(20.))
    };
    input.targets = {
        support_target(1, 30, 6., 0., 0., 8., 8.),
        support_target(2, 60, 12., 7., 0., 8., 8.)
    };
    input.branch_angle = 0.;
    input.trunk_height = 2.;
    input.rib_spacing = 2.;
    input.rib_length = 6.;
    input.extrusion_width = 0.4;
    input.maximum_bed_contact_area = 100.;

    const std::optional<Tsunami::SharedTrunkPlan> shared = Tsunami::plan_shared_trunk(input);
    REQUIRE(shared.has_value());
    CHECK(shared->all_targets_directly_above);
    CHECK(shared->maximum_required_angle == 0.);

    input.blocked_region_by_layer.resize(61);
    input.blocked_region_by_layer[10] = input.targets.front().region;
    CHECK_FALSE(Tsunami::plan_shared_trunk(input).has_value());
    input.blocked_region_by_layer.clear();

    input.targets[1] = support_target(2, 60, 12., 12., 0., 4., 4.);
    CHECK_FALSE(Tsunami::plan_shared_trunk(input).has_value());
}

TEST_CASE("Tsunami treats a partially blocked target projection as lateral demand",
          "[TsunamiSupport][SharedTrunk][PartialProjection]")
{
    Tsunami::SharedTrunkInput input;
    input.bed_region = Polygon {
        Point(scale_(-50.), scale_(-50.)), Point(scale_(50.), scale_(-50.)),
        Point(scale_(50.), scale_(50.)), Point(scale_(-50.), scale_(50.))
    };
    input.blocked_region = ExPolygons { ExPolygon(circle(20.)) };
    Tsunami::SupportTargetSpec target;
    target.id = 1;
    target.layer_index = 100;
    target.print_z = 20.2;
    target.center = Point(scale_(19.), scale_(0.));
    target.region = ExPolygons { annular_sector(18., 22., -0.35, 0.35) };
    input.targets = { target };
    input.layer_height = 0.2;
    input.branch_angle = 45.;
    input.trunk_height = 4.;
    input.rib_spacing = 1.5;
    input.rib_length = 8.;
    input.extrusion_width = 0.4;
    input.minimum_bed_contact_area = 1.;
    input.maximum_bed_contact_area = 100.;

    const std::optional<Tsunami::SharedTrunkPlan> plan = Tsunami::plan_shared_trunk(input);
    REQUIRE(plan.has_value());
    CHECK_FALSE(plan->root.direct_projection);
    REQUIRE(plan->target_plans.size() == 1);
    CHECK_FALSE(plan->target_plans.front().direct_projection);
    CHECK(plan->target_plans.front().xy_distance > 0.);
    CHECK(plan->target_plans.front().required_angle <= input.branch_angle + 1e-9);
}

TEST_CASE("Tsunami shared trunk uses closed-branch geometry for similar XY targets at different Z",
          "[TsunamiSupport][SharedTrunk][DifferentZ]")
{
    Tsunami::SharedTrunkInput input;
    input.bed_region = Polygon {
        Point(scale_(-30.), scale_(-30.)), Point(scale_(30.), scale_(-30.)),
        Point(scale_(30.), scale_(30.)), Point(scale_(-30.), scale_(30.))
    };
    input.blocked_region = ExPolygons { ExPolygon(Polygon {
        Point(scale_(-3.), scale_(-3.)), Point(scale_(3.), scale_(-3.)),
        Point(scale_(3.), scale_(3.)), Point(scale_(-3.), scale_(3.))
    }) };
    input.targets = {
        support_target(1, 40, 8., 0., 0., 6., 6.),
        support_target(2, 80, 16., 0.4, 0.2, 6., 6.)
    };
    input.layer_height = 0.2;
    input.branch_angle = 45.;
    input.trunk_height = 2.;
    input.rib_spacing = 2.;
    input.rib_length = 6.;
    input.extrusion_width = 0.4;
    input.minimum_layer_support_ratio = 0.5;
    input.maximum_bed_contact_area = 100.;

    const std::optional<Tsunami::SharedTrunkPlan> plan =
        Tsunami::plan_shared_trunk(input);
    REQUIRE(plan.has_value());
    CHECK_FALSE(plan->root.direct_projection);
    CHECK_FALSE(plan->all_targets_directly_above);
    REQUIRE(plan->target_plans.size() == 2);
    for (const Tsunami::SharedTrunkTargetPlan &target : plan->target_plans) {
        CHECK_FALSE(target.direct_projection);
        CHECK(target.xy_distance > 0.);
        CHECK(target.required_angle <= input.branch_angle + 1e-9);
    }
}

TEST_CASE("Tsunami shared trunk reach uses actual adaptive layer Z values",
          "[TsunamiSupport][SharedTrunk][AdaptiveLayers]")
{
    Tsunami::SharedTrunkInput input;
    input.bed_region = Polygon {
        Point(scale_(-30.), scale_(-30.)), Point(scale_(30.), scale_(-30.)),
        Point(scale_(30.), scale_(30.)), Point(scale_(-30.), scale_(30.))
    };
    input.blocked_region = ExPolygons { ExPolygon(Polygon {
        Point(scale_(-5.), scale_(-5.)), Point(scale_(5.), scale_(-5.)),
        Point(scale_(5.), scale_(5.)), Point(scale_(-5.), scale_(5.))
    }) };
    input.targets = { support_target(1, 5, 10., 0., 0.) };
    input.layer_height = 0.2;
    input.branch_angle = 45.;
    input.trunk_height = 2.;
    input.rib_spacing = 2.;
    input.rib_length = 6.;
    input.extrusion_width = 0.4;
    input.minimum_layer_support_ratio = 0.5;
    input.maximum_bed_contact_area = 100.;

    REQUIRE(Tsunami::plan_shared_trunk(input).has_value());
    input.print_z_by_layer = { 0.2, 0.4, 0.6, 2., 6., 10. };
    CHECK_FALSE(Tsunami::plan_shared_trunk(input).has_value());
}

TEST_CASE("Tsunami splits only unreachable targets into additional trunks",
          "[TsunamiSupport][SharedTrunk][FailureIsolation]")
{
    Tsunami::SharedTrunkInput input;
    input.bed_region = Polygon {
        Point(scale_(-80.), scale_(-50.)), Point(scale_(80.), scale_(-50.)),
        Point(scale_(80.), scale_(50.)), Point(scale_(-80.), scale_(50.))
    };
    input.targets = {
        support_target(3, 30, 6., 55., 0.),
        support_target(1, 100, 20., -5., 0.),
        support_target(2, 100, 20., 5., 0.)
    };
    input.layer_height = 0.2;
    input.branch_angle = 35.;
    input.trunk_height = 2.;
    input.rib_spacing = 2.;
    input.rib_length = 8.;
    input.extrusion_width = 0.4;
    input.minimum_layer_support_ratio = 0.5;
    input.maximum_bed_contact_area = 100.;

    const Tsunami::SharedTrunkForestPlan plan = Tsunami::plan_shared_trunks(input);
    REQUIRE(plan.trunks.size() == 2);
    CHECK(plan.trunks[0].target_ids == std::vector<Tsunami::TargetId> { 1, 2 });
    CHECK(plan.trunks[1].target_ids == std::vector<Tsunami::TargetId> { 3 });
    CHECK(plan.unassigned_target_ids.empty());

    input.minimum_bed_contact_area = 1000.;
    const Tsunami::SharedTrunkForestPlan unavailable = Tsunami::plan_shared_trunks(input);
    CHECK(unavailable.trunks.empty());
    CHECK(unavailable.unassigned_target_ids == std::vector<Tsunami::TargetId> { 1, 2, 3 });
}

TEST_CASE("Tsunami assigns same-height XY targets to deterministic convex trunk turns",
          "[TsunamiSupport][BranchTopology][SameZ]")
{
    Tsunami::BranchTopologyInput input;
    input.branch_angle = 45.;
    input.targets = {
        support_target(2, 50, 10., 20., 0.),
        support_target(1, 50, 10., 10., 0.)
    };
    input.trunk_turns = {
        trunk_turn(3, 10, 2., 14., 0.),
        trunk_turn(2, 20, 4., 8., 0.),
        trunk_turn(1, 10, 2., 0., 0.)
    };

    const Tsunami::BranchTopologyPlan plan = Tsunami::plan_macro_branches(input);
    REQUIRE(plan.failures.empty());
    REQUIRE(plan.branches.size() == 2);
    CHECK(plan.branches[0].target_ids == std::vector<Tsunami::TargetId> { 1 });
    CHECK(plan.branches[0].birth_turn_index == 2);
    CHECK(plan.branches[1].target_ids == std::vector<Tsunami::TargetId> { 2 });
    CHECK(plan.branches[1].birth_turn_index == 3);
    CHECK(plan.branches[0].required_angle <= input.branch_angle);
    CHECK(plan.branches[1].required_angle <= input.branch_angle);
    CHECK(plan.preserves_even_trunk_degree());
}

TEST_CASE("Tsunami extracts only completed convex U-turns from trunk layers",
          "[TsunamiSupport][BranchTopology][TurnExtraction]")
{
    const Tsunami::StraightBranchPlan trunk = Tsunami::plan_straight_branch(basic_input());
    std::vector<double> print_z_by_layer(trunk.layers.size());
    for (size_t layer_index = 0; layer_index < print_z_by_layer.size(); ++layer_index)
        print_z_by_layer[layer_index] = 0.2 * double(layer_index + 1);

    const std::vector<Tsunami::TrunkTurnCandidate> turns =
        Tsunami::collect_trunk_turn_candidates(trunk, print_z_by_layer);
    REQUIRE_FALSE(turns.empty());
    size_t layer_two_turns = 0;
    for (size_t index = 0; index < turns.size(); ++index) {
        const Tsunami::TrunkTurnCandidate &turn = turns[index];
        CHECK(turn.id == index);
        CHECK(turn.convex);
        CHECK(turn.printable);
        CHECK(std::abs(turn.outward_direction.norm() - 1.) < 1e-12);
        CHECK(std::abs(turn.print_z - print_z_by_layer[turn.layer_index]) < 1e-12);
        if (turn.layer_index == 2)
            ++layer_two_turns;
    }
    // Layer two contains one completed turn and one active partial turn.
    CHECK(layer_two_turns == 1);
}

TEST_CASE("Tsunami splits similar-XY targets at different Z from different convex turns",
          "[TsunamiSupport][BranchTopology][DifferentZ]")
{
    Tsunami::BranchTopologyInput input;
    input.branch_angle = 40.;
    input.targets = {
        support_target(4, 80, 16., 10., 0.),
        support_target(3, 40, 8., 10., 0.)
    };
    input.trunk_turns = {
        trunk_turn(2, 20, 4., 8., 0.),
        trunk_turn(4, 60, 12., 8., 0.)
    };

    const Tsunami::BranchTopologyPlan plan = Tsunami::plan_macro_branches(input);
    REQUIRE(plan.failures.empty());
    REQUIRE(plan.branches.size() == 2);
    CHECK(plan.branches[0].target_ids == std::vector<Tsunami::TargetId> { 3 });
    CHECK(plan.branches[0].birth_turn_index == 2);
    CHECK(plan.branches[1].target_ids == std::vector<Tsunami::TargetId> { 4 });
    CHECK(plan.branches[1].birth_turn_index == 4);
    CHECK(plan.branches[0].closed_turn_loop);
    CHECK(plan.branches[1].closed_turn_loop);
}

TEST_CASE("Tsunami assigns fully different XYZ targets along one trunk when reachable",
          "[TsunamiSupport][BranchTopology][DifferentXYZ]")
{
    Tsunami::BranchTopologyInput input;
    input.branch_angle = 45.;
    input.targets = {
        support_target(7, 100, 20., -10., 12.),
        support_target(5, 40, 8., 2., 2.),
        support_target(6, 70, 14., 10., -5.)
    };
    input.trunk_turns = {
        trunk_turn(3, 60, 12., -6., 8., -1., 1.),
        trunk_turn(1, 10, 2., 0., 0., 1., 1.),
        trunk_turn(2, 40, 8., 6., -3., 2., -1.)
    };

    const Tsunami::BranchTopologyPlan plan = Tsunami::plan_macro_branches(input);
    REQUIRE(plan.failures.empty());
    REQUIRE(plan.branches.size() == 3);
    CHECK(plan.branches[0].birth_turn_index == 1);
    CHECK(plan.branches[1].birth_turn_index == 2);
    CHECK(plan.branches[2].birth_turn_index == 3);
    CHECK(plan.preserves_even_trunk_degree());
}

TEST_CASE("Tsunami macro branches use real Z and never exceed the configured angle",
          "[TsunamiSupport][BranchTopology][AngleBoundary]")
{
    Tsunami::BranchTopologyInput input;
    input.branch_angle = 45.;
    input.targets = { support_target(1, 101, 15., 10., 0.) };
    input.trunk_turns = {
        trunk_turn(9, 100, 14., 10., 0., 1., 0., false),
        trunk_turn(1, 1, 5., 0., 0.)
    };

    Tsunami::BranchTopologyPlan plan = Tsunami::plan_macro_branches(input);
    REQUIRE(plan.failures.empty());
    REQUIRE(plan.branches.size() == 1);
    CHECK(plan.branches.front().birth_turn_index == 1);
    CHECK(std::abs(plan.branches.front().required_angle - 45.) < 1e-9);

    input.branch_angle = 44.999;
    plan = Tsunami::plan_macro_branches(input);
    REQUIRE(plan.branches.empty());
    REQUIRE(plan.failures.size() == 1);
    CHECK(plan.failures.front().reason == Tsunami::TargetFailureReason::BranchAngleExceeded);
}

TEST_CASE("Tsunami branch topology isolates convex-side and turn-capacity failures",
          "[TsunamiSupport][BranchTopology][FailureIsolation]")
{
    Tsunami::BranchTopologyInput input;
    input.branch_angle = 45.;
    input.max_branches_per_turn = 1;
    input.targets = {
        support_target(1, 50, 10., 5., 0.),
        support_target(2, 50, 10., 6., 0.),
        support_target(3, 50, 10., -2., 0.)
    };
    input.trunk_turns = { trunk_turn(7, 10, 2., 0., 0.) };

    const Tsunami::BranchTopologyPlan plan = Tsunami::plan_macro_branches(input);
    REQUIRE(plan.branches.size() == 1);
    CHECK(plan.branches.front().target_ids == std::vector<Tsunami::TargetId> { 1 });
    REQUIRE(plan.failures.size() == 2);
    CHECK(plan.failures[0].target_id == 3);
    CHECK(plan.failures[0].reason == Tsunami::TargetFailureReason::OutsideConvexSide);
    CHECK(plan.failures[1].target_id == 2);
    CHECK(plan.failures[1].reason == Tsunami::TargetFailureReason::TurnCapacityExceeded);
}

TEST_CASE("Tsunami rejects only the failed target and turn geometry pairing",
          "[TsunamiSupport][BranchTopology][GeometryRetry]")
{
    Tsunami::BranchTopologyInput input;
    input.branch_angle = 45.;
    input.max_branches_per_turn = 1;
    input.targets = {
        support_target(1, 50, 10., 5., 0.),
        support_target(2, 50, 10., 6., 0.)
    };
    input.trunk_turns = {
        trunk_turn(1, 10, 2., 0., 0.),
        trunk_turn(2, 20, 4., 0., 0.)
    };

    Tsunami::BranchTopologyPlan plan = Tsunami::plan_macro_branches(input);
    REQUIRE(plan.failures.empty());
    REQUIRE(plan.branches.size() == 2);
    CHECK(plan.branches[0].birth_turn_index == 2);
    CHECK(plan.branches[1].birth_turn_index == 1);

    input.forbidden_assignments.emplace_back(1, 2);
    plan = Tsunami::plan_macro_branches(input);
    REQUIRE(plan.failures.empty());
    REQUIRE(plan.branches.size() == 2);
    CHECK(plan.branches[0].birth_turn_index == 1);
    CHECK(plan.branches[1].birth_turn_index == 2);
}

TEST_CASE("Tsunami macro U branch leaves immutable straight wake behind one propagating turn",
          "[TsunamiSupport][MacroBranch][ClosedLoop]")
{
    const Tsunami::ClosedMacroBranchInput input = basic_closed_macro_branch_input();
    const Tsunami::ClosedMacroBranchResult result = Tsunami::plan_closed_macro_branch(input);
    REQUIRE(result.plan.has_value());
    REQUIRE_FALSE(result.failure.has_value());
    const Tsunami::ClosedMacroBranchPlan &plan = *result.plan;
    REQUIRE(plan.reached_target);
    REQUIRE(plan.layers.size() == input.target_layer - input.birth_layer);
    CHECK(plan.layers.front().layer_index == input.birth_layer + 1);
    CHECK(plan.layers.back().layer_index == input.target_layer);
    CHECK(plan.attachment == Point(scale_(1.), scale_(0.)));
    CHECK(plan.required_angle <= input.branch_angle + 1e-9);

    REQUIRE(plan.source_cap.polyline.points.size() >= 3);
    const Point source_cap_start = plan.source_cap.polyline.points.front();
    for (const Point &point : plan.layers.front().detour.points) {
        const bool lies_on_source_vertex = std::find(
            input.source_turn.polyline.points.begin(), input.source_turn.polyline.points.end(), point) !=
            input.source_turn.polyline.points.end();
        if (lies_on_source_vertex)
            CHECK(point == plan.attachment);
    }
    std::vector<Polyline> immutable_segments;
    for (const Tsunami::ClosedMacroBranchLayer &layer : plan.layers) {
        REQUIRE(layer.detour.points.size() >= plan.source_cap.polyline.points.size());
        CHECK(layer.detour.points.front() == plan.attachment);
        CHECK(layer.detour.points.back() == plan.attachment);
        REQUIRE(layer.closed_cycle.points.size() >= layer.detour.points.size());
        CHECK(layer.closed_cycle.points.front() == layer.closed_cycle.points.back());
        Polygon cycle_polygon;
        cycle_polygon.points.assign(layer.closed_cycle.points.begin(), layer.closed_cycle.points.end() - 1);
        CHECK(std::abs(cycle_polygon.area()) > 0.);
        CHECK(layer.active_turn.kind == Tsunami::SegmentKind::Turn);
        CHECK(layer.support_ratio + 1e-9 >= input.minimum_layer_support_ratio);
        CHECK(layer.applied_growth <=
              Tsunami::maximum_lateral_growth(input.layer_height, input.branch_angle) + 2e-6);

        for (const Tsunami::ImmutableBranchSegment &segment : layer.straight_wake) {
            if (segment.id == immutable_segments.size())
                immutable_segments.emplace_back(segment.polyline);
            REQUIRE(segment.id < immutable_segments.size());
            CHECK(segment.polyline.points == immutable_segments[segment.id].points);
            CHECK(segment.birth_layer <= layer.layer_index);
        }

        const Point turn_shift = layer.active_turn.polyline.points.front() - source_cap_start;
        REQUIRE(layer.active_turn.polyline.points.size() == plan.source_cap.polyline.points.size());
        for (size_t point_index = 0; point_index < layer.active_turn.polyline.points.size(); ++point_index) {
            const Point expected = plan.source_cap.polyline.points[point_index] + turn_shift;
            CHECK(layer.active_turn.polyline.points[point_index] == expected);
        }
    }
    REQUIRE_FALSE(immutable_segments.empty());
    CHECK(std::abs(plan.layers.back().frontier_distance - 4.) < 2e-6);
}

TEST_CASE("Tsunami macro U branch stays parallel to its source Trunk direction for a laterally offset target",
          "[TsunamiSupport][MacroBranch][RigidParallel]")
{
    // Regression for the rejected endpoint-directed routing: aiming
    // growth_direction at the target tilted Macro branches away from their
    // source Trunk rib and let independently planned branches overlap
    // (CODEX_HANDOFF.md "Failed Approach: Aim every Macro branch directly at
    // its target endpoint"). source->outward_direction for
    // completed_convex_turn() is (1, 0); a target offset to (5, 3) has the
    // same 4 mm forward reach as the axis-aligned basic fixture (target
    // (5, 0)) but adds a 3 mm lateral component that must NOT bend the rail.
    Tsunami::ClosedMacroBranchInput input = basic_closed_macro_branch_input();
    input.target = Point(scale_(5.), scale_(3.));
    const Tsunami::ClosedMacroBranchResult result = Tsunami::plan_closed_macro_branch(input);
    REQUIRE(result.plan.has_value());
    REQUIRE_FALSE(result.failure.has_value());
    const Tsunami::ClosedMacroBranchPlan &plan = *result.plan;
    REQUIRE(plan.reached_target);

    // Rails inherit the source Trunk's undirected orientation; they never
    // rotate toward the target's lateral offset.
    CHECK(std::abs(plan.growth_direction.x() - 1.) < 1e-9);
    CHECK(std::abs(plan.growth_direction.y()) < 1e-9);

    // target_distance is the forward projection onto that fixed direction
    // (4 mm), not the euclidean distance to the target (5 mm). An
    // endpoint-directed implementation would report atan2(5, 8) =~ 32.005
    // degrees here instead of atan2(4, 8) =~ 26.565 degrees.
    CHECK(std::abs(plan.required_angle - 26.565051177) < 1e-6);
    REQUIRE_FALSE(plan.layers.empty());
    CHECK(std::abs(plan.layers.back().frontier_distance - 4.) < 2e-6);

    // Every active turn on every layer is a pure translation of the source
    // cap along the fixed growth_direction, so it never curves toward the
    // target either.
    const Point source_cap_start = plan.source_cap.polyline.points.front();
    for (const Tsunami::ClosedMacroBranchLayer &layer : plan.layers) {
        REQUIRE(layer.active_turn.polyline.points.size() == plan.source_cap.polyline.points.size());
        const Point turn_shift = layer.active_turn.polyline.points.front() - source_cap_start;
        CHECK(turn_shift.y() == 0);
        for (size_t point_index = 0; point_index < layer.active_turn.polyline.points.size(); ++point_index) {
            const Point expected = plan.source_cap.polyline.points[point_index] + turn_shift;
            CHECK(layer.active_turn.polyline.points[point_index] == expected);
        }
    }
}

TEST_CASE("Tsunami macro U branch enforces angle convex-side and anchor boundaries",
          "[TsunamiSupport][MacroBranch][Boundary]")
{
    Tsunami::ClosedMacroBranchInput input = basic_closed_macro_branch_input();
    input.target = Point(scale_(9.), scale_(0.));
    input.branch_angle = 45.;
    input.minimum_layer_support_ratio = 0.;
    Tsunami::ClosedMacroBranchResult result = Tsunami::plan_closed_macro_branch(input);
    REQUIRE(result.plan.has_value());
    CHECK(std::abs(result.plan->required_angle - 45.) < 1e-9);

    input.branch_angle = 44.999;
    result = Tsunami::plan_closed_macro_branch(input);
    REQUIRE(result.failure.has_value());
    CHECK(*result.failure == Tsunami::MacroBranchGeometryFailureReason::BranchAngleExceeded);

    input = basic_closed_macro_branch_input();
    input.target = Point(scale_(-2.), scale_(0.));
    result = Tsunami::plan_closed_macro_branch(input);
    REQUIRE(result.failure.has_value());
    CHECK(*result.failure == Tsunami::MacroBranchGeometryFailureReason::TargetOutsideConvexSide);

    input = basic_closed_macro_branch_input();
    input.anchor_length = input.minimum_anchor_length - 1e-3;
    result = Tsunami::plan_closed_macro_branch(input);
    REQUIRE(result.failure.has_value());
    CHECK(*result.failure == Tsunami::MacroBranchGeometryFailureReason::InsufficientAnchor);
}

TEST_CASE("Tsunami macro U branch isolates collision and printability failures",
          "[TsunamiSupport][MacroBranch][FailureIsolation]")
{
    Tsunami::ClosedMacroBranchInput input = basic_closed_macro_branch_input();
    const ExPolygons obstacle { ExPolygon(Polygon {
        Point(scale_(2.), scale_(-2.)), Point(scale_(3.), scale_(-2.)),
        Point(scale_(3.), scale_(2.)), Point(scale_(2.), scale_(2.))
    }) };
    input.blocked_region_by_layer.assign(input.target_layer + 1, obstacle);
    Tsunami::ClosedMacroBranchResult result = Tsunami::plan_closed_macro_branch(input);
    REQUIRE(result.failure.has_value());
    CHECK(*result.failure == Tsunami::MacroBranchGeometryFailureReason::Collision);
    CHECK(result.failure_layer <= input.target_layer);

    input = basic_closed_macro_branch_input();
    input.minimum_layer_support_ratio = 1.;
    result = Tsunami::plan_closed_macro_branch(input);
    REQUIRE(result.failure.has_value());
    CHECK(*result.failure == Tsunami::MacroBranchGeometryFailureReason::PrintabilityLimited);
}

TEST_CASE("Tsunami micro branch reaches XY early and freezes the terminal U-turn for a vertical ring",
          "[TsunamiSupport][MacroBranch][TerminalRing]")
{
    Tsunami::ClosedMacroBranchInput input = basic_closed_macro_branch_input();
    input.complete_early = true;
    input.target_layer = 90;
    input.target_z = 18.2;
    input.print_z_by_layer.resize(input.target_layer + 1);
    for (size_t layer_index = 0; layer_index < input.print_z_by_layer.size(); ++layer_index)
        input.print_z_by_layer[layer_index] = 0.2 * double(layer_index + 1);

    const Tsunami::ClosedMacroBranchResult result = Tsunami::plan_closed_macro_branch(input);
    REQUIRE(result.plan.has_value());
    const Tsunami::ClosedMacroBranchPlan &branch = *result.plan;
    REQUIRE(branch.reached_target);
    REQUIRE(branch.first_target_layer != size_t(-1));
    CHECK(branch.first_target_layer < input.target_layer);

    const auto first_target = std::find_if(
        branch.layers.begin(), branch.layers.end(), [&branch](const Tsunami::ClosedMacroBranchLayer &layer) {
            return layer.layer_index == branch.first_target_layer;
        });
    REQUIRE(first_target != branch.layers.end());
    const std::optional<Tsunami::TerminalRingPlan> ring = Tsunami::plan_terminal_ring(
        first_target->active_turn, branch.first_target_layer, input.target_layer);
    REQUIRE(ring.has_value());
    CHECK(ring->base_layer == branch.first_target_layer);
    CHECK(ring->tree_start_layer == input.target_layer);

    for (auto layer = first_target + 1; layer != branch.layers.end(); ++layer) {
        CHECK(std::abs(layer->applied_growth) < 1e-9);
        CHECK(std::abs(layer->frontier_distance - first_target->frontier_distance) < 1e-9);
        CHECK(layer->active_turn.polyline.points == first_target->active_turn.polyline.points);
    }
}

TEST_CASE("Tsunami stabilizes its trunk by extending the first rib away from the model",
          "[TsunamiSupport][TrunkExtension]")
{
    Tsunami::TrunkRibExtensionInput input;
    input.rib.id = 4;
    input.rib.origin = Point(scale_(0.), scale_(0.));
    input.rib.direction = Vec2d(1., 0.);
    input.rib.length = 4.;
    input.rib.birth_layer = 0;
    input.model_reference = Point(scale_(10.), scale_(0.));
    input.bed_region = Polygon {
        Point(scale_(-20.), scale_(-10.)), Point(scale_(20.), scale_(-10.)),
        Point(scale_(20.), scale_(10.)), Point(scale_(-20.), scale_(10.))
    };
    input.extrusion_width = 0.4;
    input.requested_extension = 5.;

    const std::optional<Tsunami::TrunkRibExtensionPlan> extension =
        Tsunami::plan_trunk_rib_extension(input);
    REQUIRE(extension.has_value());
    CHECK(extension->extended_negative_end);
    CHECK(std::abs(extension->applied_extension - 5.) < 1e-9);
    CHECK(extension->rib.id == input.rib.id);
    CHECK(extension->rib.birth_layer == input.rib.birth_layer);
    CHECK(std::abs(extension->rib.length - 9.) < 1e-9);

    const Vec2d extended_origin(
        unscale<double>(extension->rib.origin.x()), unscale<double>(extension->rib.origin.y()));
    const Vec2d negative_end = extended_origin - 0.5 * extension->rib.length * extension->rib.direction;
    const Vec2d positive_end = extended_origin + 0.5 * extension->rib.length * extension->rib.direction;
    CHECK(std::abs(negative_end.x() + 7.) < 2e-6);
    CHECK(std::abs(positive_end.x() - 2.) < 2e-6);
}

TEST_CASE("Tsunami trunk rib extension is clamped before crossing the bed boundary",
          "[TsunamiSupport][TrunkExtension][Boundary]")
{
    Tsunami::TrunkRibExtensionInput input;
    input.rib.origin = Point(scale_(0.), scale_(0.));
    input.rib.direction = Vec2d(1., 0.);
    input.rib.length = 4.;
    input.model_reference = Point(scale_(10.), scale_(0.));
    input.bed_region = Polygon {
        Point(scale_(-5.), scale_(-5.)), Point(scale_(5.), scale_(-5.)),
        Point(scale_(5.), scale_(5.)), Point(scale_(-5.), scale_(5.))
    };
    input.extrusion_width = 0.4;
    input.requested_extension = 10.;

    const std::optional<Tsunami::TrunkRibExtensionPlan> extension =
        Tsunami::plan_trunk_rib_extension(input);
    REQUIRE(extension.has_value());
    CHECK(extension->applied_extension < input.requested_extension);
    CHECK(extension->applied_extension > 2.7);
    CHECK(extension->applied_extension < 2.9);

    const Vec2d origin(
        unscale<double>(extension->rib.origin.x()), unscale<double>(extension->rib.origin.y()));
    const Vec2d negative_end = origin - 0.5 * extension->rib.length * extension->rib.direction;
    CHECK(negative_end.x() >= -4.81);
}

TEST_CASE("Tsunami terminal ring closes an existing U-turn and starts at its branch layer",
          "[TsunamiSupport][TerminalRing]")
{
    Tsunami::StraightBranchInput input = basic_input();
    input.target = Point(scale_(2.), scale_(0.));
    input.layer_count = 4;
    const Tsunami::StraightBranchPlan branch = Tsunami::plan_straight_branch(input);
    REQUIRE_FALSE(branch.layers.empty());
    const auto source_turn = std::find_if(
        branch.layers.front().segments.begin(), branch.layers.front().segments.end(),
        [](const Tsunami::PathSegment &segment) { return segment.kind == Tsunami::SegmentKind::Turn; });
    REQUIRE(source_turn != branch.layers.front().segments.end());

    const std::optional<Tsunami::TerminalRingPlan> ring =
        Tsunami::plan_terminal_ring(*source_turn, 7, 13);
    REQUIRE(ring.has_value());
    REQUIRE(ring->base_complement.points.size() >= 3);
    REQUIRE(ring->vertical_ring.points.size() > source_turn->polyline.points.size());
    CHECK(ring->base_complement.points.front() == source_turn->polyline.points.back());
    CHECK(ring->base_complement.points.back() == source_turn->polyline.points.front());
    CHECK(ring->vertical_ring.points.front() == ring->vertical_ring.points.back());

    const Vec2d center(unscale<double>(ring->center.x()), unscale<double>(ring->center.y()));
    for (const Point &point : ring->vertical_ring.points) {
        const Vec2d point_mm(unscale<double>(point.x()), unscale<double>(point.y()));
        CHECK(std::abs((point_mm - center).norm() - ring->radius) < 2e-6);
    }

    for (size_t layer_index = 0; layer_index < 16; ++layer_index) {
        CHECK(ring->emits_base_complement(layer_index) == (layer_index == 7));
        CHECK(ring->emits_vertical_ring(layer_index) ==
              (layer_index > 7 && layer_index <= 13));
    }
}

TEST_CASE("Tsunami terminal ring rejects a non-turn or incomplete U-turn",
          "[TsunamiSupport][TerminalRing][Boundary]")
{
    Tsunami::PathSegment straight;
    straight.kind = Tsunami::SegmentKind::StraightRib;
    straight.polyline.points = {
        Point(scale_(0.), scale_(0.)), Point(scale_(2.), scale_(0.))
    };
    CHECK_FALSE(Tsunami::plan_terminal_ring(straight, 2, 5).has_value());

    Tsunami::PathSegment incomplete;
    incomplete.kind = Tsunami::SegmentKind::Turn;
    incomplete.polyline.points = {
        Point(scale_(-1.), scale_(0.)), Point(scale_(-0.7), scale_(0.7)),
        Point(scale_(0.), scale_(1.))
    };
    CHECK_FALSE(Tsunami::plan_terminal_ring(incomplete, 2, 5).has_value());
    CHECK_FALSE(Tsunami::plan_terminal_ring(incomplete, 5, 2).has_value());
}

TEST_CASE("Tsunami seeded micro tree grows only above its vertical terminal ring",
          "[TsunamiSupport][MicroTree]")
{
    Tsunami::SeededMicroTreeInput input;
    input.target_id = 7;
    input.source_turn = completed_convex_turn();
    input.source_turn.polyline.translate(Point::new_scale(4., 0.));
    input.base_layer = 20;
    input.target = support_target(7, 80, 16.2, 5., 0., 2., 2.);
    input.branch_angle = 40.;
    input.branch_distance = 1.;
    input.tip_diameter = 0.8;
    input.extrusion_width = 0.4;
    input.minimum_layer_support_ratio = 0.5;
    input.print_z_by_layer.resize(input.target.layer_index + 1);
    for (size_t layer_index = 0; layer_index < input.print_z_by_layer.size(); ++layer_index)
        input.print_z_by_layer[layer_index] = 0.2 * double(layer_index + 1);

    const std::optional<Tsunami::SeededMicroTreePlan> plan =
        Tsunami::plan_seeded_micro_tree(input);
    REQUIRE(plan.has_value());
    CHECK(plan->target_id == input.target_id);
    CHECK(plan->seed_ring.base_layer == input.base_layer);
    CHECK(plan->seed_ring.tree_start_layer > input.base_layer);
    CHECK(plan->seed_ring.tree_start_layer < input.target.layer_index);
    CHECK(plan->reached_target);
    REQUIRE_FALSE(plan->layers.empty());
    CHECK(plan->layers.front().layer_index == plan->seed_ring.tree_start_layer + 1);
    CHECK(plan->layers.back().layer_index == input.target.layer_index);
    for (const Tsunami::SeededMicroTreeLayer &layer : plan->layers) {
        CHECK_FALSE(layer.paths.empty());
        CHECK_FALSE(layer.branch_centers.empty());
        CHECK(layer.support_ratio + 1e-9 >= input.minimum_layer_support_ratio);
        for (const Polyline &path : layer.paths)
            CHECK(path.points.front() == path.points.back());
    }

    std::vector<Point> top_centers = plan->layers.back().branch_centers;
    std::sort(top_centers.begin(), top_centers.end(), [](const Point &left, const Point &right) {
        return std::tie(left.x(), left.y()) < std::tie(right.x(), right.y());
    });
    CHECK(top_centers == plan->contact_points);
}

TEST_CASE("Tsunami seeded micro tree refuses insufficient height and model collision",
          "[TsunamiSupport][MicroTree][FailureIsolation]")
{
    Tsunami::SeededMicroTreeInput input;
    input.target_id = 8;
    input.source_turn = completed_convex_turn();
    input.base_layer = 20;
    input.target = support_target(8, 22, 4.6, 4., 0., 2., 2.);
    input.branch_angle = 20.;
    input.branch_distance = 1.;
    input.tip_diameter = 0.8;
    input.extrusion_width = 0.4;
    input.minimum_layer_support_ratio = 0.5;
    input.print_z_by_layer.resize(input.target.layer_index + 1);
    for (size_t layer_index = 0; layer_index < input.print_z_by_layer.size(); ++layer_index)
        input.print_z_by_layer[layer_index] = 0.2 * double(layer_index + 1);
    CHECK_FALSE(Tsunami::plan_seeded_micro_tree(input).has_value());

    input.target = support_target(8, 80, 16.2, 1., 0., 2., 2.);
    input.print_z_by_layer.resize(input.target.layer_index + 1);
    for (size_t layer_index = 0; layer_index < input.print_z_by_layer.size(); ++layer_index)
        input.print_z_by_layer[layer_index] = 0.2 * double(layer_index + 1);
    input.blocked_region_by_layer.resize(input.target.layer_index + 1);
    const ExPolygons obstacle { ExPolygon(Polygon {
        Point(scale_(-2.), scale_(-2.)), Point(scale_(2.), scale_(-2.)),
        Point(scale_(2.), scale_(2.)), Point(scale_(-2.), scale_(2.))
    }) };
    for (size_t layer_index = input.base_layer + 1; layer_index <= input.target.layer_index; ++layer_index)
        input.blocked_region_by_layer[layer_index] = obstacle;
    CHECK_FALSE(Tsunami::plan_seeded_micro_tree(input).has_value());
}

TEST_CASE("Tsunami target interface preserves one complex footprint through its full stack",
          "[TsunamiSupport][Interface]")
{
    Tsunami::TargetInterfaceInput input;
    input.target = support_target(31, 20, 4.2, 0., 0., 8., 6.);
    input.target.region.front().holes.emplace_back(Polygon {
        Point(scale_(-1.), scale_(-1.)), Point(scale_(-1.), scale_(1.)),
        Point(scale_(1.), scale_(1.)), Point(scale_(1.), scale_(-1.))
    });
    input.interface_layer_count = 3;
    input.lower_support_footprint = offset_ex(input.target.region, scale_(0.8), ClipperLib::jtRound);
    input.maximum_bridge_distance = 0.;
    input.blocked_region_by_layer.resize(input.target.layer_index + 1);

    const std::optional<Tsunami::TargetInterfacePlan> plan =
        Tsunami::plan_target_interface(input);
    REQUIRE(plan.has_value());
    CHECK(plan->target_id == input.target.id);
    CHECK(plan->support_base_layer == 17);
    REQUIRE(plan->layers.size() == 3);
    for (size_t index = 0; index < plan->layers.size(); ++index) {
        const Tsunami::TargetInterfaceLayer &layer = plan->layers[index];
        CHECK(layer.layer_index == 18 + index);
        CHECK(layer.interface_number == 3 - index);
        CHECK(diff_ex(layer.regions, input.target.region).empty());
        CHECK(diff_ex(input.target.region, layer.regions).empty());
        REQUIRE(layer.regions.size() == 1);
        CHECK(layer.regions.front().holes.size() == 1);
    }
}

TEST_CASE("Tsunami target interface refuses unsupported, colliding, and under-height stacks",
          "[TsunamiSupport][Interface][FailureIsolation]")
{
    Tsunami::TargetInterfaceInput input;
    input.target = support_target(32, 20, 4.2, 0., 0., 8., 6.);
    input.interface_layer_count = 3;
    input.lower_support_footprint = ExPolygons { ExPolygon(Polygon {
        Point(scale_(-0.5), scale_(-0.5)), Point(scale_(0.5), scale_(-0.5)),
        Point(scale_(0.5), scale_(0.5)), Point(scale_(-0.5), scale_(0.5))
    }) };
    input.maximum_bridge_distance = 0.5;
    input.blocked_region_by_layer.resize(input.target.layer_index + 1);
    CHECK_FALSE(Tsunami::plan_target_interface(input).has_value());

    input.lower_support_footprint = offset_ex(input.target.region, scale_(0.8), ClipperLib::jtRound);
    input.maximum_bridge_distance = 0.;
    input.blocked_region_by_layer[19] = input.target.region;
    CHECK_FALSE(Tsunami::plan_target_interface(input).has_value());

    input.blocked_region_by_layer[19].clear();
    input.interface_layer_count = input.target.layer_index + 1;
    CHECK_FALSE(Tsunami::plan_target_interface(input).has_value());
}

TEST_CASE("Tsunami straight ribs are immutable after birth", "[TsunamiSupport]")
{
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_straight_branch(basic_input());
    REQUIRE(plan.layers.size() == 40);

    for (const Tsunami::PhysicalRib &final_rib : plan.layers.back().physical_ribs) {
        for (size_t layer = final_rib.birth_layer; layer < plan.layers.size(); ++layer) {
            const auto &ribs = plan.layers[layer].physical_ribs;
            const auto found = std::find_if(ribs.begin(), ribs.end(), [&](const Tsunami::PhysicalRib &rib) {
                return rib.id == final_rib.id;
            });
            REQUIRE(found != ribs.end());
            CHECK(found->origin == final_rib.origin);
            CHECK(found->direction.isApprox(final_rib.direction));
        }
    }
}

TEST_CASE("Tsunami lateral propagation exists only in turn segments", "[TsunamiSupport]")
{
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_straight_branch(basic_input());
    const Vec2d guide = plan.rib_field.guide_direction;
    bool saw_lateral_turn = false;

    for (const Tsunami::LayerPlan &layer : plan.layers) {
        for (const Tsunami::PathSegment &segment : layer.segments) {
            REQUIRE(segment.polyline.points.size() >= 2);
            const Point delta = segment.polyline.points.back() - segment.polyline.points.front();
            const double lateral = Vec2d(double(delta.x()), double(delta.y())).dot(guide);
            if (segment.kind == Tsunami::SegmentKind::StraightRib)
                CHECK(std::abs(lateral) < 0.5);
            else if (std::abs(lateral) > 0.5)
                saw_lateral_turn = true;
        }
    }
    CHECK(saw_lateral_turn);
}

TEST_CASE("Tsunami virtual rib field survives a root too narrow for physical ribs", "[TsunamiSupport]")
{
    Tsunami::StraightBranchInput input = basic_input();
    input.rib_length = 0.5;
    input.minimum_physical_rib_length = 1.;
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_straight_branch(input);

    REQUIRE_FALSE(plan.layers.empty());
    for (const Tsunami::LayerPlan &layer : plan.layers)
        CHECK(layer.physical_ribs.empty());
    CHECK(plan.rib_field.rib_origin(3) == Point(scale_(6.), scale_(0.)));
    CHECK(plan.rib_field.rib_direction.isApprox(Vec2d(0., 1.)));
    CHECK(plan.rib_field.nearest_index(Point(scale_(5.9), scale_(0.))) == 3);
}

TEST_CASE("Tsunami trunk keeps identical XY geometry", "[TsunamiSupport]")
{
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_straight_branch(basic_input());
    REQUIRE(plan.layers.size() >= 2);
    CHECK(plan.layers[0].path.points == plan.layers[1].path.points);
}

TEST_CASE("Tsunami branch angle controls only active turn XY growth", "[TsunamiSupport]")
{
    const Tsunami::StraightBranchInput input = basic_input();
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_straight_branch(input);
    REQUIRE(plan.layers.size() > 2);
    REQUIRE(plan.layers[2].has_active_turn);

    const Point endpoint = plan.layers[2].path.points.back();
    const Vec2d endpoint_scaled(double(endpoint.x() - input.root.x()), double(endpoint.y() - input.root.y()));
    const double growth = unscale_(endpoint_scaled.dot(plan.rib_field.guide_direction));
    const double expected = input.rib_spacing + input.layer_height * std::tan(M_PI / 4.);
    CHECK(std::abs(growth - expected) < 0.00001);
}

TEST_CASE("Tsunami clamps aggressive turn growth by extrusion support ratio", "[TsunamiSupport][Phase2]")
{
    Tsunami::StraightBranchInput input = basic_input();
    input.trunk_height = 0.;
    input.branch_angle = 75.;
    input.extrusion_width = 0.4;
    input.minimum_layer_support_ratio = 0.5;
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_straight_branch(input);

    REQUIRE_FALSE(plan.layers.empty());
    CHECK(std::abs(plan.layers.front().applied_growth - 0.2) < 0.00001);
    CHECK(plan.layers.front().estimated_support_ratio + 1e-9 >= 0.5);
}

TEST_CASE("Tsunami raises too-small rib spacing to its printable turn diameter", "[TsunamiSupport][Phase2]")
{
    Tsunami::StraightBranchInput input = basic_input();
    input.rib_spacing = 0.2;
    input.extrusion_width = 0.6;
    input.minimum_turn_radius = 0.3;
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_straight_branch(input);

    CHECK(std::abs(plan.rib_field.spacing - 0.6) < 0.00001);
}

TEST_CASE("Tsunami delays turn growth until its anchor rib is long enough", "[TsunamiSupport][Phase2]")
{
    Tsunami::StraightBranchInput input = basic_input();
    input.trunk_height = 0.;
    input.minimum_anchor_length = input.rib_length + 1.;
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_straight_branch(input);

    REQUIRE_FALSE(plan.layers.empty());
    for (const Tsunami::LayerPlan &layer : plan.layers)
        CHECK(std::abs(layer.applied_growth) < 0.00001);
}

TEST_CASE("Tsunami straight branch does not catch up past its angle after an anchor delay",
          "[TsunamiSupport][Phase2][BranchAngle]")
{
    Tsunami::StraightBranchInput input = basic_input();
    input.trunk_height = 0.;
    input.minimum_anchor_length = input.rib_length;
    input.available_rib_length_by_layer.assign(input.layer_count, input.rib_length);
    input.available_rib_length_by_layer[0] = 0.5;
    input.available_rib_length_by_layer[1] = 0.5;
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_straight_branch(input);

    const double maximum_growth = Tsunami::maximum_lateral_growth(
        input.layer_height, input.branch_angle);
    REQUIRE(plan.layers.size() == input.layer_count);
    CHECK(std::abs(plan.layers[0].applied_growth) < 1e-9);
    CHECK(std::abs(plan.layers[1].applied_growth) < 1e-9);
    CHECK(plan.layers[2].applied_growth > 1e-6);
    for (const Tsunami::LayerPlan &layer : plan.layers)
        CHECK(layer.applied_growth <= maximum_growth + 2e-6);
}

TEST_CASE("Tsunami physical rib activation uses a retention dead band", "[TsunamiSupport][Phase2]")
{
    Tsunami::StraightBranchInput input = basic_input();
    input.trunk_height = 1000.;
    input.activation_threshold = 2.;
    input.retention_threshold = 1.5;
    input.available_rib_length_by_layer = { 2.1, 1.8, 1.6, 1.4, 1.6, 2.1 };
    input.layer_count = input.available_rib_length_by_layer.size();
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_straight_branch(input);

    REQUIRE(plan.layers.size() == 6);
    CHECK(plan.layers[0].physical_ribs.size() == 2);
    CHECK(plan.layers[1].physical_ribs.size() == 2);
    CHECK(plan.layers[2].physical_ribs.size() == 2);
    CHECK(plan.layers[3].physical_ribs.empty());
    CHECK(plan.layers[4].physical_ribs.empty());
    REQUIRE(plan.layers[5].physical_ribs.size() == 2);
    CHECK(plan.layers[5].physical_ribs.front().birth_layer == 0);
}

TEST_CASE("Tsunami root follows the solid model contour without crossing it", "[TsunamiSupport][Phase2]")
{
    Tsunami::RootSelectionInput input = circular_root_input();

    const auto first = Tsunami::select_root_candidate(input);
    const auto second = Tsunami::select_root_candidate(input);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->position == second->position);
    CHECK(first->path.points == second->path.points);
    REQUIRE(first->physical_ribs.size() >= 3);
    CHECK(first->bed_contact_area >= input.minimum_bed_contact_area);

    const double expected_length = first->physical_ribs.front().length;
    for (const Tsunami::PhysicalRib &rib : first->physical_ribs) {
        CHECK(std::abs(rib.length - expected_length) < 0.00001);
        Polyline centerline;
        const Vec2d origin(unscale<double>(rib.origin.x()), unscale<double>(rib.origin.y()));
        const Vec2d half = 0.5 * rib.length * rib.direction;
        centerline.points = {
            Point(scale_(origin.x() - half.x()), scale_(origin.y() - half.y())),
            Point(scale_(origin.x() + half.x()), scale_(origin.y() + half.y()))
        };
        const Polygons footprint = offset(centerline, float(scale_(0.5 * input.extrusion_width)), ClipperLib::jtRound);
        CHECK(intersection_ex(footprint, input.blocked_region).empty());
    }
    for (size_t segment_index = 1; segment_index + 1 < first->segments.size(); ++segment_index) {
        if (first->segments[segment_index].kind != Tsunami::SegmentKind::Turn)
            continue;
        const Polyline &previous = first->segments[segment_index - 1].polyline;
        const Polyline &turn = first->segments[segment_index].polyline;
        const Polyline &next = first->segments[segment_index + 1].polyline;
        REQUIRE(previous.points.size() >= 2);
        REQUIRE(turn.points.size() >= 3);
        REQUIRE(next.points.size() >= 2);
        const Vec2d previous_direction = (previous.points.back() - previous.points[previous.points.size() - 2])
                                             .cast<double>().normalized();
        const Vec2d turn_start_direction = (turn.points[1] - turn.points[0]).cast<double>().normalized();
        const Vec2d turn_end_direction = (turn.points.back() - turn.points[turn.points.size() - 2])
                                             .cast<double>().normalized();
        const Vec2d next_direction = (next.points[1] - next.points[0]).cast<double>().normalized();
        CHECK(previous_direction.dot(turn_start_direction) > 0.9);
        CHECK(turn_end_direction.dot(next_direction) > 0.9);
    }

    input.maximum_bed_contact_area = 0.1;
    CHECK_FALSE(Tsunami::select_root_candidate(input).has_value());
}

TEST_CASE("Tsunami contour root ranks the same U-turn approach after rotation",
          "[TsunamiSupport][ContourRoot][Rotation]")
{
    Tsunami::RootSelectionInput input = circular_root_input();
    const std::optional<Tsunami::RootCandidate> baseline =
        Tsunami::select_root_candidate(input);
    REQUIRE(baseline.has_value());

    for (ExPolygon &region : input.blocked_region) {
        region.contour.rotate(M_PI);
        for (Polygon &hole : region.holes)
            hole.rotate(M_PI);
    }
    for (ExPolygon &region : input.target_region) {
        region.contour.rotate(M_PI);
        for (Polygon &hole : region.holes)
            hole.rotate(M_PI);
    }
    input.target = Point(-input.target.x(), -input.target.y());

    const std::optional<Tsunami::RootCandidate> rotated =
        Tsunami::select_root_candidate(input);
    REQUIRE(rotated.has_value());
    CHECK(std::abs(rotated->xy_distance - baseline->xy_distance) < 1e-6);
    CHECK(rotated->approach_point ==
          Point(-baseline->approach_point.x(), -baseline->approach_point.y()));
}

TEST_CASE("Tsunami uses a zero-offset vertical root below an unobstructed target",
          "[TsunamiSupport][DirectRoot]")
{
    Tsunami::RootSelectionInput input;
    input.bed_region = Polygon {
        Point(scale_(-40.), scale_(-30.)), Point(scale_(40.), scale_(-30.)),
        Point(scale_(40.), scale_(30.)), Point(scale_(-40.), scale_(30.))
    };
    input.blocked_region = ExPolygons { ExPolygon(Polygon {
        Point(scale_(-30.), scale_(-8.)), Point(scale_(-18.), scale_(-8.)),
        Point(scale_(-18.), scale_(8.)), Point(scale_(-30.), scale_(8.))
    }) };
    input.target_region = ExPolygons { ExPolygon(Polygon {
        Point(scale_(8.), scale_(-6.)), Point(scale_(22.), scale_(-6.)),
        Point(scale_(22.), scale_(6.)), Point(scale_(8.), scale_(6.))
    }) };
    input.target = Point(scale_(15.), scale_(0.));
    input.maximum_xy_distance = 0.;
    input.rib_spacing = 2.;
    input.rib_length = 10.;
    input.extrusion_width = 0.4;
    input.minimum_bed_contact_area = 0.;
    input.maximum_bed_contact_area = 100.;

    const std::optional<Tsunami::RootCandidate> root = Tsunami::select_root_candidate(input);
    REQUIRE(root.has_value());
    CHECK(std::abs(root->xy_distance) < 1e-9);
    REQUIRE_FALSE(root->physical_ribs.empty());
    const Polygons root_footprint = offset(
        root->path, float(scale_(0.5 * input.extrusion_width)), ClipperLib::jtRound);
    CHECK(intersection_ex(root_footprint, input.blocked_region).empty());
    CHECK(diff_ex(root_footprint, Polygons { input.bed_region }).empty());

    Tsunami::StraightBranchInput branch = basic_input();
    branch.root = root->position;
    branch.target = input.target;
    branch.branch_angle = 45.;
    branch.trunk_height = 0.;
    branch.rib_spacing = input.rib_spacing;
    branch.rib_length = root->rib_length;
    branch.extrusion_width = input.extrusion_width;
    branch.layer_count = 12;
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_contour_branch(*root, branch);

    REQUIRE(plan.reached_target);
    REQUIRE(plan.layers.size() == branch.layer_count);
    for (const Tsunami::LayerPlan &layer : plan.layers) {
        CHECK(layer.path.points == root->path.points);
        CHECK(std::abs(layer.applied_growth) < 1e-9);
    }
}

TEST_CASE("Tsunami direct roots remain deterministic across target polygon shapes",
          "[TsunamiSupport][DirectRoot][GeometryMatrix]")
{
    const Polygon bed {
        Point(scale_(-40.), scale_(-30.)), Point(scale_(40.), scale_(-30.)),
        Point(scale_(40.), scale_(30.)), Point(scale_(-40.), scale_(30.))
    };
    const ExPolygons obstacle { ExPolygon(Polygon {
        Point(scale_(-30.), scale_(-8.)), Point(scale_(-18.), scale_(-8.)),
        Point(scale_(-18.), scale_(8.)), Point(scale_(-30.), scale_(8.))
    }) };
    const std::vector<Polygon> targets {
        Polygon {
            Point(scale_(8.), scale_(-6.)), Point(scale_(22.), scale_(-6.)),
            Point(scale_(22.), scale_(6.)), Point(scale_(8.), scale_(6.))
        },
        Polygon {
            Point(scale_(15.), scale_(-8.)), Point(scale_(23.), scale_(0.)),
            Point(scale_(15.), scale_(8.)), Point(scale_(7.), scale_(0.))
        },
        Polygon {
            Point(scale_(8.), scale_(-8.)), Point(scale_(22.), scale_(-8.)),
            Point(scale_(22.), scale_(-2.)), Point(scale_(14.), scale_(-2.)),
            Point(scale_(14.), scale_(8.)), Point(scale_(8.), scale_(8.))
        },
        Polygon {
            Point(scale_(8.), scale_(-0.15)), Point(scale_(22.), scale_(-0.15)),
            Point(scale_(22.), scale_(0.15)), Point(scale_(8.), scale_(0.15))
        }
    };

    for (const Polygon &target : targets) {
        Tsunami::RootSelectionInput input;
        input.bed_region = bed;
        input.blocked_region = obstacle;
        input.target_region = ExPolygons { ExPolygon(target) };
        input.target = Point(scale_(15.), scale_(0.));
        input.maximum_xy_distance = 20.;
        input.rib_spacing = 2.;
        input.rib_length = 10.;
        input.extrusion_width = 0.4;
        input.maximum_bed_contact_area = 100.;

        const std::optional<Tsunami::RootCandidate> first = Tsunami::select_root_candidate(input);
        const std::optional<Tsunami::RootCandidate> second = Tsunami::select_root_candidate(input);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->direct_projection);
        CHECK(first->path.points == second->path.points);
        REQUIRE_FALSE(first->physical_ribs.empty());
        const double rib_length = first->physical_ribs.front().length;
        for (const Tsunami::PhysicalRib &rib : first->physical_ribs)
            CHECK(std::abs(rib.length - rib_length) < 1e-9);

        const Polygons footprint = offset(
            first->path, float(scale_(0.5 * input.extrusion_width)), ClipperLib::jtRound);
        CHECK(intersection_ex(footprint, input.blocked_region).empty());
        CHECK(diff_ex(footprint, Polygons { input.bed_region }).empty());
        CHECK_FALSE(intersection_ex(footprint, input.target_region).empty());
    }
}

TEST_CASE("Tsunami direct root clips bed edges and rejects unavailable vertical projections",
          "[TsunamiSupport][DirectRoot][Boundary]")
{
    Tsunami::RootSelectionInput input;
    input.bed_region = Polygon {
        Point(scale_(-40.), scale_(-30.)), Point(scale_(40.), scale_(-30.)),
        Point(scale_(40.), scale_(30.)), Point(scale_(-40.), scale_(30.))
    };
    input.target_region = ExPolygons { ExPolygon(Polygon {
        Point(scale_(8.), scale_(-6.)), Point(scale_(22.), scale_(-6.)),
        Point(scale_(22.), scale_(6.)), Point(scale_(8.), scale_(6.))
    }) };
    input.target = Point(scale_(15.), scale_(0.));
    input.maximum_xy_distance = 0.;
    input.rib_spacing = 2.;
    input.rib_length = 10.;
    input.extrusion_width = 0.4;
    input.maximum_bed_contact_area = 100.;

    const std::optional<Tsunami::RootCandidate> empty_obstacle = Tsunami::select_root_candidate(input);
    REQUIRE(empty_obstacle.has_value());
    CHECK(empty_obstacle->direct_projection);

    input.target_region = ExPolygons { ExPolygon(Polygon {
        Point(scale_(35.), scale_(-6.)), Point(scale_(45.), scale_(-6.)),
        Point(scale_(45.), scale_(6.)), Point(scale_(35.), scale_(6.))
    }) };
    input.target = Point(scale_(40.), scale_(0.));
    const std::optional<Tsunami::RootCandidate> bed_edge = Tsunami::select_root_candidate(input);
    REQUIRE(bed_edge.has_value());
    const Polygons edge_footprint = offset(
        bed_edge->path, float(scale_(0.5 * input.extrusion_width)), ClipperLib::jtRound);
    CHECK(diff_ex(edge_footprint, Polygons { input.bed_region }).empty());

    input.target_region = ExPolygons { ExPolygon(Polygon {
        Point(scale_(50.), scale_(-6.)), Point(scale_(60.), scale_(-6.)),
        Point(scale_(60.), scale_(6.)), Point(scale_(50.), scale_(6.))
    }) };
    input.target = Point(scale_(55.), scale_(0.));
    CHECK_FALSE(Tsunami::select_root_candidate(input).has_value());

    input.target_region = ExPolygons { ExPolygon(Polygon {
        Point(scale_(8.), scale_(-6.)), Point(scale_(22.), scale_(-6.)),
        Point(scale_(22.), scale_(6.)), Point(scale_(8.), scale_(6.))
    }) };
    input.target = Point(scale_(15.), scale_(0.));
    input.blocked_region = input.target_region;
    CHECK_FALSE(Tsunami::select_root_candidate(input).has_value());
}

TEST_CASE("Tsunami direct root supports a deterministic rotated convex polygon sweep",
          "[TsunamiSupport][DirectRoot][GeometrySweep]")
{
    Tsunami::RootSelectionInput input;
    input.bed_region = Polygon {
        Point(scale_(-40.), scale_(-30.)), Point(scale_(40.), scale_(-30.)),
        Point(scale_(40.), scale_(30.)), Point(scale_(-40.), scale_(30.))
    };
    input.blocked_region = ExPolygons { ExPolygon(Polygon {
        Point(scale_(-30.), scale_(-8.)), Point(scale_(-18.), scale_(-8.)),
        Point(scale_(-18.), scale_(8.)), Point(scale_(-30.), scale_(8.))
    }) };
    input.target = Point(scale_(15.), scale_(0.));
    input.maximum_xy_distance = 20.;
    input.rib_spacing = 1.5;
    input.rib_length = 10.;
    input.extrusion_width = 0.4;
    input.maximum_bed_contact_area = 100.;

    const std::array<std::pair<double, double>, 3> radii {{ { 7., 5. }, { 9., 2. }, { 2., 9. } }};
    const std::array<double, 3> rotations {{ 0., 0.17, 0.41 }};
    const std::array<int, 6> vertex_counts {{ 3, 4, 5, 6, 8, 12 }};
    for (const auto &[radius_x, radius_y] : radii) {
        for (double rotation : rotations) {
            for (int vertex_count : vertex_counts) {
                Polygon target;
                target.points.reserve(size_t(vertex_count));
                for (int index = 0; index < vertex_count; ++index) {
                    const double angle = rotation + 2. * M_PI * double(index) / double(vertex_count);
                    target.points.emplace_back(
                        scale_(15. + radius_x * std::cos(angle)),
                        scale_(radius_y * std::sin(angle)));
                }
                input.target_region = ExPolygons { ExPolygon(std::move(target)) };

                const std::optional<Tsunami::RootCandidate> root = Tsunami::select_root_candidate(input);
                REQUIRE(root.has_value());
                CHECK(root->direct_projection);
                REQUIRE_FALSE(root->physical_ribs.empty());
                const double rib_length = root->physical_ribs.front().length;
                for (const Tsunami::PhysicalRib &rib : root->physical_ribs)
                    CHECK(std::abs(rib.length - rib_length) < 1e-9);
                const Polygons footprint = offset(
                    root->path, float(scale_(0.5 * input.extrusion_width)), ClipperLib::jtRound);
                CHECK(intersection_ex(footprint, input.blocked_region).empty());
                CHECK(diff_ex(footprint, Polygons { input.bed_region }).empty());
            }
        }
    }
}

TEST_CASE("Tsunami direct root survives a deterministic arbitrary-shape stress corpus",
          "[TsunamiSupport][DirectRoot][GeometryStress]")
{
    Tsunami::RootSelectionInput input;
    input.bed_region = Polygon {
        Point(scale_(-50.), scale_(-50.)), Point(scale_(50.), scale_(-50.)),
        Point(scale_(50.), scale_(50.)), Point(scale_(-50.), scale_(50.))
    };
    input.blocked_region = ExPolygons { ExPolygon(Polygon {
        Point(scale_(-48.), scale_(-4.)), Point(scale_(-42.), scale_(-4.)),
        Point(scale_(-42.), scale_(4.)), Point(scale_(-48.), scale_(4.))
    }) };
    input.maximum_xy_distance = 30.;
    input.rib_spacing = 1.5;
    input.rib_length = 14.;
    input.extrusion_width = 0.4;
    input.maximum_bed_contact_area = 500.;

    const auto validate_root = [&](const Tsunami::RootCandidate &root, const Point &target) {
        CHECK(root.direct_projection);
        CHECK(std::abs(root.xy_distance) < 1e-9);
        REQUIRE_FALSE(root.path.points.empty());
        REQUIRE_FALSE(root.physical_ribs.empty());
        REQUIRE(root.segments.size() == 2 * root.physical_ribs.size() - 1);
        for (size_t segment_index = 0; segment_index < root.segments.size(); ++segment_index) {
            const Tsunami::PathSegment &segment = root.segments[segment_index];
            REQUIRE(segment.polyline.points.size() >= 2);
            CHECK(segment.kind == (segment_index % 2 == 0
                ? Tsunami::SegmentKind::StraightRib : Tsunami::SegmentKind::Turn));
            if (segment_index + 1 < root.segments.size())
                CHECK(segment.polyline.points.back() == root.segments[segment_index + 1].polyline.points.front());
        }
        const double rib_length = root.physical_ribs.front().length;
        for (size_t rib_index = 0; rib_index < root.physical_ribs.size(); ++rib_index) {
            const Tsunami::PhysicalRib &rib = root.physical_ribs[rib_index];
            CHECK(std::abs(rib.length - rib_length) < 1e-9);
            const Polyline &straight = root.segments[2 * rib_index].polyline;
            const Vec2d direction = (straight.points.back() - straight.points.front()).cast<double>().normalized();
            CHECK(std::abs(direction.dot(rib.direction)) > 0.999);
        }
        const Polygons footprint = offset(
            root.path, float(scale_(0.5 * input.extrusion_width)), ClipperLib::jtRound);
        CHECK(intersection_ex(footprint, input.blocked_region).empty());
        CHECK(diff_ex(footprint, Polygons { input.bed_region }).empty());
        CHECK_FALSE(intersection_ex(footprint, input.target_region).empty());

        Tsunami::StraightBranchInput branch = basic_input();
        branch.root = root.position;
        branch.target = target;
        branch.branch_angle = 67.;
        branch.trunk_height = 0.;
        branch.rib_spacing = input.rib_spacing;
        branch.rib_length = root.rib_length;
        branch.extrusion_width = input.extrusion_width;
        branch.layer_count = 7;
        const Tsunami::StraightBranchPlan plan = Tsunami::plan_contour_branch(root, branch);
        REQUIRE(plan.reached_target);
        REQUIRE(plan.layers.size() == branch.layer_count);
        for (const Tsunami::LayerPlan &layer : plan.layers) {
            CHECK(layer.path.points == root.path.points);
            CHECK(layer.physical_ribs.size() == root.physical_ribs.size());
            CHECK_FALSE(layer.has_active_turn);
            CHECK(std::abs(layer.applied_growth) < 1e-9);
        }
    };

    constexpr uint32_t radial_case_count = 384;
    for (uint32_t case_index = 0; case_index < radial_case_count; ++case_index) {
        INFO("radial case " << case_index);
        uint32_t state = 0x9e3779b9u ^ (case_index * 0x85ebca6bu);
        auto next_unit = [&]() {
            state = state * 1664525u + 1013904223u;
            return double(state & 0x00ffffffu) / double(0x01000000u);
        };
        const int vertex_count = 3 + int(case_index % 30);
        const double center_x = -18. + 36. * next_unit();
        const double center_y = -18. + 36. * next_unit();
        double radius_x = 2. + 12. * next_unit();
        double radius_y = 0.12 + 10. * next_unit();
        if (case_index % 2 == 1)
            std::swap(radius_x, radius_y);
        const double rotation = 2. * M_PI * next_unit();
        Polygon target_polygon;
        target_polygon.points.reserve(size_t(vertex_count));
        for (int vertex = 0; vertex < vertex_count; ++vertex) {
            const double angle = rotation + 2. * M_PI * double(vertex) / double(vertex_count);
            const double radial_scale = 0.35 + 0.65 * next_unit();
            target_polygon.points.emplace_back(
                scale_(center_x + radial_scale * radius_x * std::cos(angle)),
                scale_(center_y + radial_scale * radius_y * std::sin(angle)));
        }
        input.target_region = ExPolygons { ExPolygon(std::move(target_polygon)) };
        input.target = Point(scale_(center_x), scale_(center_y));

        const std::optional<Tsunami::RootCandidate> first = Tsunami::select_root_candidate(input);
        const std::optional<Tsunami::RootCandidate> second = Tsunami::select_root_candidate(input);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->path.points == second->path.points);
        CHECK(first->physical_ribs.size() == second->physical_ribs.size());
        validate_root(*first, input.target);
    }

    size_t printable_annuli = 0;
    constexpr uint32_t annular_case_count = 96;
    for (uint32_t case_index = 0; case_index < annular_case_count; ++case_index) {
        INFO("annular case " << case_index);
        const int segments = 24 + int(case_index % 5) * 8;
        const double outer_radius = 2. + 0.12 * double(case_index);
        const double wall = 0.18 + 0.08 * double(case_index % 12);
        const double inner_radius = std::max(0.2, outer_radius - wall);
        const double center_x = -12. + 8. * double(case_index % 4);
        const double center_y = -12. + 8. * double((case_index / 4) % 4);
        const double rotation = 0.07 * double(case_index % 11);
        Polygon outer;
        Polygon hole;
        outer.points.reserve(size_t(segments));
        hole.points.reserve(size_t(segments));
        for (int vertex = 0; vertex < segments; ++vertex) {
            const double angle = rotation + 2. * M_PI * double(vertex) / double(segments);
            outer.points.emplace_back(scale_(center_x + outer_radius * std::cos(angle)),
                                      scale_(center_y + outer_radius * std::sin(angle)));
            hole.points.emplace_back(scale_(center_x + inner_radius * std::cos(angle)),
                                     scale_(center_y + inner_radius * std::sin(angle)));
        }
        std::reverse(hole.points.begin(), hole.points.end());
        ExPolygon annulus(std::move(outer));
        annulus.holes.emplace_back(std::move(hole));
        input.target_region = ExPolygons { std::move(annulus) };
        input.target = Point(scale_(center_x), scale_(center_y));

        const std::optional<Tsunami::RootCandidate> first = Tsunami::select_root_candidate(input);
        const std::optional<Tsunami::RootCandidate> second = Tsunami::select_root_candidate(input);
        REQUIRE(first.has_value() == second.has_value());
        if (!first)
            continue;
        ++printable_annuli;
        CHECK(first->path.points == second->path.points);
        validate_root(*first, input.target);
    }
    CHECK(printable_annuli >= 72);
}

TEST_CASE("Tsunami contour root transitions through one immutable anchor rib", "[TsunamiSupport][ContourRoot]")
{
    const Tsunami::RootSelectionInput root_input = circular_root_input();
    const std::optional<Tsunami::RootCandidate> root = Tsunami::select_root_candidate(root_input);
    REQUIRE(root.has_value());

    Tsunami::StraightBranchInput branch = basic_input();
    branch.root = root->position;
    branch.target = root_input.target;
    branch.trunk_height = 0.4;
    branch.rib_spacing = root_input.rib_spacing;
    branch.rib_length = root->rib_length;
    branch.extrusion_width = root_input.extrusion_width;
    branch.minimum_anchor_length = root->rib_length;
    branch.layer_count = 30;
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_contour_branch(*root, branch);

    REQUIRE(plan.layers.size() == branch.layer_count);
    REQUIRE(plan.layers[0].path.points == root->path.points);
    REQUIRE(plan.layers[1].path.points == root->path.points);
    REQUIRE_FALSE(plan.layers[2].physical_ribs.empty());
    CHECK(plan.layers[2].physical_ribs.front().origin == root->position);
    REQUIRE(root->frontier_rib_index < root->physical_ribs.size());
    CHECK(plan.layers[2].physical_ribs.front().direction.isApprox(
        root->physical_ribs[root->frontier_rib_index].direction));
    REQUIRE(plan.layers[2].segments.size() >= 2);
    CHECK(plan.layers[2].segments.front().kind == Tsunami::SegmentKind::StraightRib);
    CHECK(plan.layers[2].segments[1].kind == Tsunami::SegmentKind::Turn);
    CHECK(plan.layers[2].segments.front().polyline.points.back() == root->frontier_point);
    CHECK(plan.layers[2].segments[1].polyline.points.front() == root->frontier_point);
}

TEST_CASE("Tsunami guided ribs never move or rotate after birth", "[TsunamiSupport][ContourRoot]")
{
    const Tsunami::RootSelectionInput root_input = circular_root_input();
    const std::optional<Tsunami::RootCandidate> root = Tsunami::select_root_candidate(root_input);
    REQUIRE(root.has_value());
    Tsunami::StraightBranchInput branch = basic_input();
    branch.root = root->position;
    branch.target = root_input.target;
    branch.trunk_height = 0.4;
    branch.rib_spacing = root_input.rib_spacing;
    branch.rib_length = root->rib_length;
    branch.extrusion_width = root_input.extrusion_width;
    branch.minimum_anchor_length = root->rib_length;
    branch.layer_count = 80;
    const Tsunami::StraightBranchPlan plan = Tsunami::plan_contour_branch(*root, branch);
    CHECK(plan.reached_target);
    REQUIRE_FALSE(plan.layers.back().physical_ribs.empty());

    for (const Tsunami::PhysicalRib &final_rib : plan.layers.back().physical_ribs) {
        for (size_t layer_index = std::max<size_t>(2, final_rib.birth_layer); layer_index < plan.layers.size(); ++layer_index) {
            const auto &ribs = plan.layers[layer_index].physical_ribs;
            const auto found = std::find_if(ribs.begin(), ribs.end(), [&](const Tsunami::PhysicalRib &rib) {
                return rib.id == final_rib.id;
            });
            REQUIRE(found != ribs.end());
            CHECK(found->origin == final_rib.origin);
            CHECK(found->direction.isApprox(final_rib.direction));
        }
    }
}

TEST_CASE("Tsunami contour branch reports when available height cannot reach its target",
          "[TsunamiSupport][ContourRoot][Reachability]")
{
    const Tsunami::RootSelectionInput root_input = circular_root_input();
    const std::optional<Tsunami::RootCandidate> root = Tsunami::select_root_candidate(root_input);
    REQUIRE(root.has_value());

    Tsunami::StraightBranchInput branch = basic_input();
    branch.root = root->position;
    branch.target = root_input.target;
    branch.trunk_height = 0.4;
    branch.rib_spacing = root_input.rib_spacing;
    branch.rib_length = root->rib_length;
    branch.extrusion_width = root_input.extrusion_width;
    branch.minimum_anchor_length = root->rib_length;
    branch.layer_count = 3;

    const Tsunami::StraightBranchPlan plan = Tsunami::plan_contour_branch(*root, branch);
    REQUIRE(plan.layers.size() == branch.layer_count);
    CHECK_FALSE(plan.reached_target);
}

TEST_CASE("Tsunami guided frontier waits at model collision and resumes after clearance",
          "[TsunamiSupport][ContourRoot][Collision]")
{
    const Tsunami::RootSelectionInput root_input = circular_root_input();
    const std::optional<Tsunami::RootCandidate> root = Tsunami::select_root_candidate(root_input);
    REQUIRE(root.has_value());

    Tsunami::StraightBranchInput branch = basic_input();
    branch.root = root->position;
    branch.target = root_input.target;
    branch.trunk_height = 0.4;
    branch.rib_spacing = root_input.rib_spacing;
    branch.rib_length = root->rib_length;
    branch.extrusion_width = root_input.extrusion_width;
    branch.minimum_anchor_length = root->rib_length;
    branch.layer_count = 80;
    branch.blocked_region_by_layer.resize(branch.layer_count);
    for (size_t layer_index = 0; layer_index < 35; ++layer_index)
        branch.blocked_region_by_layer[layer_index] = root_input.blocked_region;

    const Tsunami::StraightBranchPlan plan = Tsunami::plan_contour_branch(*root, branch);
    REQUIRE(plan.layers.size() == branch.layer_count);
    bool saw_wait = false;
    bool saw_resume = false;
    const double maximum_growth = Tsunami::maximum_lateral_growth(
        branch.layer_height, branch.branch_angle);
    for (size_t layer_index = 0; layer_index < plan.layers.size(); ++layer_index) {
        const Tsunami::LayerPlan &layer = plan.layers[layer_index];
        CHECK(layer.applied_growth <= maximum_growth + 2e-6);
        if (layer_index >= 2 && layer_index < 35 && std::abs(layer.applied_growth) < 1e-9)
            saw_wait = true;
        if (layer_index >= 35 && layer.applied_growth > 1e-6)
            saw_resume = true;
        if (!branch.blocked_region_by_layer[layer_index].empty()) {
            const Polygons footprint = offset(
                layer.path, float(scale_(0.5 * branch.extrusion_width)), ClipperLib::jtRound);
            CHECK(intersection_ex(footprint, branch.blocked_region_by_layer[layer_index]).empty());
        }
    }
    CHECK(saw_wait);
    CHECK(saw_resume);
}

TEST_CASE("Tsunami concave footprint splits invalid contour runs instead of drawing a chord",
          "[TsunamiSupport][ContourRoot][Concave]")
{
    Tsunami::RootSelectionInput input;
    input.bed_region = Polygon {
        Point(scale_(-30.), scale_(-30.)), Point(scale_(30.), scale_(-30.)),
        Point(scale_(30.), scale_(30.)), Point(scale_(-30.), scale_(30.))
    };
    Polygon concave {
        Point(scale_(-10.), scale_(-10.)), Point(scale_(10.), scale_(-10.)),
        Point(scale_(10.), scale_(0.)), Point(scale_(0.), scale_(0.)),
        Point(scale_(0.), scale_(10.)), Point(scale_(-10.), scale_(10.))
    };
    input.blocked_region = ExPolygons { ExPolygon(std::move(concave)) };
    Polygon target {
        Point(scale_(2.), scale_(2.)), Point(scale_(8.), scale_(2.)),
        Point(scale_(8.), scale_(8.)), Point(scale_(2.), scale_(8.))
    };
    input.target_region = ExPolygons { ExPolygon(std::move(target)) };
    input.target = Point(scale_(5.), scale_(5.));
    input.maximum_xy_distance = 20.;
    input.rib_spacing = 2.;
    input.rib_length = 5.;
    input.extrusion_width = 0.4;
    input.maximum_bed_contact_area = 100.;

    const std::optional<Tsunami::RootCandidate> root = Tsunami::select_root_candidate(input);
    REQUIRE(root.has_value());
    REQUIRE(root->physical_ribs.size() >= 2);
    const Polygons root_footprint = offset(
        root->path, float(scale_(0.5 * input.extrusion_width)), ClipperLib::jtRound);
    CHECK(intersection_ex(root_footprint, input.blocked_region).empty());
    for (const Tsunami::PathSegment &segment : root->segments) {
        if (segment.kind != Tsunami::SegmentKind::StraightRib)
            continue;
        const Polygons rib_footprint = offset(
            segment.polyline, float(scale_(0.5 * input.extrusion_width)), ClipperLib::jtRound);
        CHECK(intersection_ex(rib_footprint, input.blocked_region).empty());
    }
}

TEST_CASE("Tsunami narrow bed arc remains deterministic at contact-area limits",
          "[TsunamiSupport][ContourRoot][Boundary]")
{
    Tsunami::RootSelectionInput input = circular_root_input();
    input.bed_region = Polygon {
        Point(scale_(20.), scale_(-4.)), Point(scale_(26.), scale_(-4.)),
        Point(scale_(26.), scale_(4.)), Point(scale_(20.), scale_(4.))
    };
    input.target_region = ExPolygons { annular_sector(16., 18., -0.08, 0.08) };
    input.target = Point(scale_(18.), scale_(0.));
    input.rib_length = 4.;
    input.minimum_bed_contact_area = 0.;
    input.maximum_bed_contact_area = 100.;

    const std::optional<Tsunami::RootCandidate> baseline = Tsunami::select_root_candidate(input);
    REQUIRE(baseline.has_value());
    const double area = baseline->bed_contact_area;

    input.minimum_bed_contact_area = area - 1e-6;
    input.maximum_bed_contact_area = area + 1e-6;
    const std::optional<Tsunami::RootCandidate> threshold = Tsunami::select_root_candidate(input);
    REQUIRE(threshold.has_value());
    CHECK(threshold->path.points == baseline->path.points);

    input.minimum_bed_contact_area = area + 1e-3;
    input.maximum_bed_contact_area = area - 1e-3;
    CHECK_FALSE(Tsunami::select_root_candidate(input).has_value());
}
