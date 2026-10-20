#include "planner.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace LayerTimeBlend {

namespace {

std::function<void(const std::string &)> &diagnostic_sink()
{
    static std::function<void(const std::string &)> sink;
    return sink;
}

void diagnostic(const std::string &text)
{
    if (diagnostic_sink())
        diagnostic_sink()(text);
}

// Shortest layer time the log type works with, s. A layer time is never this short in practice; it
// keeps log() finite for an empty interval.
constexpr double min_log_time_s = 1e-3;

// Pointwise smallest v' >= v with |v'[i] - v'[i-1]| <= limit: the upper envelope of cones of slope
// `limit` standing on every point, found by one pass each way.
void lipschitz_envelope(std::vector<double> &v, double limit)
{
    for (size_t i = 1; i < v.size(); ++i)
        v[i] = std::max(v[i], v[i - 1] - limit);
    for (size_t i = v.size() - 1; i-- > 0;)
        v[i] = std::max(v[i], v[i + 1] - limit);
}

// The smallest concave sequence >= w (its upper hull, evaluated at every index).
void concave_majorant(std::vector<double> &w)
{
    const size_t n = w.size();
    if (n < 3)
        return;
    std::vector<size_t> hull;
    hull.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        // Drop the last hull point while it lies on or under the chord from the one before it to i.
        while (hull.size() >= 2) {
            const size_t a = hull[hull.size() - 2], b = hull.back();
            const double cross = (double(b) - double(a)) * (w[i] - w[a]) - (w[b] - w[a]) * (double(i) - double(a));
            if (cross < 0.)
                break;
            hull.pop_back();
        }
        hull.push_back(i);
    }
    for (size_t h = 1; h < hull.size(); ++h) {
        const size_t a = hull[h - 1], b = hull[h];
        for (size_t i = a + 1; i < b; ++i)
            w[i] = std::max(w[i], w[a] + (w[b] - w[a]) * double(i - a) / double(b - a));
    }
}

// One chain of linked layers, in the space the limit applies to (seconds, or log seconds).
std::vector<double> blend_chain(std::vector<double> v, double limit, int ease_layers)
{
    if (v.size() < 2 || !(limit >= 0.))
        return v;
    if (ease_layers <= 0) {
        lipschitz_envelope(v, limit);
        return v;
    }
    // Convex bends (ramp starts and ends) take at most `curvature` of second difference: v - curvature * i^2 / 2
    // must be concave. Both constraint sets are closed under the pointwise minimum, so the smallest
    // solution exists; alternating the two projections, each of which only raises values, reaches it.
    const double curvature = limit / double(ease_layers + 1);
    auto         q         = [curvature](size_t i) { return curvature * double(i) * double(i) * 0.5; };
    // Progress: every round raises v (both steps are increasing and never lower a value), and v is
    // bounded by the envelope of max(t). The 2026-10-19 check against an LP found 2 rounds in every
    // case; the cap only guards a bug.
    constexpr int max_rounds = 64;
    for (int round = 0; round < max_rounds; ++round) {
        lipschitz_envelope(v, limit);
        std::vector<double> w(v.size());
        for (size_t i = 0; i < v.size(); ++i)
            w[i] = v[i] - q(i);
        concave_majorant(w);
        bool changed = false;
        for (size_t i = 0; i < v.size(); ++i) {
            const double raised = w[i] + q(i);
            // Change that counts as a raise, in the chain's units (s or log s): far below a millisecond.
            const double tolerance = 1e-9 * std::max(1., std::abs(v[i]));
            if (raised > v[i] + tolerance)
                changed = true;
            v[i] = std::max(v[i], raised);
        }
        if (!changed)
            return v;
    }
    // PERMANENT DIAGNOSTIC: the alternation did not settle, which the closedness argument rules out.
    diagnostic("Layer time blending: easing did not converge in " + std::to_string(max_rounds) + " rounds over " +
               std::to_string(v.size()) + " layers");
    lipschitz_envelope(v, limit);
    return v;
}

} // namespace

std::vector<double> target_times(const std::vector<double> &t, const std::vector<bool> &links, const Settings &settings)
{
    std::vector<double> out(t.size());
    const double        limit = settings.log ? std::log1p(std::max(0., settings.max_ratio_pct) / 100.) : std::max(0., settings.max_diff_s);
    size_t              begin = 0;
    while (begin < t.size()) {
        size_t end = begin + 1;
        while (end < t.size() && (end >= links.size() || links[end]))
            ++end;
        std::vector<double> chain(t.begin() + begin, t.begin() + end);
        if (settings.log)
            for (double &x : chain)
                x = std::log(std::max(x, min_log_time_s));
        chain = blend_chain(std::move(chain), limit, settings.ease_layers);
        for (size_t i = 0; i < chain.size(); ++i)
            out[begin + i] = std::max(t[begin + i], settings.log ? std::exp(chain[i]) : chain[i]);
        begin = end;
    }
    return out;
}

WaitPlan plan(const std::vector<WaitSite> &sites, const Settings &settings, double print_time_s)
{
    const size_t n = sites.size();
    WaitPlan     out;
    out.dwell_s.assign(n, 0.);
    out.use_park.assign(n, false);
    out.added_s.assign(n, 0.);
    out.target_s.resize(n);
    for (size_t k = 0; k < n; ++k)
        out.target_s[k] = sites[k].interval_s;

    // A chain starts at site 0 and at every break site; neither's interval is a layer time. Within a chain
    // the j-th site's interval is the time of the chain's layer j (1-based), blended from start_layer on.
    const size_t      skip = size_t(std::max(1, settings.start_layer));
    std::vector<bool> included(n, false);
    std::vector<bool> linked(n, false); // linked to the included site before it
    for (size_t k = 0, chain_start = 0; k < n; ++k) {
        if (k == 0 || sites[k].constraint_break)
            chain_start = k;
        included[k] = k - chain_start >= skip;
        linked[k]   = included[k] && k - chain_start > skip;
    }
    std::vector<double> t;
    std::vector<bool>   links;
    std::vector<size_t> site_of;
    for (size_t k = 0; k < n; ++k)
        if (included[k]) {
            t.push_back(std::max(0., sites[k].interval_s));
            links.push_back(linked[k]);
            site_of.push_back(k);
        }
    const std::vector<double> target = target_times(t, links, settings);

    for (size_t i = 0; i < site_of.size(); ++i) {
        const size_t    k     = site_of[i];
        const WaitSite &site  = sites[k];
        double          added = target[i] - t[i];
        if (settings.wait_cap_s > 0.)
            added = std::min(added, settings.wait_cap_s);
        // Waits are written to the G-code in whole milliseconds; anything shorter is no wait.
        if (added < 0.0005)
            added = 0.;
        out.added_s[k]  = added;
        out.target_s[k] = site.interval_s + added;
        if (settings.park && site.park_trip_s > 0. && added >= std::max(settings.park_min_s, site.park_trip_s)) {
            out.use_park[k] = true;
            out.dwell_s[k]  = added - site.park_trip_s;
        } else {
            out.dwell_s[k] = added;
        }
    }

    // The run of waits around site k within its chain, and the layer that causes it: the longer of the two
    // unwaited layers that bound the run (waits ramp towards a long layer, which itself gets none).
    auto describe = [&](WaitWarning w, size_t k) {
        size_t a = k, b = k;
        while (linked[a] && out.added_s[a - 1] > 0.)
            --a;
        while (b + 1 < n && linked[b + 1] && out.added_s[b + 1] > 0.)
            ++b;
        w.first_site  = a;
        w.last_site   = b;
        w.source_site = k;
        double longest = -1.;
        if (linked[a]) {
            longest       = sites[a - 1].interval_s;
            w.source_site = a - 1;
        }
        if (b + 1 < n && linked[b + 1] && sites[b + 1].interval_s > longest)
            w.source_site = b + 1;
        return w;
    };
    // Tower blocks: keep them up to the last wait and as far above it as dropping them would break a limit.
    // Dropping the blocks from site K on takes the tower out of the layers from K: layer K ends where its
    // block would have started, and every later layer loses the block before it.
    out.tower_end = n;
    if (std::any_of(sites.begin(), sites.end(), [](const WaitSite &s) { return s.tower_block_s > 0.; })) {
        size_t last_wait = n;
        for (size_t k = 0; k < n; ++k)
            if (out.added_s[k] > 0.)
                last_wait = k;
        const double limit = settings.log ? std::log1p(std::max(0., settings.max_ratio_pct) / 100.) : std::max(0., settings.max_diff_s);
        auto within = [&](double a, double b) {
            if (settings.log)
                return std::abs(std::log(std::max(a, min_log_time_s)) - std::log(std::max(b, min_log_time_s))) <= limit + 1e-9;
            return std::abs(a - b) <= limit + 1e-9; // tolerance, s: far below a millisecond
        };
        auto without_tower = [&](size_t k, size_t cut) {
            const double t = sites[k].interval_s - sites[k].tower_pre_s;
            return k == cut ? t : t + sites[k - 1].tower_pre_s - sites[k - 1].tower_block_s;
        };
        for (size_t cut = last_wait == n ? 0 : last_wait + 1; cut < n; ++cut) {
            bool ok = true;
            for (size_t k = std::max<size_t>(cut, 1); ok && k < n; ++k)
                if (linked[k])
                    ok = within(k - 1 < cut ? out.target_s[k - 1] : without_tower(k - 1, cut), without_tower(k, cut));
            if (ok) {
                out.tower_end = cut;
                break;
            }
        }
    }

    double total   = 0.;
    size_t longest = n;
    for (size_t k = 0; k < n; ++k) {
        total += out.added_s[k];
        if (out.added_s[k] > 0. && (longest == n || out.added_s[k] > out.added_s[longest]))
            longest = k;
    }
    if (longest == n)
        return out; // no waits, nothing to warn about
    if (print_time_s > 0. && settings.warn_total_pct > 0. && total > print_time_s * settings.warn_total_pct / 100.) {
        WaitWarning w{WaitWarning::Kind::TotalShare};
        w.value = 100. * total / print_time_s;
        out.warnings.push_back(describe(w, longest));
    }
    if (settings.warn_single_s > 0. && out.added_s[longest] > settings.warn_single_s) {
        WaitWarning w{WaitWarning::Kind::SingleWait};
        w.value = out.added_s[longest];
        out.warnings.push_back(describe(w, longest));
    }
    // A guessed interval matters only where it moves a wait: a wait there, or a wait next to it.
    WaitWarning uncertain{WaitWarning::Kind::Uncertain};
    for (size_t k = 0; k < n; ++k) {
        const bool near_wait = out.added_s[k] > 0. || (k + 1 < n && linked[k + 1] && out.added_s[k + 1] > 0.) ||
                               (linked[k] && out.added_s[k - 1] > 0.);
        if (!included[k] || !sites[k].uncertain || !near_wait)
            continue;
        if (uncertain.value == 0.)
            uncertain.first_site = uncertain.source_site = k;
        uncertain.last_site = k;
        uncertain.value += 1.;
    }
    if (uncertain.value > 0.)
        out.warnings.push_back(uncertain);
    return out;
}

void set_diagnostic_sink(std::function<void(const std::string &)> sink) { diagnostic_sink() = std::move(sink); }

} // namespace LayerTimeBlend
