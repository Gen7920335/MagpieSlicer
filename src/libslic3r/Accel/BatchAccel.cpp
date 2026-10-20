#include "BatchAccel.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>

#include <boost/log/trivial.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/StageTimer.hpp"
#include "libslic3r/ModHost/NativeMods.hpp"

#include "../../../mods/sdk/include/magpie_accel.h"

namespace Slic3r::Accel {

namespace {

static_assert(uint32_t(Op::FacetLayerRanges) == MP_ACCEL_OP_FACET_LAYER_RANGES &&
                  uint32_t(Op::VerticalCrossings) == MP_ACCEL_OP_VERTICAL_CROSSINGS &&
                  uint32_t(Op::MinDistances) == MP_ACCEL_OP_MIN_DIST2 &&
                  uint32_t(Op::PointsInExPolygons) == MP_ACCEL_OP_POINTS_IN_EXPOLYS,
              "Op must number the operations as magpie_accel.h does");
// The core's points go to the accelerator as they lie in memory (see NativeMods.cpp).
static_assert(sizeof(Point) == sizeof(mp_point) && sizeof(coord_t) == sizeof(int64_t), "Point must be an mp_point");
static_assert(sizeof(std::array<float, 3>) == 3 * sizeof(float), "facet heights must be packed floats");

struct SessionState
{
    std::shared_ptr<const ModHost::NativeModSet> mods; // keeps the library loaded for the session
    const ModHost::NativeEntry                  *entry = nullptr;
    const mp_accel_batch_v1                     *table = nullptr;
    int32_t                                      mode  = MP_ACCEL_OFF;
};

std::atomic<const SessionState *> g_session{nullptr};
std::atomic<uint64_t>             g_rejected{0};
// The accelerator mod of the current or last session (session_mod).
std::mutex                        g_session_mod_mutex;
std::string                       g_session_mod;

// Per operation, for the session: time spent in the accelerator on accepted batches (the call into the
// mod, packing and decoding included) and, in Verify mode, time the CPU reference took on the same
// batches. Same batches, same items: the two times compare the accelerator with the CPU directly.
struct OpTiming
{
    std::atomic<uint64_t> batches{0}, items{0}, accel_ns{0};
    std::atomic<uint64_t> checked_batches{0}, cpu_ns{0};
    void reset() { batches = items = accel_ns = checked_batches = cpu_ns = 0; }
};
constexpr const char *op_names[] = {"facet layer ranges", "vertical crossings", "min distances", "points in expolygons"};
OpTiming g_timing[4];

uint64_t elapsed_ns(std::chrono::steady_clock::time_point since)
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

const char *mode_text(int32_t mode)
{
    return mode == MP_ACCEL_TRUSTED ? "Trusted" : mode == MP_ACCEL_VERIFY ? "Verify" : "Off";
}

bool accepts(const SessionState &s, Op op, size_t items)
{
    int32_t taken = 0;
    const int32_t rc = s.entry->mod->guarded_call([&]() {
        taken = s.table->accepts(s.entry->user, uint32_t(op), uint64_t(items));
        return int32_t(MP_OK);
    });
    return rc == MP_OK && taken != 0;
}

// Runs one batch: `call(state)` fills the decoded results into `out` and returns the mod's status. In
// Verify mode every item is then compared with the caller's CPU calculation.
template<class T, class Call>
bool run_batch(Op op, size_t count, const Call &call, const std::function<T(size_t)> &cpu, std::vector<T> &out)
{
    const SessionState *s = g_session.load(std::memory_order_acquire);
    if (s == nullptr || count == 0 || !accepts(*s, op, count))
        return false;
    MAGPIE_STAGE_TIMER("accel.batch");
    OpTiming     &timing  = g_timing[uint32_t(op)];
    const auto    started = std::chrono::steady_clock::now();
    std::string   message;
    const int32_t rc = s->entry->mod->guarded_call([&]() { return call(*s); }, &message);
    const uint64_t accel_ns = elapsed_ns(started);
    if (rc != MP_OK) {
        // A decline (device busy, batch too large, driver error the mod logged) or a crash, which
        // guarded_call logged and which disabled the mod: the CPU computes the batch either way.
        out.clear();
        return false;
    }
    if (s->mode == MP_ACCEL_VERIFY) {
        const auto cpu_started = std::chrono::steady_clock::now();
        for (size_t i = 0; i < count; ++i)
            if (!(out[i] == cpu(i))) {
                // PERMANENT DIAGNOSTIC: an accelerator that drifted from the CPU reference shows up here first.
                BOOST_LOG_TRIVIAL(error) << "Accelerator " << s->entry->mod->id() << ": operation " << uint32_t(op) << ", item " << i
                                         << " of " << count << " differs from the CPU; batch discarded";
                ++g_rejected;
                out.clear();
                return false;
            }
        // Only whole batches are timed, so the CPU time covers exactly the items the accelerator computed.
        timing.cpu_ns += elapsed_ns(cpu_started);
        ++timing.checked_batches;
    }
    timing.accel_ns += accel_ns;
    timing.items += count;
    ++timing.batches;
    return true;
}

} // namespace

Session::Session(const DynamicPrintConfig &full_print_config)
{
    g_rejected = 0;
    {
        std::lock_guard<std::mutex> lock(g_session_mod_mutex);
        g_session_mod.clear();
    }
    for (OpTiming &t : g_timing)
        t.reset();
    std::shared_ptr<const ModHost::NativeModSet> mods = ModHost::active_native_mods();
    if (!mods)
        return;
    for (const ModHost::NativeEntry *entry : mods->slots(MP_ACCEL_SLOT)) {
        if (entry->version < MP_ACCEL_SLOT_VERSION || entry->table_size < sizeof(mp_accel_batch_v1)) {
            BOOST_LOG_TRIVIAL(warning) << "Accelerator " << entry->mod->id() << ": its accel.batch table is too old; not used";
            continue;
        }
        const auto *table = static_cast<const mp_accel_batch_v1 *>(entry->table);
        mp_ctx      ctx;
        ctx.mod          = entry->mod;
        ctx.print_config = &full_print_config;
        int32_t       mode = MP_ACCEL_OFF;
        mp_status     st{MP_OK, nullptr};
        std::string   message;
        const int32_t rc = entry->mod->guarded_call([&]() {
            st = table->begin(entry->user, &ctx, &mode);
            return st.code;
        }, &message);
        if (rc != MP_OK) {
            BOOST_LOG_TRIVIAL(warning) << "Accelerator " << entry->mod->id() << " did not start: "
                                       << (!message.empty() ? message : st.message ? std::string(st.message) : std::to_string(rc));
            continue;
        }
        if (mode == MP_ACCEL_OFF)
            continue;
        if (mode != MP_ACCEL_VERIFY && mode != MP_ACCEL_TRUSTED) {
            BOOST_LOG_TRIVIAL(warning) << "Accelerator " << entry->mod->id() << " asked for unknown mode " << mode << "; not used";
            continue;
        }
        if (mode == MP_ACCEL_TRUSTED && !entry->mod->manifest().official) {
            // PERMANENT DIAGNOSTIC: why a mod set to skip the CPU check is checked anyway.
            BOOST_LOG_TRIVIAL(warning) << "Accelerator " << entry->mod->id()
                                       << " asked to skip the CPU check, which only official mods may; its results are checked";
            mode = MP_ACCEL_VERIFY;
        }
        auto *state  = new SessionState;
        state->mods  = mods;
        state->entry = entry;
        state->table = table;
        state->mode  = mode;
        g_session.store(state, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_session_mod_mutex);
            g_session_mod = entry->mod->id();
        }
        BOOST_LOG_TRIVIAL(info) << "Accelerator " << entry->mod->id() << ": " << mode_text(mode);
        break;
    }
}

Session::~Session()
{
    const SessionState *s = g_session.exchange(nullptr, std::memory_order_acq_rel);
    if (s == nullptr)
        return;
    s->entry->mod->guarded_call([&]() {
        s->table->end(s->entry->user);
        return int32_t(MP_OK);
    });
    if (g_rejected > 0)
        BOOST_LOG_TRIVIAL(warning) << "Accelerator " << s->entry->mod->id() << ": " << g_rejected.load()
                                   << " batch(es) differed from the CPU and were computed on the CPU";
    const std::string text = "Accelerator " + s->entry->mod->id() + " (" + mode_text(s->mode) + "):\n" + timing_report();
    BOOST_LOG_TRIVIAL(info) << text;
    if (const char *env = std::getenv("MAGPIE_ACCEL_REPORT"); env != nullptr && std::string(env) == "1")
        std::fprintf(stderr, "%s", text.c_str());
    delete s;
}

bool active() { return g_session.load(std::memory_order_acquire) != nullptr; }

bool should_dispatch(Op op, size_t items)
{
    const SessionState *s = g_session.load(std::memory_order_acquire);
    return s != nullptr && items > 0 && accepts(*s, op, items);
}

const char *mode_name()
{
    const SessionState *s = g_session.load(std::memory_order_acquire);
    return mode_text(s ? s->mode : MP_ACCEL_OFF);
}

uint64_t rejected_batches() { return g_rejected.load(); }

std::string session_mod()
{
    std::lock_guard<std::mutex> lock(g_session_mod_mutex);
    return g_session_mod;
}

OpTimes op_times(Op op)
{
    const OpTiming &t = g_timing[uint32_t(op)];
    return {t.batches.load(), t.items.load(), double(t.accel_ns.load()) * 1e-6, t.checked_batches.load(), double(t.cpu_ns.load()) * 1e-6};
}

std::string timing_report()
{
    std::ostringstream out;
    for (uint32_t op = 0; op < 4; ++op) {
        const OpTimes t = op_times(Op(op));
        out << "  " << op_names[op] << ": " << t.batches << " batches, " << t.items << " items, accelerator " << t.accel_ms << " ms";
        if (t.checked_batches > 0)
            out << "; CPU on the same batches " << t.cpu_ms << " ms";
        out << "\n";
    }
    return out.str();
}

bool mesh_layer_ranges(const std::vector<std::array<float, 3>> &facet_z, const std::vector<float> &zs,
                       const std::function<LayerRange(size_t)> &cpu, std::vector<LayerRange> &out)
{
    if (zs.empty())
        return false;
    std::vector<uint32_t> ranges;
    auto call = [&](const SessionState &s) {
        ranges.assign(facet_z.size() * 3, 0);
        const mp_status st = s.table->facet_layer_ranges(s.entry->user, facet_z.front().data(), facet_z.size(), zs.data(),
                                                         zs.size(), ranges.data());
        if (st.code == MP_OK) {
            out.resize(facet_z.size());
            for (size_t i = 0; i < facet_z.size(); ++i)
                out[i] = {ranges[3 * i], ranges[3 * i + 1], ranges[3 * i + 2]};
        }
        return st.code;
    };
    return run_batch<LayerRange>(Op::FacetLayerRanges, facet_z.size(), call, cpu, out);
}

bool vertical_intersections(const std::vector<VerticalCrossing> &crossings, const std::function<Rational(size_t)> &cpu,
                            std::vector<Rational> &out)
{
    auto call = [&](const SessionState &s) {
        std::vector<int64_t> input;
        input.reserve(crossings.size() * 5);
        for (const VerticalCrossing &c : crossings)
            input.insert(input.end(), {int64_t(c.p1.x()), int64_t(c.p1.y()), int64_t(c.p2.x()), int64_t(c.p2.y()), int64_t(c.x)});
        std::vector<int64_t>  p(crossings.size());
        std::vector<uint32_t> q(crossings.size());
        const mp_status st = s.table->vertical_crossings(s.entry->user, input.data(), crossings.size(), p.data(), q.data());
        if (st.code == MP_OK) {
            out.resize(crossings.size());
            for (size_t i = 0; i < crossings.size(); ++i)
                out[i] = {p[i], q[i]};
        }
        return st.code;
    };
    return run_batch<Rational>(Op::VerticalCrossings, crossings.size(), call, cpu, out);
}

bool min_distances_squared(const Points &points, const Lines &edges, const std::function<double(size_t)> &cpu,
                           std::vector<double> &out)
{
    auto call = [&](const SessionState &s) {
        std::vector<mp_point> ends;
        ends.reserve(edges.size() * 2);
        for (const Line &e : edges) {
            ends.push_back({e.a.x(), e.a.y()});
            ends.push_back({e.b.x(), e.b.y()});
        }
        out.assign(points.size(), 0.);
        return s.table->min_dist2(s.entry->user, reinterpret_cast<const mp_point *>(points.data()), points.size(),
                                  ends.data(), edges.size(), out.data()).code;
    };
    return run_batch<double>(Op::MinDistances, points.size(), call, cpu, out);
}

bool points_in_expolygons(const Points &points, const ExPolygons &expolygons, const std::function<bool(size_t)> &cpu,
                          std::vector<uint8_t> &out)
{
    auto call = [&](const SessionState &s) {
        // Views of the core's own rings, contour first, as mp_expolys reads them.
        std::vector<std::vector<mp_ring>> rings(expolygons.size());
        std::vector<mp_expoly>            items(expolygons.size());
        for (size_t i = 0; i < expolygons.size(); ++i) {
            const ExPolygon &e = expolygons[i];
            rings[i].push_back({reinterpret_cast<const mp_point *>(e.contour.points.data()), uint32_t(e.contour.points.size())});
            for (const Polygon &h : e.holes)
                rings[i].push_back({reinterpret_cast<const mp_point *>(h.points.data()), uint32_t(h.points.size())});
            items[i] = {rings[i].data(), uint32_t(rings[i].size())};
        }
        const mp_expolys polys{items.data(), uint32_t(items.size())};
        out.assign(points.size(), 0);
        return s.table->points_in_expolys(s.entry->user, reinterpret_cast<const mp_point *>(points.data()), points.size(), &polys,
                                          out.data()).code;
    };
    return run_batch<uint8_t>(Op::PointsInExPolygons, points.size(), call,
                              std::function<uint8_t(size_t)>([&cpu](size_t i) { return uint8_t(cpu(i) ? 1 : 0); }), out);
}

} // namespace Slic3r::Accel
