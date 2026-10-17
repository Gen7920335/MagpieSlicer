#pragma once

#include "ExtrusionSpeed.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../HalfLayerPlan.hpp"
#include "../PrintConfig.hpp"
#include <algorithm>
#include <limits>

namespace Slic3r {

class Layer;
class LayerRegion;
class SupportLayer;
class PrintObject;
class LayerTools;

enum class HalfLayerWipeRootKind : unsigned char {
    Model,
    SupportBody,
    SupportInterface
};

// Stable references into completed PrintObject toolpaths. The owning PrintObject
// must outlive the plan; any geometry/config invalidation discards the plan.
// One task is one whole loop, multipath or open path, never part of a loop.
struct HalfLayerExecutionTask {
    size_t id = 0;
    const ExtrusionEntity *entity = nullptr;
    const ExtrusionEntityCollection *overrides_key = nullptr;
    const Layer *physical_layer = nullptr;
    const Layer *logical_layer = nullptr;
    const LayerRegion *region = nullptr;
    const PrintObject *object = nullptr;
    size_t instance_id = 0;
    HalfLayerToolVisit tool {};
    // Geometry ownership never changes. Wipe-into may only change the
    // effective material on the same physical nozzle, and must retain this
    // complete normal snapshot for deterministic one-pass rollback.
    HalfLayerToolVisit normal_tool {unsigned(-1), unsigned(-1)};
    double normal_extrusion_seconds = std::numeric_limits<double>::quiet_NaN();
    double normal_wipe_into_volume_mm3 = 0.;
    double normal_clearance_mm = std::numeric_limits<double>::quiet_NaN();
    double normal_tool_entry_seconds = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> normal_tool_entry_seconds_by_filament;
    size_t wipe_reservation_id = size_t(-1);
    unsigned int wipe_reservation_source_filament = unsigned(-1);
    unsigned int wipe_reservation_destination_filament = unsigned(-1);
    HalfLayerWipeRootKind wipe_root_kind = HalfLayerWipeRootKind::Model;
    bool perimeter = false;
    bool support = false;
    bool requires_upper_before = false;
    // Global XY endpoints include the instance shift; all coordinates are mm.
    Vec3d first_mm = Vec3d::Zero();
    Vec3d last_mm = Vec3d::Zero();
    double extrusion_seconds = 0.;
    double travel_speed_mm_s = 0.;
    double z_speed_mm_s = 0.;
    double clearance_mm = 0.;
    // Full logical/model layer height used by mandatory travel clearance.
    // This must never be inferred from the half-height physical band.
    double motion_reference_height = 0.;
    // Existing configured load/unload/purge estimate, supplied by tool planning.
    double tool_entry_seconds = 0.;
    // Cached entry cost indexed by the previously active filament. This keeps
    // the midpoint scan linear while retaining asymmetric A->B / B->A purge
    // volumes and the physical-nozzle mapping used by the tower planner.
    std::vector<double> tool_entry_seconds_by_filament;
    // Wipe-into may consume this task's extrusion only when the complete
    // override root is already the first dependency-ready block of a visit.
    // No geometry, task order or material owner is changed by the claim.
    bool wipe_into_eligible = false;
    double wipe_into_volume_mm3 = 0.;
};

struct HalfLayerPurgeTransaction {
    size_t visit_index = size_t(-1);
    unsigned int physical_tool = unsigned(-1);
    unsigned int source_filament = unsigned(-1);
    unsigned int destination_filament = unsigned(-1);
    double required_volume_mm3 = 0.;
    double claimed_volume_mm3 = 0.;
    double tower_volume_mm3 = 0.;
    std::vector<size_t> claimed_task_ids;
};

struct HalfLayerExecutionVisit {
    HalfLayerToolVisit tool;
    size_t begin;
    size_t end;
    bool tower_only = false;
    size_t purge_transaction = size_t(-1);
};

struct HalfLayerExecutionPlan {
    std::vector<HalfLayerExecutionTask> tasks;
    std::vector<HalfLayerExecutionVisit> visits;
    std::vector<HalfLayerPurgeTransaction> purge_transactions;
    HalfLayerInsertion insertion;
    double initial_seconds = 0.;
    size_t lower_begin = 0;
    size_t lower_end = 0;
    size_t upper_begin = 0;
    size_t upper_end = 0;
};

struct HalfLayerWipeIntoContext {
    size_t filament_count = 0;
    std::vector<int> loaded_filament_by_tool;
    // One flattened source-major filament matrix for each physical tool.
    // Values are the configured purge requirement in mm3 before backend-
    // specific mandatory tower/grab allowances.
    std::vector<std::vector<double>> purge_volume_mm3_by_tool;
    std::vector<bool> transition_claimable_filament;

    double required_volume_mm3(unsigned int physical_tool, unsigned int source,
                               unsigned int destination) const
    {
        if (source >= filament_count || destination >= filament_count ||
            physical_tool >= purge_volume_mm3_by_tool.size())
            return 0.;
        const auto &matrix = purge_volume_mm3_by_tool[physical_tool];
        const size_t index = size_t(source) * filament_count + destination;
        return index < matrix.size() && std::isfinite(matrix[index]) ? std::max(0., matrix[index]) : 0.;
    }

    bool transition_claimable(unsigned int source, unsigned int destination) const
    {
        return source < transition_claimable_filament.size() &&
               destination < transition_claimable_filament.size() &&
               transition_claimable_filament[source] && transition_claimable_filament[destination];
    }
};

inline bool same_half_layer_wipe_root(const HalfLayerExecutionTask &a,
                                      const HalfLayerExecutionTask &b)
{
    return a.overrides_key != nullptr && a.overrides_key == b.overrides_key &&
           a.object == b.object && a.instance_id == b.instance_id &&
           a.physical_layer == b.physical_layer && a.support == b.support &&
           a.wipe_root_kind == b.wipe_root_kind;
}

inline double half_layer_transition_seconds(const HalfLayerExecutionTask &from,
                                            const HalfLayerExecutionTask &to);
inline double half_layer_initial_entry_seconds(unsigned int incoming_filament,
                                               const HalfLayerExecutionTask &to);

inline void initialize_half_layer_normal_owner(HalfLayerExecutionTask &task)
{
    if (task.normal_tool.physical_tool == unsigned(-1))
        task.normal_tool = task.tool;
}

inline void capture_half_layer_normal_snapshot(HalfLayerExecutionTask &task)
{
    initialize_half_layer_normal_owner(task);
    if (std::isfinite(task.normal_extrusion_seconds))
        return;
    if (task.tool.physical_tool != task.normal_tool.physical_tool ||
        task.tool.filament != task.normal_tool.filament)
        throw std::logic_error("Half-layer normal snapshot was requested after retargeting");
    task.normal_extrusion_seconds = task.extrusion_seconds;
    task.normal_wipe_into_volume_mm3 = task.wipe_into_volume_mm3;
    task.normal_clearance_mm = task.clearance_mm;
    task.normal_tool_entry_seconds = task.tool_entry_seconds;
    task.normal_tool_entry_seconds_by_filament = task.tool_entry_seconds_by_filament;
}

inline void restore_half_layer_normal_snapshot(HalfLayerExecutionTask &task)
{
    initialize_half_layer_normal_owner(task);
    if (std::isfinite(task.normal_extrusion_seconds)) {
        task.tool = task.normal_tool;
        task.extrusion_seconds = task.normal_extrusion_seconds;
        task.wipe_into_volume_mm3 = task.normal_wipe_into_volume_mm3;
        task.clearance_mm = task.normal_clearance_mm;
        task.tool_entry_seconds = task.normal_tool_entry_seconds;
        task.tool_entry_seconds_by_filament = task.normal_tool_entry_seconds_by_filament;
    } else if (task.tool.physical_tool != task.normal_tool.physical_tool ||
               task.tool.filament != task.normal_tool.filament)
        throw std::logic_error("Half-layer wipe rollback has no normal snapshot");
    task.wipe_reservation_id = size_t(-1);
    task.wipe_reservation_source_filament = unsigned(-1);
    task.wipe_reservation_destination_filament = unsigned(-1);
}

// Product implementation in GCode.cpp. It applies the same speed, pair-entry
// and mandatory-clearance resolver used when the normal task was collected.
void retarget_half_layer_task_material(HalfLayerExecutionTask &task,
                                       unsigned int destination_filament);

// Shift an existing same-physical-nozzle S->D boundary backward over the
// smallest complete eligible suffix of S. Task order and the provisional visit
// count remain unchanged. The caller supplies the production retarget resolver
// so synthetic planner tests may use a cheap deterministic substitute.
template<class Retarget>
inline size_t reserve_half_layer_cross_material_wipe(
    std::vector<HalfLayerExecutionTask *> stream,
    const HalfLayerWipeIntoContext &context,
    bool allow_claims,
    Retarget &&retarget)
{
    for (HalfLayerExecutionTask *task : stream)
        if (task != nullptr)
            initialize_half_layer_normal_owner(*task);
    if (!allow_claims || stream.empty())
        return 0;

    struct Run { size_t begin, end; HalfLayerToolVisit tool; };
    std::vector<Run> runs;
    for (size_t i = 0; i < stream.size();) {
        if (stream[i] == nullptr)
            throw std::invalid_argument("Half-layer wipe stream contains a null task");
        const HalfLayerToolVisit tool = stream[i]->normal_tool;
        size_t end = i + 1;
        while (end < stream.size() && stream[end] != nullptr &&
               stream[end]->normal_tool.physical_tool == tool.physical_tool &&
               stream[end]->normal_tool.filament == tool.filament)
            ++end;
        runs.push_back({i, end, tool});
        i = end;
    }

    size_t next_reservation_id = 0;
    for (size_t run_index = 1; run_index < runs.size(); ++run_index) {
        const Run &source = runs[run_index - 1];
        const Run &destination = runs[run_index];
        if (source.tool.physical_tool != destination.tool.physical_tool ||
            source.tool.filament == destination.tool.filament ||
            !context.transition_claimable(source.tool.filament, destination.tool.filament))
            continue;
        const double required = context.required_volume_mm3(source.tool.physical_tool,
            source.tool.filament, destination.tool.filament);
        if (!(required > 0.))
            continue;

        // Count only the dependency-ready eligible prefix already owned by D.
        double available = 0.;
        for (size_t begin = destination.begin; begin < destination.end;) {
            size_t end = begin + 1;
            while (end < destination.end && same_half_layer_wipe_root(*stream[begin], *stream[end]))
                ++end;
            bool eligible = true;
            double volume = 0.;
            for (size_t i = begin; i < end; ++i) {
                const HalfLayerExecutionTask &task = *stream[i];
                if (!task.wipe_into_eligible || !std::isfinite(task.wipe_into_volume_mm3) ||
                    task.wipe_into_volume_mm3 <= 0.) {
                    eligible = false;
                    break;
                }
                volume += task.wipe_into_volume_mm3;
            }
            if (!eligible)
                break;
            available += volume;
            if (available >= required)
                break;
            begin = end;
        }
        if (available >= required)
            continue;

        struct Block { size_t begin, end; };
        std::vector<Block> suffix;
        size_t end = source.end;
        while (end > source.begin && available < required) {
            size_t begin = end - 1;
            while (begin > source.begin && same_half_layer_wipe_root(*stream[begin - 1], *stream[end - 1]))
                --begin;
            bool eligible = true;
            double volume = 0.;
            for (size_t i = begin; i < end; ++i) {
                const HalfLayerExecutionTask &task = *stream[i];
                if (!task.wipe_into_eligible || !std::isfinite(task.wipe_into_volume_mm3) ||
                    task.wipe_into_volume_mm3 <= 0. ||
                    task.normal_tool.physical_tool != source.tool.physical_tool ||
                    task.normal_tool.filament != source.tool.filament) {
                    eligible = false;
                    break;
                }
                volume += task.wipe_into_volume_mm3;
            }
            if (!eligible)
                break;
            suffix.push_back({begin, end});
            // Size the suffix using D's actual flow, not S's geometric or
            // material volume. Resolve each selected task just once.
            volume = 0.;
            for (size_t i = begin; i < end; ++i) {
                capture_half_layer_normal_snapshot(*stream[i]);
                retarget(*stream[i], destination.tool.filament);
                volume += stream[i]->wipe_into_volume_mm3;
            }
            available += volume;
            end = begin;
        }
        if (suffix.empty())
            continue;

        const size_t reservation_id = next_reservation_id++;
        for (auto block = suffix.rbegin(); block != suffix.rend(); ++block)
            for (size_t i = block->begin; i < block->end; ++i) {
                HalfLayerExecutionTask &task = *stream[i];
                task.wipe_reservation_id = reservation_id;
                task.wipe_reservation_source_filament = source.tool.filament;
                task.wipe_reservation_destination_filament = destination.tool.filament;
            }
    }
    return next_reservation_id;
}

inline void rebuild_half_layer_visits(HalfLayerExecutionPlan &plan)
{
    plan.visits.clear();
    for (size_t i = 0; i < plan.tasks.size(); ++i) {
        const HalfLayerToolVisit tool = plan.tasks[i].tool;
        if (plan.visits.empty() || plan.visits.back().tool.physical_tool != tool.physical_tool ||
            plan.visits.back().tool.filament != tool.filament)
            plan.visits.push_back({tool, i, i + 1, false, size_t(-1)});
        else
            plan.visits.back().end = i + 1;
    }
}

inline void refresh_half_layer_fixed_insertion_metrics(HalfLayerExecutionPlan &plan)
{
    if (plan.insertion.boundary == HalfLayerInsertion::unavailable ||
        plan.lower_begin >= plan.tasks.size() || plan.upper_begin >= plan.upper_end ||
        plan.upper_end > plan.tasks.size())
        return;
    std::vector<double> starts(plan.tasks.size(), plan.initial_seconds);
    double elapsed = plan.initial_seconds;
    for (size_t i = 0; i < plan.tasks.size(); ++i) {
        if (i > 0)
            elapsed += half_layer_transition_seconds(plan.tasks[i - 1], plan.tasks[i]);
        starts[i] = elapsed;
        elapsed += plan.tasks[i].extrusion_seconds;
    }
    plan.insertion.second_start_seconds = starts[plan.upper_begin];
    plan.insertion.total_seconds = elapsed;
    plan.insertion.midpoint_error_seconds = std::abs(
        plan.insertion.second_start_seconds - starts[plan.lower_begin] - 0.5 * elapsed);
}

// Reconcile once after upper-wall insertion. A reservation survives only when
// it is still one contiguous block, starts a real source->destination change on
// its physical nozzle and flows directly into an unreserved destination task.
inline void reconcile_half_layer_cross_material_wipe(
    HalfLayerExecutionPlan &plan,
    const std::vector<int> &initial_loaded_filament_by_tool,
    size_t reservation_count,
    unsigned int incoming_filament)
{
    std::vector<size_t> first(reservation_count, size_t(-1));
    std::vector<size_t> last(reservation_count, size_t(-1));
    std::vector<size_t> count(reservation_count, 0);
    for (size_t i = 0; i < plan.tasks.size(); ++i) {
        const size_t id = plan.tasks[i].wipe_reservation_id;
        if (id == size_t(-1))
            continue;
        if (id >= reservation_count)
            throw std::logic_error("Half-layer wipe reservation has an invalid ID");
        if (first[id] == size_t(-1)) first[id] = i;
        last[id] = i;
        ++count[id];
    }
    for (size_t id = 0; id < reservation_count; ++id)
        if (first[id] != size_t(-1) && last[id] - first[id] + 1 != count[id])
            for (HalfLayerExecutionTask &task : plan.tasks)
                if (task.wipe_reservation_id == id)
                    restore_half_layer_normal_snapshot(task);

    std::vector<int> loaded = initial_loaded_filament_by_tool;
    size_t i = 0;
    while (i < plan.tasks.size()) {
        HalfLayerExecutionTask &task = plan.tasks[i];
        const size_t id = task.wipe_reservation_id;
        if (id == size_t(-1)) {
            if (task.tool.physical_tool >= loaded.size())
                throw std::logic_error("Half-layer task has an invalid physical tool during wipe reconciliation");
            loaded[task.tool.physical_tool] = int(task.tool.filament);
            ++i;
            continue;
        }

        size_t end = i + 1;
        while (end < plan.tasks.size() && plan.tasks[end].wipe_reservation_id == id)
            ++end;
        const unsigned physical_tool = task.tool.physical_tool;
        const unsigned source = task.wipe_reservation_source_filament;
        const unsigned destination = task.wipe_reservation_destination_filament;
        bool valid = physical_tool < loaded.size() && loaded[physical_tool] == int(source) &&
            task.tool.filament == destination &&
            (i == 0 || plan.tasks[i - 1].tool.physical_tool != physical_tool ||
             plan.tasks[i - 1].tool.filament != destination) &&
            end < plan.tasks.size() && plan.tasks[end].wipe_reservation_id == size_t(-1) &&
            plan.tasks[end].tool.physical_tool == physical_tool &&
            plan.tasks[end].tool.filament == destination;
        for (size_t j = i; valid && j < end; ++j)
            valid = plan.tasks[j].tool.physical_tool == physical_tool &&
                plan.tasks[j].tool.filament == destination &&
                plan.tasks[j].wipe_reservation_source_filament == source &&
                plan.tasks[j].wipe_reservation_destination_filament == destination;
        if (!valid)
            for (size_t j = i; j < end; ++j)
                restore_half_layer_normal_snapshot(plan.tasks[j]);
        for (size_t j = i; j < end; ++j) {
            HalfLayerExecutionTask &actual = plan.tasks[j];
            if (actual.tool.physical_tool >= loaded.size())
                throw std::logic_error("Half-layer task has an invalid physical tool after wipe rollback");
            loaded[actual.tool.physical_tool] = int(actual.tool.filament);
        }
        i = end;
    }
    if (!plan.tasks.empty())
        plan.initial_seconds = half_layer_initial_entry_seconds(incoming_filament, plan.tasks.front());
    refresh_half_layer_fixed_insertion_metrics(plan);
    rebuild_half_layer_visits(plan);
}

// Reserve only the dependency-ready prefix of each destination visit. This is
// the maximal safe visit-local reuse that needs neither task relocation nor a
// material/nozzle override: every claimed root is emitted immediately after
// the already-planned tool change, and every unclaimed remainder stays on the
// tower. Work is linear in visits plus tasks and every task is inspected once.
inline void apply_half_layer_wipe_into_claims(HalfLayerExecutionPlan &plan,
                                               HalfLayerWipeIntoContext &context,
                                               bool allow_claims)
{
    plan.purge_transactions.clear();
    std::vector<bool> claimed(plan.tasks.size(), false);
    for (size_t visit_index = 0; visit_index < plan.visits.size(); ++visit_index) {
        HalfLayerExecutionVisit &visit = plan.visits[visit_index];
        visit.purge_transaction = size_t(-1);
        if (visit.tool.physical_tool >= context.loaded_filament_by_tool.size())
            throw std::invalid_argument("Half-layer wipe visit has an invalid physical tool");

        const int loaded = context.loaded_filament_by_tool[visit.tool.physical_tool];
        if (loaded >= 0 && unsigned(loaded) != visit.tool.filament) {
            HalfLayerPurgeTransaction transaction;
            transaction.visit_index = visit_index;
            transaction.physical_tool = visit.tool.physical_tool;
            transaction.source_filament = unsigned(loaded);
            transaction.destination_filament = visit.tool.filament;
            transaction.required_volume_mm3 = context.required_volume_mm3(
                visit.tool.physical_tool, transaction.source_filament, transaction.destination_filament);

            if (allow_claims && !visit.tower_only && transaction.required_volume_mm3 > 0. &&
                context.transition_claimable(transaction.source_filament, transaction.destination_filament)) {
                size_t block_begin = visit.begin;
                while (block_begin < visit.end &&
                       transaction.claimed_volume_mm3 < transaction.required_volume_mm3) {
                    size_t block_end = block_begin + 1;
                    while (block_end < visit.end &&
                           same_half_layer_wipe_root(plan.tasks[block_begin], plan.tasks[block_end]))
                        ++block_end;

                    bool block_eligible = true;
                    double block_volume_mm3 = 0.;
                    for (size_t i = block_begin; i < block_end; ++i) {
                        const HalfLayerExecutionTask &task = plan.tasks[i];
                        if (claimed[i] || !task.wipe_into_eligible ||
                            !std::isfinite(task.wipe_into_volume_mm3) || task.wipe_into_volume_mm3 <= 0. ||
                            task.tool.physical_tool != visit.tool.physical_tool ||
                            task.tool.filament != visit.tool.filament) {
                            block_eligible = false;
                            break;
                        }
                        block_volume_mm3 += task.wipe_into_volume_mm3;
                    }
                    // A non-eligible leading block is a contamination barrier:
                    // later geometry is no longer immediately after the change.
                    if (!block_eligible)
                        break;
                    for (size_t i = block_begin; i < block_end; ++i) {
                        claimed[i] = true;
                        transaction.claimed_task_ids.push_back(i);
                    }
                    transaction.claimed_volume_mm3 += block_volume_mm3;
                    block_begin = block_end;
                }
            }

            transaction.tower_volume_mm3 = std::max(0.,
                transaction.required_volume_mm3 - transaction.claimed_volume_mm3);
            visit.purge_transaction = plan.purge_transactions.size();
            plan.purge_transactions.push_back(std::move(transaction));
        }
        context.loaded_filament_by_tool[visit.tool.physical_tool] = int(visit.tool.filament);
    }
}

// One immutable execution frame is attached to the existing logical LayerTools
// envelope. LayerTools keeps its historical unique material inventory; this
// frame is the ordered execution view and may therefore contain A-B-A visits.
struct HalfLayerExecutionFrame {
    coordf_t print_z = 0.;
    HalfLayerExecutionPlan plan;

    bool valid() const
    {
        if (plan.tasks.empty() || plan.visits.empty())
            return false;
        size_t next = 0;
        std::vector<bool> claimed(plan.tasks.size(), false);
        std::vector<bool> referenced_transactions(plan.purge_transactions.size(), false);
        for (const HalfLayerExecutionVisit &visit : plan.visits) {
            // Validate ranges before any purge transaction indexes task storage.
            if (visit.begin != next || visit.end > plan.tasks.size() ||
                (visit.tower_only ? visit.end != visit.begin : visit.end <= visit.begin))
                return false;
            if (visit.purge_transaction != size_t(-1)) {
                if (visit.purge_transaction >= plan.purge_transactions.size() ||
                    referenced_transactions[visit.purge_transaction])
                    return false;
                referenced_transactions[visit.purge_transaction] = true;
                const HalfLayerPurgeTransaction &transaction = plan.purge_transactions[visit.purge_transaction];
                if (transaction.visit_index != size_t(&visit - plan.visits.data()) ||
                    transaction.physical_tool != visit.tool.physical_tool ||
                    transaction.destination_filament != visit.tool.filament ||
                    transaction.source_filament == transaction.destination_filament ||
                    !std::isfinite(transaction.required_volume_mm3) || transaction.required_volume_mm3 < 0. ||
                    !std::isfinite(transaction.claimed_volume_mm3) || transaction.claimed_volume_mm3 < 0. ||
                    !std::isfinite(transaction.tower_volume_mm3) || transaction.tower_volume_mm3 < 0. ||
                    std::abs(transaction.tower_volume_mm3 - std::max(0.,
                        transaction.required_volume_mm3 - transaction.claimed_volume_mm3)) > 1e-6)
                    return false;
                double claimed_volume = 0.;
                size_t expected_task_id = visit.begin;
                for (size_t task_id : transaction.claimed_task_ids) {
                    if (task_id != expected_task_id++ || task_id >= visit.end || claimed[task_id] ||
                        !plan.tasks[task_id].wipe_into_eligible ||
                        !std::isfinite(plan.tasks[task_id].wipe_into_volume_mm3) ||
                        plan.tasks[task_id].wipe_into_volume_mm3 <= 0. ||
                        plan.tasks[task_id].tool.physical_tool != visit.tool.physical_tool ||
                        plan.tasks[task_id].tool.filament != visit.tool.filament)
                        return false;
                    claimed[task_id] = true;
                    claimed_volume += plan.tasks[task_id].wipe_into_volume_mm3;
                }
                if (std::abs(claimed_volume - transaction.claimed_volume_mm3) > 1e-6)
                    return false;
            }
            if (visit.tower_only) {
                if (visit.begin != next || visit.end != next)
                    return false;
                continue;
            }
            if (visit.begin != next || visit.begin >= visit.end || visit.end > plan.tasks.size())
                return false;
            for (size_t i = visit.begin; i < visit.end; ++i) {
                if (plan.tasks[i].id != i ||
                    plan.tasks[i].tool.physical_tool != visit.tool.physical_tool ||
                    plan.tasks[i].tool.filament != visit.tool.filament ||
                    plan.tasks[i].normal_tool.physical_tool != plan.tasks[i].tool.physical_tool ||
                    (plan.tasks[i].wipe_reservation_id == size_t(-1) &&
                     plan.tasks[i].normal_tool.filament != plan.tasks[i].tool.filament))
                    return false;
                if (plan.tasks[i].wipe_reservation_id != size_t(-1) &&
                    (plan.tasks[i].normal_tool.filament != plan.tasks[i].wipe_reservation_source_filament ||
                     plan.tasks[i].tool.filament != plan.tasks[i].wipe_reservation_destination_filament ||
                     !std::isfinite(plan.tasks[i].normal_extrusion_seconds)))
                    return false;
            }
            next = visit.end;
        }
        for (size_t begin = 0; begin < plan.tasks.size();) {
            size_t end = begin + 1;
            while (end < plan.tasks.size() && same_half_layer_wipe_root(plan.tasks[begin], plan.tasks[end]))
                ++end;
            const bool root_claimed = claimed[begin];
            for (size_t i = begin + 1; i < end; ++i)
                if (claimed[i] != root_claimed)
                    return false;
            begin = end;
        }
        return next == plan.tasks.size() &&
            std::all_of(referenced_transactions.begin(), referenced_transactions.end(), [](bool value) { return value; });
    }
};

struct HalfLayerPrintExecutionPlan {
    std::vector<HalfLayerExecutionFrame> frames;

    bool valid() const
    {
        return std::all_of(frames.begin(), frames.end(),
            [](const HalfLayerExecutionFrame &frame) { return frame.valid(); });
    }
};

// Model-side task producer. Shared by the layer's tool/purge planning and its
// G-code execution; support/priming tasks are added by the layer owner.
void append_half_layer_model_tasks(const PrintObject &object, size_t parent_index,
    const LayerTools &tools, size_t instance_id,
    std::vector<HalfLayerExecutionTask> &lower,
    std::vector<HalfLayerExecutionTask> &core,
    std::vector<HalfLayerExecutionTask> &upper);

// Support-side producer. A logical support event may own several physical
// bands; every borrowed leaf is emitted once for each real print instance.
void append_half_layer_support_tasks(const PrintObject &object, const Layer &logical_event,
    const LayerTools &tools, size_t instance_id, unsigned int incoming_filament,
    std::vector<HalfLayerExecutionTask> &core,
    const SupportLayer *physical_only = nullptr);

// Sum each constituent path once. No G-code is generated or simulated. The
// caller supplies resolved physical-tool/material IDs and logical-layer state.
struct HalfLayerExtrusionMetrics {
    double seconds = 0.;
    double volume_mm3 = 0.;
    HalfLayerExtrusionMetrics &operator+=(const HalfLayerExtrusionMetrics &other)
    {
        seconds += other.seconds;
        volume_mm3 += other.volume_mm3;
        return *this;
    }
};

inline HalfLayerExtrusionMetrics half_layer_entity_metrics(const ExtrusionEntity &entity,
                                        const FullPrintConfig &config,
                                        ExtrusionSpeedContext context)
{
    const auto hint = entity.tool_hint;
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        context.sloped = dynamic_cast<const ExtrusionPathSloped *>(path) != nullptr;
        const auto speed = extrusion_base_speed(config, *path, context, -1., hint);
        const double length = unscale<double>(path->length());
        return {half_layer_path_seconds(length, speed.speed_mm_s), length * speed.effective_mm3_per_mm};
    }
    auto path_seconds = [&](const ExtrusionPaths &paths) {
        HalfLayerExtrusionMetrics metrics;
        for (const auto &path : paths) {
            const auto speed = extrusion_base_speed(config, path, context, -1., hint);
            const double length = unscale<double>(path.length());
            metrics += {half_layer_path_seconds(length, speed.speed_mm_s), length * speed.effective_mm3_per_mm};
        }
        return metrics;
    };
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
        return path_seconds(loop->paths);
    if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity))
        return path_seconds(multi->paths);
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        HalfLayerExtrusionMetrics metrics;
        for (const auto *child : collection->entities)
            if (child != nullptr)
                metrics += half_layer_entity_metrics(*child, config, context);
        return metrics;
    }
    throw std::invalid_argument("Unknown half-layer timed extrusion type");
}

inline double half_layer_entity_seconds(const ExtrusionEntity &entity,
                                        const FullPrintConfig &config,
                                        ExtrusionSpeedContext context)
{
    return half_layer_entity_metrics(entity, config, context).seconds;
}

inline double half_layer_transition_seconds(const HalfLayerExecutionTask &from,
                                            const HalfLayerExecutionTask &to)
{
    if (!from.last_mm.allFinite() || !to.first_mm.allFinite() ||
        !std::isfinite(to.travel_speed_mm_s) || to.travel_speed_mm_s <= 0. ||
        !std::isfinite(to.z_speed_mm_s) || to.z_speed_mm_s <= 0. ||
        !std::isfinite(to.clearance_mm) || to.clearance_mm < 0. ||
        !std::isfinite(to.tool_entry_seconds) || to.tool_entry_seconds < 0.)
        throw std::invalid_argument("Invalid half-layer transition inputs");
    const double xy_mm = (to.first_mm.head<2>() - from.last_mm.head<2>()).norm();
    const double dz_mm = std::abs(to.first_mm.z() - from.last_mm.z());
    double seconds = xy_mm / to.travel_speed_mm_s + (dz_mm + 2. * to.clearance_mm) / to.z_speed_mm_s;
    if (from.tool.physical_tool != to.tool.physical_tool || from.tool.filament != to.tool.filament) {
        const double entry_seconds = from.tool.filament < to.tool_entry_seconds_by_filament.size() ?
            to.tool_entry_seconds_by_filament[from.tool.filament] : to.tool_entry_seconds;
        if (!std::isfinite(entry_seconds) || entry_seconds < 0.)
            throw std::invalid_argument("Invalid half-layer pair transition cost");
        seconds += entry_seconds;
    }
    return seconds;
}

inline double half_layer_initial_entry_seconds(unsigned int incoming_filament,
                                               const HalfLayerExecutionTask &to)
{
    if (incoming_filament == to.tool.filament)
        return 0.;
    const double seconds = incoming_filament < to.tool_entry_seconds_by_filament.size() ?
        to.tool_entry_seconds_by_filament[incoming_filament] : to.tool_entry_seconds;
    if (!std::isfinite(seconds) || seconds < 0.)
        throw std::invalid_argument("Invalid half-layer initial transition cost");
    return seconds;
}

// Inputs are already dependency-ordered by the geometry/routing owner. Only the
// complete upper pass is inserted; every within-group order stays unchanged.
// Core boundaries are legal by construction (whole-loop/path completion).
inline HalfLayerExecutionPlan schedule_half_layer_execution(
    std::vector<HalfLayerExecutionTask> prefix,
    std::vector<HalfLayerExecutionTask> lower,
    std::vector<HalfLayerExecutionTask> core,
    std::vector<HalfLayerExecutionTask> upper,
    double initial_seconds = 0.)
{
    auto validate = [](const auto &tasks) {
        for (const auto &task : tasks)
            if (task.entity == nullptr || task.entity->is_collection() ||
                !std::isfinite(task.extrusion_seconds) || task.extrusion_seconds < 0. ||
                !std::isfinite(task.motion_reference_height) || task.motion_reference_height <= 0.)
                throw std::invalid_argument("Half-layer task must be a complete finite timed path or loop");
    };
    validate(prefix);
    validate(lower);
    validate(core);
    validate(upper);
    for (auto *tasks : {&prefix, &lower, &core, &upper})
        for (HalfLayerExecutionTask &task : *tasks)
            initialize_half_layer_normal_owner(task);
    if (!std::isfinite(initial_seconds) || initial_seconds < 0.)
        throw std::invalid_argument("Half-layer initial timing cost must be finite and nonnegative");
    HalfLayerExecutionPlan out;
    out.initial_seconds = initial_seconds;
    out.lower_begin = prefix.size();
    out.lower_end = prefix.size() + lower.size();
    std::vector<HalfLayerExecutionTask> base;
    base.reserve(prefix.size() + lower.size() + core.size());
    base.insert(base.end(), prefix.begin(), prefix.end());
    base.insert(base.end(), lower.begin(), lower.end());
    base.insert(base.end(), core.begin(), core.end());
    std::vector<double> elapsed(base.size() + 1, initial_seconds);
    for (size_t i = 0; i < base.size(); ++i)
        elapsed[i + 1] = elapsed[i] + base[i].extrusion_seconds +
            (i == 0 ? 0. : half_layer_transition_seconds(base[i - 1], base[i]));
    size_t insertion_index = out.lower_end;
    if (!lower.empty() && !upper.empty()) {
        double upper_seconds = 0.;
        for (size_t i = 0; i < upper.size(); ++i)
            upper_seconds += upper[i].extrusion_seconds +
                (i == 0 ? 0. : half_layer_transition_seconds(upper[i - 1], upper[i]));
        std::vector<HalfLayerInsertionBoundary> boundaries;
        boundaries.reserve(core.size() + 1);
        bool precedence_allowed = true;
        for (size_t i = out.lower_end; i <= base.size(); ++i) {
            if (i > out.lower_end && base[i - 1].requires_upper_before)
                precedence_allowed = false;
            // overrides_key identifies the original island/support root. A
            // root may contain no-sort children whose relative adjacency is a
            // dependency; never insert the delayed wall inside that root.
            const bool complete_block = i == out.lower_end || i == base.size() ||
                base[i - 1].overrides_key == nullptr || base[i].overrides_key == nullptr ||
                base[i - 1].overrides_key != base[i].overrides_key;
            const double approach = half_layer_transition_seconds(base[i - 1], upper.front());
            const double departure = i == base.size() ? 0. : half_layer_transition_seconds(upper.back(), base[i]);
            const double replaced = i == base.size() ? 0. : half_layer_transition_seconds(base[i - 1], base[i]);
            boundaries.push_back({elapsed[i], approach, departure, replaced, precedence_allowed && complete_block});
        }
        double first_start_seconds = elapsed[prefix.size()];
        if (!prefix.empty())
            first_start_seconds += half_layer_transition_seconds(prefix.back(), lower.front());
        out.insertion = select_half_layer_insertion(elapsed.back(), first_start_seconds,
            elapsed[out.lower_end], upper_seconds, boundaries);
        if (out.insertion.boundary == HalfLayerInsertion::unavailable)
            throw std::logic_error("No legal half-layer insertion boundary");
        insertion_index += out.insertion.boundary;
    }
    out.upper_begin = insertion_index;
    out.upper_end = insertion_index + upper.size();
    out.tasks.reserve(base.size() + upper.size());
    out.tasks.insert(out.tasks.end(), base.begin(), base.begin() + insertion_index);
    out.tasks.insert(out.tasks.end(), upper.begin(), upper.end());
    out.tasks.insert(out.tasks.end(), base.begin() + insertion_index, base.end());
    for (size_t i = 0; i < out.tasks.size(); ++i) {
        const auto tool = out.tasks[i].tool;
        if (out.visits.empty() || out.visits.back().tool.physical_tool != tool.physical_tool ||
            out.visits.back().tool.filament != tool.filament)
            out.visits.push_back({tool, i, i + 1, false, size_t(-1)});
        else
            out.visits.back().end = i + 1;
    }
    return out;
}

inline HalfLayerExecutionPlan schedule_half_layer_execution(
    std::vector<HalfLayerExecutionTask> lower,
    std::vector<HalfLayerExecutionTask> core,
    std::vector<HalfLayerExecutionTask> upper)
{
    return schedule_half_layer_execution({}, std::move(lower), std::move(core), std::move(upper), 0.);
}

} // namespace Slic3r
