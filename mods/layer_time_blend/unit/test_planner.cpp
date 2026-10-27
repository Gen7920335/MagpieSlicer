// The layer time blending planner on its own (mods/layer_time_blend/planner.cpp), without the slicer
// core: target times, waits, parking, warnings and tower cuts. The core's side of the slot is tested
// through the built mod in tests/libslic3r/test_layer_time_blend.cpp and tests/fff_print.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "planner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

using namespace LayerTimeBlend;
using Catch::Matchers::WithinAbs;

namespace {

LayerTimeBlend::Settings linear(double max_diff_s, int ease_layers = 0)
{
    LayerTimeBlend::Settings s;
    s.max_diff_s  = max_diff_s;
    s.ease_layers = ease_layers;
    return s;
}

LayerTimeBlend::Settings log_type(double max_ratio_pct, int ease_layers = 0)
{
    LayerTimeBlend::Settings s;
    s.log           = true;
    s.max_ratio_pct = max_ratio_pct;
    s.ease_layers   = ease_layers;
    return s;
}

std::vector<double> blend(const std::vector<double> &t, const LayerTimeBlend::Settings &s)
{
    return LayerTimeBlend::target_times(t, std::vector<bool>(t.size(), true), s);
}

double added(const std::vector<double> &t, const std::vector<double> &target)
{
    double sum = 0.;
    for (size_t i = 0; i < t.size(); ++i)
        sum += target[i] - t[i];
    return sum;
}

// The constraints, written out on their own: T >= t, neighbours within `diff` (s), convex bends
// (second differences) at most `bend` (s) or the input's own bend at that layer where larger, all within
// `tol` (s).
bool feasible(const std::vector<double> &t, const std::vector<double> &T, double diff, double bend, double tol)
{
    for (size_t i = 0; i < T.size(); ++i) {
        if (T[i] < t[i] - tol)
            return false;
        if (i > 0 && std::abs(T[i] - T[i - 1]) > diff + tol)
            return false;
        if (i > 0 && i + 1 < T.size() && T[i + 1] - 2. * T[i] + T[i - 1] > std::max(bend, t[i + 1] - 2. * t[i] + t[i - 1]) + tol)
            return false;
    }
    return true;
}

// Sites at the start of every layer: site k's interval is layer k's time (site 0's is the start G-code).
std::vector<WaitSite> sites_from(const std::vector<double> &layer_times)
{
    std::vector<WaitSite> sites(layer_times.size() + 1);
    sites[0].interval_s = 60.;
    for (size_t k = 1; k < sites.size(); ++k) {
        sites[k].layer_id   = k - 1;
        sites[k].interval_s = layer_times[k - 1];
    }
    return sites;
}

} // namespace

TEST_CASE("Blending raises the layers before a long layer as well as after it", "[LayerTimeBlend]")
{
    // The design note's example, D = 5 s: a 60 s layer after two 10 s layers.
    const std::vector<double> t = {30, 30, 30, 10, 10, 60, 30, 30, 30, 30, 30};
    const std::vector<double> T = blend(t, linear(5.));
    const std::vector<double> expected = {35, 40, 45, 50, 55, 60, 55, 50, 45, 40, 35};
    for (size_t i = 0; i < t.size(); ++i)
        CHECK_THAT(T[i], WithinAbs(expected[i], 1e-9));
    CHECK_THAT(added(t, T), WithinAbs(190., 1e-9));
}

TEST_CASE("Linear blending is the smallest solution found by exhaustive search", "[LayerTimeBlend]")
{
    // Integer times and limit: every bound of the problem is a whole second, so the smallest solution is
    // on the 1 s grid and the search over it finds it exactly.
    std::mt19937 rng(20261019);
    for (int round = 0; round < 40; ++round) {
        const size_t        n = 2 + rng() % 5; // 2..6 layers
        std::vector<double> t(n);
        for (double &x : t)
            x = double(rng() % 9); // 0..8 s
        const double diff = double(1 + rng() % 3);
        const double top  = *std::max_element(t.begin(), t.end()); // a constant at the longest time is always feasible
        std::vector<double> best(n, top);
        std::vector<double> S(t);
        // Every grid vector with t[i] <= S[i] <= top.
        for (;;) {
            if (feasible(t, S, diff, 1e9, 1e-12))
                for (size_t i = 0; i < n; ++i)
                    best[i] = std::min(best[i], S[i]);
            size_t i = 0;
            while (i < n && S[i] + 1. > top) {
                S[i] = t[i];
                ++i;
            }
            if (i == n)
                break;
            S[i] += 1.;
        }
        const std::vector<double> T = blend(t, linear(diff));
        CHECK(feasible(t, T, diff, 1e9, 1e-9));
        for (size_t i = 0; i < n; ++i)
            CHECK_THAT(T[i], WithinAbs(best[i], 1e-9));
    }
}

TEST_CASE("Eased blending is feasible and no higher than any searched solution", "[LayerTimeBlend]")
{
    // With easing the smallest solution need not lie on a grid, so the search bounds it from above:
    // the result must satisfy every constraint and sit at or below every feasible grid vector.
    std::mt19937 rng(7);
    for (int round = 0; round < 25; ++round) {
        const size_t        n = 3 + rng() % 3; // 3..5 layers
        std::vector<double> t(n);
        for (double &x : t)
            x = 0.5 * double(rng() % 13); // 0..6 s in 0.5 s steps
        const int    ease = 1 + int(rng() % 2);
        const double diff = 2.;
        const double bend = diff / double(ease + 1); // the definition the settings document: D / (N + 1)
        const double step = 0.25;                    // grid, s; a divisor of every bound above
        const double top  = *std::max_element(t.begin(), t.end());
        std::vector<double> best(n, top);
        std::vector<double> S(t);
        for (;;) {
            if (feasible(t, S, diff, bend, 1e-12))
                for (size_t i = 0; i < n; ++i)
                    best[i] = std::min(best[i], S[i]);
            size_t i = 0;
            while (i < n && S[i] + step > top + 1e-12) {
                S[i] = t[i];
                ++i;
            }
            if (i == n)
                break;
            S[i] += step;
        }
        const std::vector<double> T = blend(t, linear(diff, ease));
        CHECK(feasible(t, T, diff, bend, 1e-9));
        for (size_t i = 0; i < n; ++i)
            CHECK(T[i] <= best[i] + 1e-9);
    }
}

TEST_CASE("Blending reproduces the totals of the linear program", "[LayerTimeBlend]")
{
    // Added seconds from the 2026-10-19 design check, which solved each case as a linear program
    // (D = 5 s, r = 20 %); its table rounds to whole seconds. The table's N = 5 bounded the bend by D / N,
    // before the setting was redefined as D / (N + 1): that is ease_layers = 4 here. With ease_layers = 5
    // the totals come out 5625.8, 66.9 and 1093.9 s, which is how the table's definition was identified.
    std::vector<double> spike(101, 30.);
    spike[50] = 200.;
    std::vector<double> step(40, 3.);
    std::fill(step.begin() + 20, step.end(), 30.);

    CHECK_THAT(added(spike, blend(spike, linear(5.))), WithinAbs(5610., 1e-6));
    CHECK_THAT(added(spike, blend(spike, linear(5., 4))), WithinAbs(5620., 1.));
    CHECK_THAT(added(step, blend(step, linear(5.))), WithinAbs(60., 1e-6));
    CHECK_THAT(added(step, blend(step, linear(5., 4))), WithinAbs(65., 1.));
    CHECK_THAT(added(spike, blend(spike, log_type(20.))), WithinAbs(1077., 1.));
    CHECK_THAT(added(spike, blend(spike, log_type(20., 4))), WithinAbs(1089., 1.));
    CHECK_THAT(added(step, blend(step, log_type(20.))), WithinAbs(97., 1.));
    CHECK_THAT(added(step, blend(step, log_type(20., 4))), WithinAbs(98., 1.));
}

TEST_CASE("Easing rounds ramp starts and ends but leaves the long layer's peak sharp", "[LayerTimeBlend]")
{
    std::vector<double> spike(61, 30.);
    spike[30] = 100.;
    const std::vector<double> T = blend(spike, linear(5., 4));
    const double bend = 5. / 5.; // D / (N + 1)
    CHECK(feasible(spike, T, 5., bend, 1e-9));
    // The peak keeps its own time: rounding it would lift the layers around it.
    CHECK_THAT(T[30], WithinAbs(100., 1e-9));
    // Next to the peak the full slope applies; the bend there is concave, which easing does not limit.
    CHECK_THAT(T[29], WithinAbs(95., 1e-9));
    // With N = 0 the same input bends at once.
    const std::vector<double> sharp = blend(spike, linear(5.));
    CHECK(added(spike, T) > added(spike, sharp));
}

TEST_CASE("Easing leaves the bends the layer times already have", "[LayerTimeBlend]")
{
    // Alternating 30/27 s layers (infill turning every layer): every pair is within D = 5 s, so nothing waits
    // without easing. Each short layer is a convex bend of 6 s, over D / (N + 1) = 0.83 s at N = 5; easing
    // bounds only the bends blending makes, so these stay. Before it bounded every bend: +255.7 s over 200
    // layers (2026-10-27).
    std::vector<double> jitter(200);
    for (size_t i = 0; i < jitter.size(); ++i)
        jitter[i] = i % 2 == 0 ? 30. : 27.;
    CHECK_THAT(added(jitter, blend(jitter, linear(5., 5))), WithinAbs(0., 1e-9));
    CHECK_THAT(added(jitter, blend(jitter, log_type(20., 5))), WithinAbs(0., 1e-9));
    // One short layer within the limit of both neighbours: no wait at any N.
    const std::vector<double> dip{ 30., 25., 30. };
    CHECK_THAT(added(dip, blend(dip, linear(5., 5))), WithinAbs(0., 1e-9));
    // A ramp blending makes is still rounded: the same jitter under a long layer waits more with easing.
    std::vector<double> ramp = jitter;
    ramp[100] = 80.;
    const double sharp = added(ramp, blend(ramp, linear(5.)));
    const double eased = added(ramp, blend(ramp, linear(5., 5)));
    CHECK_THAT(sharp, WithinAbs(480., 1e-6));
    CHECK_THAT(eased, WithinAbs(482.333, 1e-3)); // the 2026-10-27 mirror's figure
    CHECK(eased > sharp);
    CHECK(feasible(ramp, blend(ramp, linear(5., 5)), 5., 5. / 6., 1e-9));
}

TEST_CASE("Limit exceptions minimise the waits plus their cost", "[LayerTimeBlend]")
{
    // The waits with exception cost c are the least of sum(T) + c * sum(max(0, |T[i] - T[i-1]| - D)) over
    // T >= t. A grid search over small chains finds that least value on its own.
    auto cost_of = [](const std::vector<double> &t, const std::vector<double> &T, double diff, double c) {
        double sum = 0.;
        for (size_t i = 0; i < T.size(); ++i) {
            sum += T[i] - t[i];
            if (i > 0)
                sum += c * std::max(0., std::abs(T[i] - T[i - 1]) - diff);
        }
        return sum;
    };
    std::mt19937 rng(11);
    for (int round = 0; round < 60; ++round) {
        const size_t        n = 2 + rng() % 3; // 2..4 layers
        std::vector<double> t(n);
        for (double &x : t)
            x = 0.5 * double(rng() % 17); // 0..8 s in 0.5 s steps
        const double diff = 1. + double(rng() % 2);            // s
        const double c    = (std::vector<double>{ 0.4, 1., 1.5, 3. })[rng() % 4];
        const double step = 0.5;                                // grid, s; the optimum lies on it (breakpoints are sums of t and D)
        const double top  = *std::max_element(t.begin(), t.end());
        double       best = std::numeric_limits<double>::infinity();
        std::vector<double> S(t);
        for (;;) {
            best = std::min(best, cost_of(t, S, diff, c));
            size_t i = 0;
            while (i < n && S[i] + step > top + 1e-12) {
                S[i] = t[i];
                ++i;
            }
            if (i == n)
                break;
            S[i] += step;
        }
        LayerTimeBlend::Settings s = linear(diff);
        s.exception_cost           = c;
        const std::vector<double> T = blend(t, s);
        for (size_t i = 0; i < n; ++i)
            CHECK(T[i] >= t[i] - 1e-12);
        CHECK_THAT(cost_of(t, T, diff, c), WithinAbs(best, 1e-9));
    }
}

TEST_CASE("A long layer breaks the limit instead of spreading waits when that is cheaper", "[LayerTimeBlend]")
{
    std::vector<double> spike(121, 30.);
    spike[60] = 200.;
    LayerTimeBlend::Settings s = linear(5.);
    CHECK_THAT(added(spike, blend(spike, s)), WithinAbs(5610., 1e-6));
    // Figures from the 2026-10-27 prototype checked against an LP (reports/LTB_soft_proto.py).
    s.exception_cost = 1.;
    CHECK_THAT(added(spike, blend(spike, s)), WithinAbs(0., 1e-9));
    s.exception_cost = 3.;
    CHECK_THAT(added(spike, blend(spike, s)), WithinAbs(30., 1e-6));
    s.exception_cost = 10.;
    CHECK_THAT(added(spike, blend(spike, s)), WithinAbs(450., 1e-6));
    // With easing the pairs that break the limit keep their jump and the ramps are rounded as before.
    s.ease_layers = 4;
    const std::vector<double> T = blend(spike, s);
    CHECK(added(spike, T) >= 450. - 1e-6);
    CHECK(added(spike, T) < 5610.);
    // Off: the limit holds everywhere.
    s.exception_cost = 0.;
    s.ease_layers    = 0;
    const std::vector<double> strict = blend(spike, s);
    CHECK(feasible(spike, strict, 5., 1e9, 1e-9));
}

TEST_CASE("Log blending limits the ratio of neighbouring layer times", "[LayerTimeBlend]")
{
    const std::vector<double> t = {10, 10, 10, 40, 10, 10, 10};
    const std::vector<double> T = blend(t, log_type(25.));
    for (size_t i = 1; i < T.size(); ++i)
        CHECK(std::max(T[i], T[i - 1]) / std::min(T[i], T[i - 1]) <= 1.25 + 1e-9);
    CHECK_THAT(T[3], WithinAbs(40., 1e-9));
    CHECK_THAT(T[2], WithinAbs(32., 1e-9));
    CHECK_THAT(T[1], WithinAbs(25.6, 1e-9));
}

TEST_CASE("A cut link blends each side on its own", "[LayerTimeBlend]")
{
    const std::vector<double> t = {50, 10, 10};
    std::vector<bool>         links = {true, false, true};
    const std::vector<double> T = LayerTimeBlend::target_times(t, links, linear(5.));
    CHECK_THAT(T[1], WithinAbs(10., 1e-9));
    CHECK_THAT(T[2], WithinAbs(10., 1e-9));
}

TEST_CASE("The first layer neither gets a wait nor limits the second", "[LayerTimeBlend]")
{
    // Layer 1 is slow (60 s); with start_layer 2 the layers above it keep their 20 s.
    const std::vector<WaitSite> sites = sites_from({60, 20, 20, 20});
    const WaitPlan              plan  = LayerTimeBlend::plan(sites, linear(5.), 1000.);
    REQUIRE(plan.added_s.size() == sites.size());
    for (double a : plan.added_s)
        CHECK(a == 0.);
    // From layer 1 on, layer 1's 60 s pulls layer 2 up to 55 s.
    LayerTimeBlend::Settings from_first = linear(5.);
    from_first.start_layer              = 1;
    const WaitPlan all = LayerTimeBlend::plan(sites, from_first, 1000.);
    CHECK_THAT(all.added_s[2], WithinAbs(35., 1e-9));
    CHECK_THAT(all.target_s[2], WithinAbs(55., 1e-9));
    CHECK(all.added_s[1] == 0.);
}

TEST_CASE("A wait cap limits each wait", "[LayerTimeBlend]")
{
    LayerTimeBlend::Settings s = linear(5.);
    s.wait_cap_s               = 12.;
    const WaitPlan plan        = LayerTimeBlend::plan(sites_from({10, 10, 10, 40, 10}), s, 1000.);
    // Uncapped the layer before the 40 s one would wait 25 s.
    CHECK_THAT(plan.added_s[3], WithinAbs(12., 1e-9));
    CHECK_THAT(plan.dwell_s[3], WithinAbs(12., 1e-9));
}

TEST_CASE("A long wait parks only where the park trip fits in it", "[LayerTimeBlend]")
{
    std::vector<WaitSite> sites = sites_from({10, 10, 10, 40, 10});
    for (WaitSite &site : sites)
        site.park_trip_s = 4.;
    LayerTimeBlend::Settings s = linear(5.);
    s.park                     = true;
    s.park_min_s               = 15.; // waits from 15 s park; the trip (4 s) is shorter than that
    const WaitPlan plan        = LayerTimeBlend::plan(sites, s, 1000.);
    // Waits: layer 2 -> 20 s, layer 3 -> 25 s, layer 5 -> 25 s; layer 1 is below start_layer.
    CHECK(plan.use_park[3]);
    CHECK_THAT(plan.dwell_s[3], WithinAbs(25. - 4., 1e-9));
    CHECK_THAT(plan.added_s[3], WithinAbs(25., 1e-9));
    // A threshold equal to the 20 s wait parks; just above it waits in place.
    for (double threshold : {20., 20.0001}) {
        s.park_min_s         = threshold;
        const WaitPlan edge  = LayerTimeBlend::plan(sites, s, 1000.);
        const bool     parks = threshold <= 20.;
        CHECK(edge.use_park[2] == parks);
        CHECK_THAT(edge.dwell_s[2], WithinAbs(parks ? 16. : 20., 1e-9));
    }
}

TEST_CASE("Warnings name the long layer and the range its waits cover", "[LayerTimeBlend]")
{
    // Layer 6 takes 200 s among 30 s layers: every other layer from 2 on waits (sites 2..5 and 7..12).
    std::vector<double> layers(12, 30.);
    layers[5] = 200.;
    const std::vector<WaitSite> sites = sites_from(layers);
    const double                print_time = 1000.;
    const WaitPlan              plan       = LayerTimeBlend::plan(sites, linear(5.), print_time);
    const double total = std::accumulate(plan.added_s.begin(), plan.added_s.end(), 0.);
    REQUIRE(total > 0.15 * print_time);
    bool share = false, single = false;
    for (const WaitWarning &w : plan.warnings) {
        if (w.kind == WaitWarning::Kind::TotalShare) {
            share = true;
            CHECK_THAT(w.value, WithinAbs(100. * total / print_time, 1e-9));
        }
        if (w.kind == WaitWarning::Kind::SingleWait) {
            single = true;
            CHECK_THAT(w.value, WithinAbs(165., 1e-9)); // layer 5 (site 5) waits 200 - 5 - 30
        }
        CHECK(w.source_site == 6);
        CHECK(w.first_site == 2);
        CHECK(w.last_site == 5);
    }
    CHECK(share);
    CHECK(single);
}

TEST_CASE("Warning thresholds compare at their own values", "[LayerTimeBlend]")
{
    // One wait on layer 2 before a 40 s layer: 10 10 40 with D = 15 s -> layer 2 waits 15 s.
    const std::vector<WaitSite> sites = sites_from({10, 10, 40, 40});
    for (double limit : {14.999, 15., 15.001}) {
        LayerTimeBlend::Settings s = linear(15.);
        s.warn_single_s            = limit;
        s.warn_total_pct           = 0.;
        const WaitPlan plan        = LayerTimeBlend::plan(sites, s, 1000.);
        CHECK_THAT(plan.added_s[2], WithinAbs(15., 1e-9));
        CHECK(plan.warnings.empty() == (limit >= 15.));
    }
    // Total share: 15 s of 100 s is 15 %.
    for (double pct : {14.999, 15., 15.001}) {
        LayerTimeBlend::Settings s = linear(15.);
        s.warn_single_s            = 0.;
        s.warn_total_pct           = pct;
        const WaitPlan plan        = LayerTimeBlend::plan(sites, s, 100.);
        CHECK(plan.warnings.empty() == (pct >= 15.));
    }
}

TEST_CASE("A guessed interval next to a wait is reported", "[LayerTimeBlend]")
{
    std::vector<WaitSite> sites = sites_from({10, 10, 40, 40, 40});
    sites[3].uncertain = true; // next to layer 2's wait
    sites[5].uncertain = true; // no wait near it
    const WaitPlan plan = LayerTimeBlend::plan(sites, linear(15.), 1000.);
    REQUIRE(plan.warnings.size() == 1);
    CHECK(plan.warnings[0].kind == WaitWarning::Kind::Uncertain);
    CHECK(plan.warnings[0].value == 1.);
    CHECK(plan.warnings[0].first_site == 3);
}

TEST_CASE("Each object of a by-object print is blended on its own from its own start layer", "[LayerTimeBlend]")
{
    // Object A: layers 60 20 20; object B starts at site 4: layers 60 20 10 40.
    std::vector<WaitSite> sites = sites_from({60, 20, 20, 30, 60, 20, 10, 40});
    sites[4].constraint_break   = true; // B's first layer starts here; site 4's interval (A's last layer) is no layer time
    const WaitPlan plan         = LayerTimeBlend::plan(sites, linear(5.), 10000.);
    // A: layers 2 and 3 (sites 2, 3) take 20 s each; nothing waits.
    CHECK(plan.added_s[2] == 0.);
    CHECK(plan.added_s[3] == 0.);
    CHECK(plan.added_s[4] == 0.);
    // B: its layer 1 (site 5, 60 s) is below start_layer and does not pull layer 2 up.
    CHECK(plan.added_s[5] == 0.);
    CHECK_THAT(plan.added_s[6], WithinAbs(30. - 20., 1e-9)); // 20 -> 30, then 35, then 40
    CHECK_THAT(plan.added_s[7], WithinAbs(35. - 10., 1e-9));
    CHECK(plan.added_s[8] == 0.);
}

TEST_CASE("Tower blocks are kept up to where leaving them out keeps every limit", "[LayerTimeBlend]")
{
    // Layers 1..10, 20 s each except layer 4 (40 s); every site has a tower block of 4 s whose site comes 1 s in.
    std::vector<double> layers(10, 20.);
    layers[3] = 40.;
    std::vector<WaitSite> sites = sites_from(layers);
    for (WaitSite &site : sites) {
        site.tower_pre_s   = 1.;
        site.tower_block_s = 4.;
    }
    const WaitPlan plan = LayerTimeBlend::plan(sites, linear(5.), 10000.);
    // D = 5 s around the 40 s layer (site 4): sites 2, 3 -> 30, 35 s; 5, 6, 7 -> 35, 30, 25 s; site 8 keeps 20 s.
    CHECK_THAT(plan.added_s[7], WithinAbs(5., 1e-9));
    CHECK(plan.added_s[8] == 0.);
    // Without the tower from site 8, layer 8 ends 1 s early (19 s) next to layer 7's 25 s: 6 s apart, too far.
    // From site 9: layer 8 keeps 20 s, layer 9 takes 19 s, layer 10 20 - 1 + 1 - 4 = 16 s: within 5 s.
    CHECK(plan.tower_end == 9);

    // No wait at all: no tower.
    std::vector<WaitSite> even = sites_from(std::vector<double>(6, 20.));
    for (WaitSite &site : even) {
        site.tower_pre_s   = 1.;
        site.tower_block_s = 4.;
    }
    CHECK(LayerTimeBlend::plan(even, linear(5.), 1000.).tower_end == 0);
    // Without tower blocks every site keeps everything.
    CHECK(LayerTimeBlend::plan(sites_from(layers), linear(5.), 1000.).tower_end == 11);
}
