#include "planner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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

// Pointwise smallest v' >= v with |v'[i] - v'[i-1]| <= limits[i] (limits[0] unused): the upper envelope of
// cones standing on every point, found by one pass each way.
void lipschitz_envelope(std::vector<double> &v, const std::vector<double> &limits)
{
    for (size_t i = 1; i < v.size(); ++i)
        v[i] = std::max(v[i], v[i - 1] - limits[i]);
    for (size_t i = v.size() - 1; i-- > 0;)
        v[i] = std::max(v[i], v[i + 1] - limits[i + 1]);
}

// The neighbour limits the exception cost allows: limits[i] for the pair (i-1, i), at least `limit`.
// They come from the solution of
//     minimise sum(v) + cost * sum(max(0, |v[i] - v[i-1]| - limit))   subject to v >= t,
// a chain whose every stage is a convex piecewise linear function of the layer's value, solved exactly by
// dynamic programming over those functions. Each pair keeps the jump that solution takes; the two-pass
// envelope under these limits is then no higher than the solution anywhere and breaks no limit by more,
// so it has the same cost, and easing still applies on top. In the chain's units (s or log s).
std::vector<double> exception_limits(const std::vector<double> &t, double limit, double cost)
{
    const size_t        n = t.size();
    std::vector<double> limits(n, limit);
    if (n < 2)
        return limits;
    // f, the least cost of layers 0..i as a function of v[i]: points (x, y) from its smallest x on, where it
    // starts (no smaller x is allowed), and the slope after the last point.
    struct Point { double x, y; };
    std::vector<Point> f{ { t[0], t[0] } };
    double             tail = 1.;
    // Per stage, what backtracking needs of the stage before: its smallest and largest minimiser, the point
    // past which its slope exceeds -cost, and the one from which it reaches cost (infinite: never).
    struct Stage { double min_lo, min_hi, rise_from, steep_from; };
    std::vector<Stage> stages;
    stages.reserve(n - 1);
    std::vector<double> slope; // slope right of each point of f
    std::vector<Point>  h;
    for (size_t i = 1; i < n; ++i) {
        const size_t k = f.size();
        slope.resize(k);
        for (size_t j = 0; j + 1 < k; ++j)
            slope[j] = (f[j + 1].y - f[j].y) / (f[j + 1].x - f[j].x);
        slope[k - 1] = tail;
        size_t lo = 0;
        while (lo < k && slope[lo] < 0.)
            ++lo;
        lo = std::min(lo, k - 1);
        size_t hi = lo;
        while (hi + 1 < k && slope[hi] == 0.)
            ++hi;
        size_t rise = 0;
        while (rise < lo && slope[rise] <= -cost)
            ++rise;
        size_t steep = hi;
        while (steep < k && slope[steep] < cost)
            ++steep;
        stages.push_back({ f[lo].x, f[hi].x, f[rise].x, steep < k ? f[steep].x : std::numeric_limits<double>::infinity() });
        // h(x) = min over y of f(y) + cost * max(0, |x - y| - limit): the part of f left of its minimum moves
        // left by `limit`, the part right of it moves right, and slopes beyond +-cost are cut to +-cost.
        h.clear();
        for (size_t j = rise; j <= lo; ++j)
            h.push_back({ f[j].x - limit, f[j].y });
        for (size_t j = hi; j <= std::min(steep, k - 1); ++j)
            h.push_back({ f[j].x + limit, f[j].y });
        const double h_tail = steep < k ? cost : tail;
        // From layer i's own time on, then + v[i].
        const double from = t[i];
        f.clear();
        if (from <= h.front().x) {
            f.push_back({ from, h.front().y - cost * (from - h.front().x) });
            for (const Point &p : h)
                if (p.x > from)
                    f.push_back(p);
        } else {
            size_t j = 0;
            while (j + 1 < h.size() && h[j + 1].x <= from)
                ++j;
            const double s = j + 1 < h.size() ? (h[j + 1].y - h[j].y) / (h[j + 1].x - h[j].x) : h_tail;
            f.push_back({ from, h[j].y + s * (from - h[j].x) });
            for (size_t m = j + 1; m < h.size(); ++m)
                if (h[m].x > from)
                    f.push_back(h[m]);
        }
        // Points closer than this are one, in the chain's units: far below a millisecond.
        constexpr double same_x = 1e-12;
        size_t           kept   = 0;
        for (size_t j = 0; j < f.size(); ++j) {
            if (kept > 0 && f[j].x - f[kept - 1].x < same_x) {
                f[kept - 1].y = std::min(f[kept - 1].y, f[j].y);
                continue;
            }
            f[kept++] = f[j];
        }
        f.resize(kept);
        for (Point &p : f)
            p.y += p.x;
        tail = h_tail + 1.;
    }
    // The smallest minimiser of the last stage, then back through the stages.
    std::vector<double> v(n);
    {
        const size_t k = f.size();
        size_t       lo = 0;
        while (lo + 1 < k && (f[lo + 1].y - f[lo].y) / (f[lo + 1].x - f[lo].x) < 0.)
            ++lo;
        v[n - 1] = f[lo].x;
    }
    for (size_t i = n - 1; i > 0; --i) {
        const Stage &s = stages[i - 1];
        const double x = v[i];
        if (x < s.min_lo - limit)
            v[i - 1] = std::max(x + limit, s.rise_from);
        else if (x > s.min_hi + limit)
            v[i - 1] = std::min(x - limit, s.steep_from);
        else
            v[i - 1] = std::max(s.min_lo, x - limit);
    }
    for (size_t i = 1; i < n; ++i)
        limits[i] = std::max(limit, std::abs(v[i] - v[i - 1]));
    return limits;
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
// `limits[i]` is the limit between i-1 and i (at least `limit`, more where an exception is allowed).
std::vector<double> blend_chain(std::vector<double> v, double limit, const std::vector<double> &limits, int ease_layers)
{
    if (v.size() < 2 || !(limit >= 0.))
        return v;
    if (ease_layers <= 0) {
        lipschitz_envelope(v, limits);
        return v;
    }
    // Convex bends (ramp starts and ends) take at most `curvature` of second difference, or the layer's own
    // bend in the input where that is larger: the limit rounds the bends blending makes, not the ones the
    // layer times already have (an alternating 30/27 s pair of infill layers would otherwise wait on every
    // other layer, 2026-10-27). With q the sequence whose second difference at i is that bound, v - q must
    // be concave. Each bound is fixed per layer, so both constraint sets are closed under the pointwise
    // minimum and the smallest solution exists; alternating the two projections, each of which only raises
    // values, reaches it.
    const double        curvature = limit / double(ease_layers + 1);
    const size_t        n         = v.size();
    std::vector<double> q(n, 0.);
    for (size_t j = 1; j + 1 < n; ++j) {
        // Bound on the second difference at j, in the chain's units (s or log s).
        const double bound = std::max(curvature, v[j + 1] - 2. * v[j] + v[j - 1]);
        q[j + 1]           = 2. * q[j] - q[j - 1] + bound;
    }
    // Progress: every round raises v (both steps are increasing and never lower a value), and v is
    // bounded by the envelope of max(t). The 2026-10-19 check against an LP found 2 rounds in every
    // case; the cap only guards a bug.
    constexpr int max_rounds = 64;
    for (int round = 0; round < max_rounds; ++round) {
        lipschitz_envelope(v, limits);
        std::vector<double> w(n);
        for (size_t i = 0; i < n; ++i)
            w[i] = v[i] - q[i];
        concave_majorant(w);
        bool changed = false;
        for (size_t i = 0; i < n; ++i) {
            const double raised = w[i] + q[i];
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
    lipschitz_envelope(v, limits);
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
        const std::vector<double> limits = settings.exception_cost > 0. ? exception_limits(chain, limit, settings.exception_cost) :
                                                                          std::vector<double>(chain.size(), limit);
        chain = blend_chain(std::move(chain), limit, limits, settings.ease_layers);
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
