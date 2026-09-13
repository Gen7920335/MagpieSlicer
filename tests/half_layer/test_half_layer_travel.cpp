#include <catch2/catch_all.hpp>
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/HalfLayerExecution.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/HalfLayerSources.hpp"
#include "fff_print/test_helpers.hpp"
#include <array>
#include <limits>
#include <chrono>
#include <iostream>

using namespace Slic3r;

TEST_CASE("Half-layer custom Z replay preserves coordinate mode units and G92", "[HalfLayer][Travel][CustomGCode]")
{
    constexpr double tolerance_mm = 0.000001;
    double z_mm = 0.;
    CHECK(GCodeProcessor::get_last_z_from_gcode("G91\nG1 Z1.5\nG90\n", 2.29, z_mm));
    CHECK(z_mm == Catch::Approx(3.79).margin(tolerance_mm));
    CHECK(GCodeProcessor::get_last_z_from_gcode("G90\nG1 Z3.79\n", 2.29, z_mm));
    CHECK(z_mm == Catch::Approx(3.79).margin(tolerance_mm));
    CHECK(GCodeProcessor::get_last_z_from_gcode("G20\nG91\nG1 Z0.1\nG21\n", 2., z_mm));
    CHECK(z_mm == Catch::Approx(4.54).margin(tolerance_mm));
    CHECK(GCodeProcessor::get_last_z_from_gcode("G92 Z0\nG90\nG1 Z1.5", 2., z_mm));
    CHECK(z_mm == Catch::Approx(3.5).margin(tolerance_mm));
    CHECK_FALSE(GCodeProcessor::get_last_z_from_gcode("G91\nM400\n", 2., z_mm));
}

TEST_CASE("Half-layer estimates share nozzle material and role speed limits", "[HalfLayer][ExecutionTiming]")
{
    FullPrintConfig config;
    config.outer_wall_speed.values = {100., 40.};
    config.filament_flow_ratio.values = {1., 0.8};
    config.filament_max_volumetric_speed.values = {20., 2.};
    config.filament_adaptive_volumetric_speed.values = {false, false};
    config.print_flow_ratio.value = 1.25;
    config.set_other_flow_ratios.value = true;
    config.outer_wall_flow_ratio.value = 0.5;
    config.slow_down_layers.value = 0;
    config.crisp_corner_small_nozzle_wall_speed.values = {{50., true}, {12., false}};
    ExtrusionPath path(erExternalPerimeter, 0.1, 0.5f, 0.2f);
    path.polyline.points = {Point3(scale_(0.), scale_(0.), 0.), Point3(scale_(100.), scale_(0.), 0.)};
    for (size_t nozzle : {size_t(0), size_t(1)}) {
        for (unsigned filament : {0u, 1u}) {
            const ExtrusionSpeedContext context{nozzle, filament, 5, false, false, false};
            const double area_mm2 = filament == 0 ? 0.0625 : 0.05;
            const double nominal_mm_s = nozzle == 0 ? 100. : 40.;
            const double volume_mm3_s = filament == 0 ? 20. : 2.;
            const auto speed = extrusion_base_speed(config, path, context);
            CHECK(speed.effective_mm3_per_mm == Catch::Approx(area_mm2));
            CHECK(half_layer_entity_metrics(path, config, context).volume_mm3 == Catch::Approx(100. * area_mm2));
            CHECK(speed.speed_mm_s == Catch::Approx(std::min(nominal_mm_s, volume_mm3_s / area_mm2)));
            CHECK(half_layer_entity_seconds(path, config, context) == Catch::Approx(100. / speed.speed_mm_s));
            path.tool_hint = ExtrusionToolHint::DetailWall;
            const double detail_mm_s = nozzle == 0 ? 50. : 12.;
            CHECK(half_layer_entity_seconds(path, config, context) ==
                  Catch::Approx(100. / std::min(detail_mm_s, volume_mm3_s / area_mm2)));
            path.tool_hint = ExtrusionToolHint::Auto;
        }
    }
}

TEST_CASE("Purge frame validation rejects corrupt ranges and nonprefix credits", "[HalfLayer][FrameIntegrity]")
{
    ExtrusionPath path(erInternalInfill, 0.1, 0.5f, 0.2f);
    std::array<ExtrusionEntityCollection, 2> roots;
    HalfLayerExecutionFrame frame;
    for (size_t i = 0; i < 2; ++i) {
        HalfLayerExecutionTask task;
        task.id = i;
        task.entity = &path;
        task.overrides_key = &roots[i];
        task.tool = {0, 1};
        initialize_half_layer_normal_owner(task);
        task.wipe_into_eligible = true;
        task.wipe_into_volume_mm3 = 2.;
        frame.plan.tasks.push_back(task);
    }
    rebuild_half_layer_visits(frame.plan);
    HalfLayerWipeIntoContext context;
    context.filament_count = 2;
    context.loaded_filament_by_tool = {0};
    context.purge_volume_mm3_by_tool = {{0., 2., 2., 0.}};
    context.transition_claimable_filament = {true, true};
    apply_half_layer_wipe_into_claims(frame.plan, context, true);
    REQUIRE(frame.valid());
    SECTION("nonprefix task") { frame.plan.purge_transactions[0].claimed_task_ids = {1}; }
    SECTION("NaN capacity") { frame.plan.tasks[0].wipe_into_volume_mm3 = std::numeric_limits<double>::quiet_NaN(); }
    SECTION("orphan transaction") { frame.plan.purge_transactions.push_back(frame.plan.purge_transactions[0]); }
    SECTION("out of range task") {
        frame.plan.visits[0].end = 3;
        frame.plan.purge_transactions[0].claimed_task_ids = {2};
    }
    SECTION("negative capacity") { frame.plan.tasks[0].wipe_into_volume_mm3 = -2.; }
    SECTION("partial root") { frame.plan.tasks[1].overrides_key = &roots[0]; }
    CHECK_FALSE(frame.valid());
}

TEST_CASE("Wipe suffix selection uses destination flow and restores normal capacity", "[HalfLayer][PurgeFlowAccounting]")
{
    const double destination_scale = GENERATE(0.25, 2.);
    std::array<ExtrusionEntityCollection, 4> roots;
    std::vector<HalfLayerExecutionTask> tasks(4);
    std::vector<HalfLayerExecutionTask *> stream;
    for (size_t i = 0; i < tasks.size(); ++i) {
        tasks[i].overrides_key = &roots[i];
        tasks[i].tool = {0, i == 3 ? 1u : 0u};
        tasks[i].wipe_into_eligible = i == 1 || i == 2;
        tasks[i].wipe_into_volume_mm3 = 4.;
        stream.push_back(&tasks[i]);
    }
    HalfLayerWipeIntoContext context;
    context.filament_count = 2;
    context.loaded_filament_by_tool = {0};
    context.purge_volume_mm3_by_tool = {{0., 6., 6., 0.}};
    context.transition_claimable_filament = {true, true};
    size_t retarget_calls = 0;
    REQUIRE(reserve_half_layer_cross_material_wipe(stream, context, true,
        [&](HalfLayerExecutionTask &task, unsigned destination) {
            task.tool.filament = destination;
            task.wipe_into_volume_mm3 *= destination_scale;
            ++retarget_calls;
        }) == 1);
    CHECK(retarget_calls == (destination_scale < 1. ? 2 : 1));
    CHECK(tasks[0].tool.filament == 0);
    CHECK(tasks[2].wipe_into_volume_mm3 == Catch::Approx(4. * destination_scale));
    restore_half_layer_normal_snapshot(tasks[2]);
    CHECK(tasks[2].wipe_into_volume_mm3 == Catch::Approx(4.));
    CHECK(tasks[2].tool.filament == 0);
}

TEST_CASE("Physical task insertion minimizes whole-path midpoint error and retains revisits", "[HalfLayer][ExecutionTiming]")
{
    const double core_seconds = GENERATE(0.1, 1., 5.);
    const double change_seconds = GENERATE(0., 2., 30.);
    ExtrusionPath path(erPerimeter, 0.1, 0.5f, 0.2f);
    auto task = [&](unsigned filament, double seconds, double x_mm, double z_mm) {
        HalfLayerExecutionTask result;
        result.entity = &path;
        result.tool = {filament, filament};
        result.extrusion_seconds = seconds;
        result.first_mm = Vec3d(x_mm, 0., z_mm);
        result.last_mm = Vec3d(x_mm + 1., 0., z_mm);
        result.travel_speed_mm_s = 100.;
        result.z_speed_mm_s = 10.;
        result.clearance_mm = 0.3;
        result.motion_reference_height = 0.2;
        result.tool_entry_seconds = change_seconds;
        return result;
    };
    std::vector<HalfLayerExecutionTask> lower{task(1, 2., 0., 1.1)};
    std::vector<HalfLayerExecutionTask> upper{task(1, 2., 0., 1.2)};
    std::vector<HalfLayerExecutionTask> core;
    for (size_t i = 0; i < 12; ++i)
        core.push_back(task(0, core_seconds, double(i) * 2., 1.2));
    const auto plan = schedule_half_layer_execution(lower, core, upper);
    REQUIRE(plan.tasks.size() == 14);
    REQUIRE(plan.upper_end == plan.upper_begin + 1);
    CHECK(plan.lower_end == 1);
    // Independent enumeration of complete execution streams, not the planner's
    // prefix sums or insertion-boundary formula.
    double minimum_error_seconds = std::numeric_limits<double>::infinity();
    size_t best_index = 0;
    for (size_t split = 0; split <= core.size(); ++split) {
        auto sequence = lower;
        sequence.insert(sequence.end(), core.begin(), core.begin() + split);
        sequence.insert(sequence.end(), upper.begin(), upper.end());
        sequence.insert(sequence.end(), core.begin() + split, core.end());
        double elapsed_seconds = 0., upper_start_seconds = 0.;
        for (size_t i = 0; i < sequence.size(); ++i) {
            if (i > 0) {
                const auto &a = sequence[i - 1];
                const auto &b = sequence[i];
                elapsed_seconds += std::abs(b.first_mm.x() - a.last_mm.x()) / 100.;
                elapsed_seconds += (std::abs(b.first_mm.z() - a.last_mm.z()) + 0.6) / 10.;
                if (a.tool.filament != b.tool.filament)
                    elapsed_seconds += change_seconds;
            }
            if (i == split + 1)
                upper_start_seconds = elapsed_seconds;
            elapsed_seconds += sequence[i].extrusion_seconds;
        }
        const double error_seconds = std::abs(upper_start_seconds - elapsed_seconds / 2.);
        if (error_seconds < minimum_error_seconds) {
            minimum_error_seconds = error_seconds;
            best_index = split + 1;
        }
    }
    CAPTURE(core_seconds, change_seconds, plan.upper_begin, best_index);
    CHECK(plan.insertion.midpoint_error_seconds == Catch::Approx(minimum_error_seconds));
    CHECK(plan.upper_begin == best_index);
    for (size_t i = 0; i < plan.visits.size(); ++i) {
        const auto &visit = plan.visits[i];
        CHECK(visit.begin < visit.end);
        for (size_t j = visit.begin; j < visit.end; ++j)
            CHECK(plan.tasks[j].tool.filament == visit.tool.filament);
        if (i > 0) {
            CHECK(plan.visits[i - 1].end == visit.begin);
            CHECK(plan.visits[i - 1].tool.filament != visit.tool.filament);
        }
    }
    if (plan.upper_begin > 1 && plan.upper_end < plan.tasks.size()) {
        REQUIRE(plan.visits.size() == 4);
        CHECK(plan.visits[0].tool.filament == 1);
        CHECK(plan.visits[1].tool.filament == 0);
        CHECK(plan.visits[2].tool.filament == 1);
        CHECK(plan.visits[3].tool.filament == 0);
    }
}

TEST_CASE("Half-layer midpoint accounts for support prefix and entry time", "[HalfLayer][MidpointCosts]")
{
    ExtrusionPath path(erPerimeter, 0.1, 0.5f, 0.2f);
    auto task = [&](unsigned filament, double seconds, double x_mm) {
        HalfLayerExecutionTask result;
        result.entity = &path;
        result.tool = {filament, filament};
        result.extrusion_seconds = seconds;
        result.first_mm = Vec3d(x_mm, 0., 1.);
        result.last_mm = Vec3d(x_mm + 1., 0., 1.);
        result.travel_speed_mm_s = 100.;
        result.z_speed_mm_s = 10.;
        result.clearance_mm = 0.3;
        result.motion_reference_height = 0.2;
        result.tool_entry_seconds = 5.;
        return result;
    };
    std::vector<HalfLayerExecutionTask> prefix{task(0, 20., -2.)};
    std::vector<HalfLayerExecutionTask> lower{task(1, 10., 0.)};
    std::vector<HalfLayerExecutionTask> core{task(1, 10., 2.), task(1, 10., 4.), task(1, 10., 6.)};
    std::vector<HalfLayerExecutionTask> upper{task(1, 10., 8.)};
    const auto plan = schedule_half_layer_execution(prefix, lower, core, upper, 5.);
    REQUIRE(plan.insertion.boundary != HalfLayerInsertion::unavailable);
    CHECK(plan.lower_end == 2);
    CHECK(plan.tasks.front().tool.filament == 0);
    CHECK(plan.insertion.second_start_seconds > 35.); // 5 s entry + 20 s support + A->B + lower wall.
    CHECK(plan.insertion.midpoint_error_seconds < 8.);
}

TEST_CASE("Half-layer midpoint uses asymmetric cached purge costs", "[HalfLayer][MidpointCosts]")
{
    ExtrusionPath path(erPerimeter, 0.1, 0.5f, 0.2f);
    auto task = [&](unsigned filament, double seconds, double x_mm) {
        HalfLayerExecutionTask result;
        result.entity = &path;
        result.tool = {0, filament};
        result.extrusion_seconds = seconds;
        result.first_mm = Vec3d(x_mm, 0., 1.);
        result.last_mm = Vec3d(x_mm + 1., 0., 1.);
        result.travel_speed_mm_s = 100.;
        result.z_speed_mm_s = 10.;
        result.motion_reference_height = 0.2;
        // A->B takes 20 s, B->A takes 1 s. The generic fallback is
        // intentionally different so this test proves the pair cache is used.
        result.tool_entry_seconds = 99.;
        result.tool_entry_seconds_by_filament = filament == 0 ?
            std::vector<double>{0., 1.} : std::vector<double>{20., 0.};
        return result;
    };
    const std::vector<HalfLayerExecutionTask> prefix{task(0, 4., -2.)};
    const std::vector<HalfLayerExecutionTask> lower{task(1, 4., 0.)};
    const std::vector<HalfLayerExecutionTask> core{
        task(0, 2., 2.), task(1, 2., 4.), task(0, 2., 6.)};
    const std::vector<HalfLayerExecutionTask> upper{task(1, 4., 8.)};
    const double initial_seconds = 1.;
    const auto plan = schedule_half_layer_execution(prefix, lower, core, upper, initial_seconds);

    auto pair_seconds = [](unsigned from, unsigned to) {
        if (from == to) return 0.;
        return from == 0 && to == 1 ? 20. : 1.;
    };
    double best_error = std::numeric_limits<double>::infinity();
    size_t best_index = 0;
    for (size_t split = 0; split <= core.size(); ++split) {
        auto sequence = prefix;
        sequence.insert(sequence.end(), lower.begin(), lower.end());
        sequence.insert(sequence.end(), core.begin(), core.begin() + split);
        const size_t upper_index = sequence.size();
        sequence.insert(sequence.end(), upper.begin(), upper.end());
        sequence.insert(sequence.end(), core.begin() + split, core.end());
        double elapsed = initial_seconds;
        double first_start = 0., second_start = 0.;
        for (size_t i = 0; i < sequence.size(); ++i) {
            if (i > 0) {
                elapsed += (sequence[i].first_mm - sequence[i - 1].last_mm).norm() / 100.;
                elapsed += pair_seconds(sequence[i - 1].tool.filament, sequence[i].tool.filament);
            }
            if (i == prefix.size()) first_start = elapsed;
            if (i == upper_index) second_start = elapsed;
            elapsed += sequence[i].extrusion_seconds;
        }
        const double error = std::abs(second_start - first_start - 0.5 * elapsed);
        if (error < best_error) {
            best_error = error;
            best_index = upper_index;
        }
    }
    CAPTURE(plan.upper_begin, best_index, plan.insertion.midpoint_error_seconds, best_error);
    CHECK(plan.upper_begin == best_index);
    CHECK(plan.insertion.midpoint_error_seconds == Catch::Approx(best_error));
    CHECK(half_layer_transition_seconds(core[0], lower[0]) >
          half_layer_transition_seconds(lower[0], core[0]));
}

TEST_CASE("Half-layer wipe-into claims only complete visit-local prefix roots",
    "[HalfLayer][WipeIntoPlan]")
{
    ExtrusionPath path(erInternalInfill, 0.1, 0.5f, 0.2f);
    std::array<ExtrusionEntityCollection, 4> roots;
    HalfLayerExecutionPlan plan;
    auto add_task = [&](unsigned physical_tool, unsigned filament, size_t root,
                        bool eligible, double volume_mm3) {
        HalfLayerExecutionTask task;
        task.id = plan.tasks.size();
        task.entity = &path;
        task.overrides_key = &roots[root];
        task.tool = {physical_tool, filament};
        task.wipe_into_eligible = eligible;
        task.wipe_into_volume_mm3 = volume_mm3;
        plan.tasks.push_back(task);
    };
    add_task(0, 0, 0, false, 2.); // Existing A visit.
    add_task(0, 1, 1, true, 3.);  // Whole two-leaf root may be claimed.
    add_task(0, 1, 1, true, 4.);
    add_task(0, 1, 2, false, 20.); // Contamination barrier.
    add_task(0, 1, 3, true, 20.);  // Must not be reached through the barrier.
    plan.visits = {
        {{0, 0}, 0, 1, false, size_t(-1)},
        {{0, 1}, 1, 5, false, size_t(-1)}
    };
    HalfLayerWipeIntoContext context;
    context.filament_count = 2;
    context.loaded_filament_by_tool = {0, -1};
    context.purge_volume_mm3_by_tool = {{0., 10., 6., 0.}, {0., 10., 6., 0.}};
    context.transition_claimable_filament = {true, true};

    apply_half_layer_wipe_into_claims(plan, context, true);
    REQUIRE(plan.purge_transactions.size() == 1);
    const HalfLayerPurgeTransaction &transaction = plan.purge_transactions.front();
    CHECK(transaction.source_filament == 0);
    CHECK(transaction.destination_filament == 1);
    CHECK(transaction.required_volume_mm3 == Catch::Approx(10.));
    CHECK(transaction.claimed_volume_mm3 == Catch::Approx(7.));
    CHECK(transaction.tower_volume_mm3 == Catch::Approx(3.));
    CHECK(transaction.claimed_task_ids == std::vector<size_t>{1, 2});
    CHECK(plan.visits[1].purge_transaction == 0);
    CHECK(context.loaded_filament_by_tool == std::vector<int>{1, -1});

    HalfLayerExecutionPlan separate_nozzle = plan;
    separate_nozzle.purge_transactions.clear();
    separate_nozzle.visits = {
        {{0, 0}, 0, 1, false, size_t(-1)},
        {{1, 1}, 1, 5, false, size_t(-1)}
    };
    HalfLayerWipeIntoContext separate_context = context;
    separate_context.loaded_filament_by_tool = {0, -1};
    apply_half_layer_wipe_into_claims(separate_nozzle, separate_context, true);
    CHECK(separate_nozzle.purge_transactions.empty());
    CHECK(separate_context.loaded_filament_by_tool == std::vector<int>{0, 1});

    HalfLayerExecutionPlan disabled = plan;
    disabled.purge_transactions.clear();
    disabled.visits[1].purge_transaction = size_t(-1);
    HalfLayerWipeIntoContext disabled_context = context;
    disabled_context.loaded_filament_by_tool = {0, -1};
    apply_half_layer_wipe_into_claims(disabled, disabled_context, false);
    REQUIRE(disabled.purge_transactions.size() == 1);
    CHECK(disabled.purge_transactions.front().claimed_volume_mm3 == 0.);
    CHECK(disabled.purge_transactions.front().tower_volume_mm3 == Catch::Approx(10.));
}

TEST_CASE("Cross-material wipe shifts only an adjacent same-nozzle boundary",
    "[HalfLayer][WipeIntoReassignment]")
{
    ExtrusionPath path(erInternalInfill, 0.1, 0.5f, 0.2f);
    std::array<ExtrusionEntityCollection, 4> roots;
    auto task = [&](unsigned physical_tool, unsigned filament, size_t root,
                    bool eligible, double volume, double seconds) {
        HalfLayerExecutionTask out;
        out.entity = &path;
        out.overrides_key = &roots[root];
        out.tool = {physical_tool, filament};
        out.extrusion_seconds = seconds;
        out.first_mm = Vec3d(double(root) * 2., 0., 1.);
        out.last_mm = out.first_mm + Vec3d(1., 0., 0.);
        out.travel_speed_mm_s = 100.;
        out.z_speed_mm_s = 10.;
        out.clearance_mm = 0.3;
        out.motion_reference_height = 0.2;
        out.tool_entry_seconds = 1.;
        out.tool_entry_seconds_by_filament = {0., 1.};
        out.wipe_into_eligible = eligible;
        out.wipe_into_volume_mm3 = volume;
        return out;
    };
    std::vector<HalfLayerExecutionTask> core{
        task(0, 0, 0, true, 10., 1.), // Eligible but behind a dependency barrier.
        task(0, 0, 1, false, 1., 1.),
        task(0, 0, 2, true, 4., 1.),
        task(0, 1, 3, false, 20., 1.)
    };
    std::vector<HalfLayerExecutionTask *> stream;
    for (HalfLayerExecutionTask &item : core) stream.push_back(&item);
    HalfLayerWipeIntoContext context;
    context.filament_count = 2;
    context.loaded_filament_by_tool = {0};
    context.purge_volume_mm3_by_tool = {{0., 6., 6., 0.}};
    context.transition_claimable_filament = {true, true};
    const size_t reservations = reserve_half_layer_cross_material_wipe(
        stream, context, true, [](HalfLayerExecutionTask &item, unsigned destination) {
            item.tool.filament = destination;
            item.extrusion_seconds = 3.;
            item.clearance_mm = 0.5;
            item.tool_entry_seconds_by_filament = {2., 0.};
        });
    REQUIRE(reservations == 1);
    CHECK(core[0].tool.filament == 0);
    CHECK(core[1].tool.filament == 0);
    CHECK(core[2].normal_tool.filament == 0);
    CHECK(core[2].tool.filament == 1);
    CHECK(core[2].extrusion_seconds == Catch::Approx(3.));
    CHECK(core[3].tool.filament == 1);

    HalfLayerExecutionPlan plan = schedule_half_layer_execution({}, {}, std::move(core), {}, 0.);
    reconcile_half_layer_cross_material_wipe(plan, context.loaded_filament_by_tool, reservations, 0);
    REQUIRE(plan.visits.size() == 2);
    CHECK(plan.visits[0].tool.filament == 0);
    CHECK(plan.visits[1].tool.filament == 1);
    CHECK(plan.tasks[2].wipe_reservation_id == 0);
    for (size_t i = 0; i < plan.tasks.size(); ++i) plan.tasks[i].id = i;
    apply_half_layer_wipe_into_claims(plan, context, true);
    REQUIRE(plan.purge_transactions.size() == 1);
    CHECK(plan.purge_transactions[0].claimed_task_ids == std::vector<size_t>{2});
    CHECK(plan.purge_transactions[0].claimed_volume_mm3 == Catch::Approx(4.));
    CHECK(plan.purge_transactions[0].tower_volume_mm3 == Catch::Approx(2.));
}

TEST_CASE("Cross-material wipe rolls back once when midpoint insertion breaks adjacency",
    "[HalfLayer][WipeIntoReassignment]")
{
    ExtrusionPath path(erInternalInfill, 0.1, 0.5f, 0.2f);
    std::array<ExtrusionEntityCollection, 4> roots;
    auto task = [&](unsigned filament, size_t root, bool eligible, double seconds) {
        HalfLayerExecutionTask out;
        out.entity = &path;
        out.overrides_key = &roots[root];
        out.tool = {0, filament};
        out.extrusion_seconds = seconds;
        out.first_mm = Vec3d(double(root) * 2., 0., 1.);
        out.last_mm = out.first_mm + Vec3d(1., 0., 0.);
        out.travel_speed_mm_s = 100.;
        out.z_speed_mm_s = 10.;
        out.clearance_mm = 0.3;
        out.motion_reference_height = 0.2;
        out.tool_entry_seconds = 0.;
        out.tool_entry_seconds_by_filament = {0., 0.};
        out.wipe_into_eligible = eligible;
        out.wipe_into_volume_mm3 = 4.;
        return out;
    };
    std::vector<HalfLayerExecutionTask> lower{task(0, 0, false, 1.)};
    std::vector<HalfLayerExecutionTask> core{
        task(0, 1, false, 20.), task(0, 2, true, 1.)};
    std::vector<HalfLayerExecutionTask> upper{task(1, 3, false, 1.)};
    std::vector<HalfLayerExecutionTask *> stream;
    for (auto *tasks : {&lower, &core, &upper})
        for (HalfLayerExecutionTask &item : *tasks) stream.push_back(&item);
    HalfLayerWipeIntoContext context;
    context.filament_count = 2;
    context.loaded_filament_by_tool = {0};
    context.purge_volume_mm3_by_tool = {{0., 6., 6., 0.}};
    context.transition_claimable_filament = {true, true};
    const size_t reservations = reserve_half_layer_cross_material_wipe(
        stream, context, true, [](HalfLayerExecutionTask &item, unsigned destination) {
            item.tool.filament = destination;
            item.extrusion_seconds = 5.;
            item.clearance_mm = 0.6;
            item.wipe_into_volume_mm3 = 2.;
            item.tool_entry_seconds_by_filament = {0., 0.};
        });
    REQUIRE(reservations == 1);
    HalfLayerExecutionPlan plan = schedule_half_layer_execution({}, std::move(lower),
        std::move(core), std::move(upper), 0.);
    REQUIRE(plan.upper_end < plan.tasks.size());
    reconcile_half_layer_cross_material_wipe(plan, context.loaded_filament_by_tool, reservations, 0);
    const auto restored = std::find_if(plan.tasks.begin(), plan.tasks.end(),
        [&](const HalfLayerExecutionTask &item) { return item.overrides_key == &roots[2]; });
    REQUIRE(restored != plan.tasks.end());
    CHECK(restored->wipe_reservation_id == size_t(-1));
    CHECK(restored->tool.filament == 0);
    CHECK(restored->extrusion_seconds == Catch::Approx(1.));
    CHECK(restored->clearance_mm == Catch::Approx(0.3));
    CHECK(restored->wipe_into_volume_mm3 == Catch::Approx(4.));
    REQUIRE(plan.visits.size() == 3);
    CHECK(plan.visits[0].tool.filament == 0);
    CHECK(plan.visits[1].tool.filament == 1);
    CHECK(plan.visits[2].tool.filament == 0);
}

TEST_CASE("Cross-material wipe never reserves across physical nozzles",
    "[HalfLayer][WipeIntoReassignment]")
{
    ExtrusionPath path(erInternalInfill, 0.1, 0.5f, 0.2f);
    std::array<ExtrusionEntityCollection, 2> roots;
    std::vector<HalfLayerExecutionTask> tasks(2);
    for (size_t i = 0; i < tasks.size(); ++i) {
        tasks[i].entity = &path;
        tasks[i].overrides_key = &roots[i];
        tasks[i].tool = {unsigned(i), unsigned(i)};
        tasks[i].wipe_into_eligible = true;
        tasks[i].wipe_into_volume_mm3 = 10.;
    }
    std::vector<HalfLayerExecutionTask *> stream{&tasks[0], &tasks[1]};
    HalfLayerWipeIntoContext context;
    context.filament_count = 2;
    context.loaded_filament_by_tool = {0, 1};
    context.purge_volume_mm3_by_tool = {{0., 10., 10., 0.}, {0., 10., 10., 0.}};
    context.transition_claimable_filament = {true, true};
    bool retargeted = false;
    CHECK(reserve_half_layer_cross_material_wipe(stream, context, true,
        [&](HalfLayerExecutionTask &, unsigned) { retargeted = true; }) == 0);
    CHECK_FALSE(retargeted);
    CHECK(tasks[0].tool.physical_tool == 0);
    CHECK(tasks[1].tool.physical_tool == 1);
}

TEST_CASE("Cross-material wipe rollback carries actual nozzle state forward once",
    "[HalfLayer][WipeIntoReassignment]")
{
    ExtrusionPath path(erInternalInfill, 0.1, 0.5f, 0.2f);
    std::array<ExtrusionEntityCollection, 5> roots;
    auto task = [&](unsigned filament, size_t root, bool eligible) {
        HalfLayerExecutionTask out;
        out.entity = &path;
        out.overrides_key = &roots[root];
        out.tool = {0, filament};
        out.wipe_into_eligible = eligible;
        out.wipe_into_volume_mm3 = 4.;
        return out;
    };
    std::vector<HalfLayerExecutionTask> tasks{
        task(0, 0, false), task(0, 1, true),
        task(1, 2, false), task(1, 3, true),
        task(0, 4, false)
    };
    std::vector<HalfLayerExecutionTask *> stream;
    for (HalfLayerExecutionTask &item : tasks) stream.push_back(&item);
    HalfLayerWipeIntoContext context;
    context.filament_count = 2;
    context.loaded_filament_by_tool = {0};
    context.purge_volume_mm3_by_tool = {{0., 6., 6., 0.}};
    context.transition_claimable_filament = {true, true};
    const size_t reservations = reserve_half_layer_cross_material_wipe(
        stream, context, true, [](HalfLayerExecutionTask &item, unsigned destination) {
            item.tool.filament = destination;
        });
    REQUIRE(reservations == 2);

    // Model an upper-block move that places the second reservation directly
    // after the first. Reservation 0 loses its stable destination successor;
    // its rollback leaves A loaded, which invalidates reservation 1's B->A
    // source in the same forward sweep.
    HalfLayerExecutionPlan plan;
    for (size_t index : {size_t(0), size_t(1), size_t(3), size_t(4), size_t(2)})
        plan.tasks.push_back(std::move(tasks[index]));
    reconcile_half_layer_cross_material_wipe(plan, context.loaded_filament_by_tool, reservations, 0);
    CHECK(std::none_of(plan.tasks.begin(), plan.tasks.end(), [](const HalfLayerExecutionTask &item) {
        return item.wipe_reservation_id != size_t(-1);
    }));
    REQUIRE(plan.visits.size() == 4);
    CHECK(plan.visits[0].tool.filament == 0);
    CHECK(plan.visits[1].tool.filament == 1);
    CHECK(plan.visits[2].tool.filament == 0);
    CHECK(plan.visits[3].tool.filament == 1);

    HalfLayerExecutionTask body = task(0, 0, true);
    HalfLayerExecutionTask interface = body;
    body.wipe_root_kind = HalfLayerWipeRootKind::SupportBody;
    interface.wipe_root_kind = HalfLayerWipeRootKind::SupportInterface;
    CHECK_FALSE(same_half_layer_wipe_root(body, interface));
}

TEST_CASE("First-task wipe rollback refreshes initial entry timing",
    "[HalfLayer][WipeIntoInitialTiming]")
{
    const bool delayed_wall = GENERATE(false, true);
    ExtrusionPath path(erInternalInfill, 0.1, 0.5f, 0.2f);
    std::array<ExtrusionEntityCollection, 3> roots;
    auto task = [&](unsigned filament, size_t root, bool eligible) {
        HalfLayerExecutionTask out;
        out.entity = &path;
        out.overrides_key = &roots[root];
        out.tool = {0, filament};
        out.extrusion_seconds = 1.;
        out.first_mm = Vec3d(double(root) * 2., 0., 1.);
        out.last_mm = out.first_mm + Vec3d(1., 0., 0.);
        out.travel_speed_mm_s = 100.;
        out.z_speed_mm_s = 10.;
        out.clearance_mm = 0.3;
        out.motion_reference_height = 0.2;
        out.tool_entry_seconds = 7.;
        out.tool_entry_seconds_by_filament = {0., 7.};
        out.wipe_into_eligible = eligible;
        out.wipe_into_volume_mm3 = 4.;
        return out;
    };
    std::vector<HalfLayerExecutionTask> lower{task(0, 0, true)};
    std::vector<HalfLayerExecutionTask> core{task(1, 1, false)};
    std::vector<HalfLayerExecutionTask> upper;
    if (delayed_wall)
        upper.push_back(task(0, 2, false));
    std::vector<HalfLayerExecutionTask *> stream{&lower[0], &core[0]};
    HalfLayerWipeIntoContext context;
    context.filament_count = 2;
    // B is actually loaded at frame entry, so the provisional A->B suffix
    // reservation at the first task must roll back to A.
    context.loaded_filament_by_tool = {1};
    context.purge_volume_mm3_by_tool = {{0., 6., 6., 0.}};
    context.transition_claimable_filament = {true, true};
    const size_t reservations = reserve_half_layer_cross_material_wipe(
        stream, context, true, [](HalfLayerExecutionTask &item, unsigned destination) {
            item.tool.filament = destination;
            item.tool_entry_seconds = 3.;
            item.tool_entry_seconds_by_filament = {3., 0.};
        });
    REQUIRE(reservations == 1);
    const unsigned incoming_filament = 1;
    const double provisional_initial_seconds = half_layer_initial_entry_seconds(incoming_filament, lower.front());
    REQUIRE(provisional_initial_seconds == Catch::Approx(0.));
    HalfLayerExecutionPlan plan = schedule_half_layer_execution({}, std::move(lower),
        std::move(core), std::move(upper), provisional_initial_seconds);
    const auto fixed_upper_begin = plan.upper_begin;
    reconcile_half_layer_cross_material_wipe(plan, context.loaded_filament_by_tool, reservations, incoming_filament);
    REQUIRE(plan.tasks.front().tool.filament == 0);
    CHECK(plan.initial_seconds == Catch::Approx(7.));
    CHECK(plan.upper_begin == fixed_upper_begin);
    if (delayed_wall) {
        // Independent fixed-stream timeline: 7 s entry, 1 s per path,
        // XY at 100 mm/s, 0.6 mm round-trip lift at 10 mm/s.
        double elapsed_seconds = 7., upper_start_seconds = 0.;
        for (size_t i = 0; i < plan.tasks.size(); ++i) {
            if (i > 0) {
                const auto &from = plan.tasks[i - 1];
                const auto &to = plan.tasks[i];
                elapsed_seconds += std::abs(to.first_mm.x() - from.last_mm.x()) / 100. + 0.6 / 10.;
                if (from.tool.filament != to.tool.filament)
                    elapsed_seconds += from.tool.filament == 1 ? 7. : 0.;
            }
            if (i == plan.upper_begin)
                upper_start_seconds = elapsed_seconds;
            elapsed_seconds += 1.;
        }
        CHECK(plan.insertion.total_seconds == Catch::Approx(elapsed_seconds));
        CHECK(plan.insertion.second_start_seconds == Catch::Approx(upper_start_seconds));
        CHECK(plan.insertion.midpoint_error_seconds ==
            Catch::Approx(std::abs(upper_start_seconds - 7. - elapsed_seconds / 2.)));
    }
}

TEST_CASE("Half-layer scheduler cost is measured separately from geometry", "[.SchedulerCost]")
{
    const size_t core_count = GENERATE(size_t(1000), size_t(4000), size_t(16000));
    ExtrusionPath path(erPerimeter, 0.1, 0.5f, 0.2f);
    HalfLayerExecutionTask base;
    base.entity = &path;
    base.tool = {0, 0};
    base.extrusion_seconds = 1.;
    base.travel_speed_mm_s = 100.;
    base.z_speed_mm_s = 10.;
    base.clearance_mm = 0.3;
    base.motion_reference_height = 0.2;
    std::vector<HalfLayerExecutionTask> core(core_count, base);
    for (size_t i = 0; i < core.size(); ++i) {
        core[i].first_mm = Vec3d(double(i), 0., 1.);
        core[i].last_mm = Vec3d(double(i) + 0.5, 0., 1.);
    }
    std::vector<double> elapsed_ms;
    for (int run = 0; run < 6; ++run) {
        const auto start = std::chrono::steady_clock::now();
        auto plan = schedule_half_layer_execution({base}, core, {base});
        reconcile_half_layer_cross_material_wipe(plan, {0}, 0, 0);
        const double duration_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        REQUIRE(plan.tasks.size() == core_count + 2);
        if (run > 0) elapsed_ms.push_back(duration_ms); // One warm-up, five measured samples.
    }
    std::sort(elapsed_ms.begin(), elapsed_ms.end());
    std::cout << "half_layer_scheduler core_tasks=" << core_count
              << " median_ms=" << elapsed_ms[elapsed_ms.size() / 2] << '\n';
}

TEST_CASE("Required half-layer clearance survives pending lifts and Z direction changes", "[HalfLayer][Travel]")
{
    const double parent_h_mm = GENERATE(0.1, 0.2, 0.3);
    const double configured_hop_mm = GENERATE(0., 0.15, 0.3, 0.6);
    const int preexisting = GENERATE(0, 1, 2); // None, pending lazy lift, already lifted.
    const double target_delta_mm = GENERATE(-0.1, 0., 0.1);
    GCodeWriter writer;
    writer.set_extruders({0});
    writer.set_extruder(0);
    writer.config.z_hop.values = {configured_hop_mm};
    std::string emitted = writer.travel_to_z(10.);
    writer.set_current_position_clear(true);
    if (preexisting == 1)
        emitted += writer.lazy_lift(LiftType::SlopeLift);
    else if (preexisting == 2)
        emitted += writer.eager_lift(LiftType::NormalLift);
    const double target_z_mm = 10. + target_delta_mm;
    const double minimum_hop_mm = 1.5 * parent_h_mm;
    const double expected_z_mm = std::max(10., target_z_mm) + std::max(configured_hop_mm, minimum_hop_mm);
    emitted += writer.ensure_travel_clearance(target_z_mm, minimum_hop_mm);
    CAPTURE(parent_h_mm, configured_hop_mm, preexisting, target_delta_mm);
    CHECK(writer.get_position().z() == Catch::Approx(expected_z_mm));
    CHECK(writer.get_zhop() == Catch::Approx(expected_z_mm - target_z_mm));
    CHECK(writer.ensure_travel_clearance(target_z_mm, minimum_hop_mm).empty());
    emitted += writer.travel_to_xyz(Vec3d(1., 2., target_z_mm));
    CHECK(writer.get_position().z() == Catch::Approx(expected_z_mm));
    emitted += writer.unlift();
    CHECK(writer.get_position().z() == Catch::Approx(target_z_mm));
    CHECK(writer.get_zhop() == 0.);
    GCodeReader reader;
    size_t xy_moves = 0;
    reader.parse_buffer(emitted, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
        if (line.dist_XY(state) > 0.) {
            ++xy_moves;
            constexpr double decoded_z_tolerance_mm = 0.00001; // Float decoding tolerance, mm.
            CHECK(state.z() == Catch::Approx(expected_z_mm).margin(decoded_z_tolerance_mm));
            CHECK(line.new_Z(state) == Catch::Approx(expected_z_mm).margin(decoded_z_tolerance_mm));
        }
    });
    CHECK(xy_moves == 1);
    CHECK(reader.z() == Catch::Approx(target_z_mm));
    writer.ensure_travel_clearance(target_z_mm, minimum_hop_mm);
    CHECK(writer.get_position().z() == Catch::Approx(target_z_mm + std::max(configured_hop_mm, minimum_hop_mm)));
}

TEST_CASE("Explicit source plane survives custom-position bookkeeping", "[HalfLayer][Travel][CustomGCode]")
{
    GCodeWriter writer;
    writer.set_extruders({0});
    writer.set_extruder(0);
    writer.config.z_hop.values = {0.05};
    writer.travel_to_z(2.24);
    writer.set_position_with_nominal_z(Vec3d(0., 0., 3.79), 2.24);
    CHECK(writer.get_zhop() == Catch::Approx(1.55));
    CHECK(writer.ensure_travel_clearance(2.24, 2.16, 0.24).empty());
    CHECK(writer.get_position().z() == Catch::Approx(3.79));
    CHECK(writer.get_zhop() == Catch::Approx(1.63));
    const std::string travel = writer.travel_to_xyz(Vec3d(1., 0., 2.16));
    CHECK(travel.find("Z2.16") == std::string::npos);
    const std::string restore = writer.unlift();
    CHECK(restore.find("Z2.16") != std::string::npos);
}

TEST_CASE("Half-layer clearance rejects invalid inputs before changing writer state", "[HalfLayer][Travel]")
{
    GCodeWriter writer;
    CHECK_THROWS_AS(writer.ensure_travel_clearance(1., 0.3), std::logic_error);
    writer.set_extruders({0});
    writer.set_extruder(0);
    writer.travel_to_z(10.);
    for (const double invalid : {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        CHECK_THROWS_AS(writer.ensure_travel_clearance(1., invalid), std::invalid_argument);
        CHECK(writer.get_position().z() == 10.);
    }
}

TEST_CASE("G-code short travel applies the logical parent lift with either toggle", "[HalfLayer][TravelAdapter]")
{
    using namespace Slic3r::Test;
    const int toggles = GENERATE(0, 1, 2, 3);
    const double configured_hop_mm = GENERATE(0., 0.6);
    const bool variable = GENERATE(false, true);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"outer_wall_half_layer_height", bool(toggles & 1)}, {"support_half_layer_height", bool(toggles & 2)},
        {"z_hop", configured_hop_mm}, {"retraction_minimum_travel", 1000.}, {"reduce_crossing_wall", false},
        {"travel_speed", 120.}, {"initial_layer_travel_speed", 30.}, {"retract_lift_above", 100.}});
    Print print;
    Model model;
    init_print({cube(5.)}, print, model, config);
    if (variable) {
        model.objects.front()->layer_height_profile.set({0., 0.2, 2., 0.12, 4., 0.28, 5., 0.16});
        print.apply(model, config);
    }
    auto &object = *print.get_object(size_t(0));
    const auto sources = object.make_half_layer_source_layers();
    for (const size_t index : {size_t(0), size_t(5)}) {
        const Layer &parent = *object.layers()[index];
        GCode generator;
        generator.apply_print_config(print.config());
        generator.set_layer_count(7);
        generator.writer().set_extruders({0});
        generator.writer().set_extruder(0);
        GCodeReader reader;
        reader.parse_buffer(generator.writer().travel_to_z(parent.bottom_z()));
        generator.writer().set_current_position_clear(true);
        size_t moves = 0;
        for (const unsigned phase : {0u, 1u, 0u}) {
            const Layer &physical = *sources.phases[phase][index];
            const double source_z_mm = generator.writer().get_position().z();
            generator.set_physical_layer(physical, parent);
            CHECK(generator.layer_count() == 7);
            const double expected_clearance_mm = toggles == 0 ? physical.print_z :
                std::max(source_z_mm, physical.print_z) + std::max(configured_hop_mm, 1.5 * parent.height);
            const Point destination = Point::new_scale(0.01 * (++moves), 0.);
            LiftType lift_type = LiftType::NormalLift;
            REQUIRE_FALSE(generator.needs_retraction(Polyline{generator.last_pos(), destination}, erExternalPerimeter, lift_type));
            const std::string travel = generator.travel_to(destination, erExternalPerimeter, "half-layer adapter test");
            const std::string restore = generator.unretract();
            size_t xy_moves = 0;
            reader.parse_buffer(travel + restore, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
                if (line.dist_XY(state) > 0.) {
                    ++xy_moves;
                    // The writer serializes Z to millimetres with 0.001 mm resolution.
                    constexpr double encoded_z_tolerance_mm = 0.00051;
                    CAPTURE(toggles, configured_hop_mm, variable, index, phase, parent.height, expected_clearance_mm);
                    CHECK(line.new_Z(state) == Catch::Approx(expected_clearance_mm).margin(encoded_z_tolerance_mm));
                    if (toggles != 0)
                        CHECK(line.new_F(state) == Catch::Approx((index == 0 ? 30. : 120.) * 60.));
                }
            });
            CHECK(xy_moves == 1);
            CHECK(generator.writer().get_position().z() == Catch::Approx(physical.print_z));
        }
        CHECK_THROWS_AS(generator.set_physical_layer(*sources.phases[0][index], *object.layers()[index + 2]), std::invalid_argument);
    }
}
