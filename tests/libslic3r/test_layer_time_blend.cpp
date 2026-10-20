// MAGPIE: the core's side of the gcode.time_plan slot, through the bundled layer time blending mod as it
// is built (mods/layer_time_blend): the slot is found by name, the sites cross the C boundary, the mod
// reads its parameters through the host and the plan comes back. The planner itself is tested in
// mods/layer_time_blend/unit.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libslic3r/Exception.hpp>
#include <libslic3r/ModHost/ModPackage.hpp>
#include <libslic3r/ModHost/ModSet.hpp>
#include <libslic3r/ModHost/NativeMods.hpp>
#include <libslic3r/ModHost/SlotRegistry.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <boost/filesystem.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::ModHost;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
namespace fs = boost::filesystem;

namespace {

// The official mods with native code, active for the test's lifetime.
struct BundledNativeMods
{
    std::shared_ptr<const ModSet>       previous        = active_mods();
    std::shared_ptr<const NativeModSet> previous_native = active_native_mods();
    fs::path                            temp = fs::temp_directory_path() / fs::unique_path("magpie-ltb-%%%%-%%%%");
    ModLoadResult                       result;
    BundledNativeMods()
    {
        ModLoadOptions options;
        // The official mods built in this tree (mods/CMakeLists.txt), and the bundled JSON mods.
        options.bundled_dir   = MAGPIE_OFFICIAL_MODS_DIR;
        options.installed_dir = (temp / "mods").string();
        options.cache_dir     = (temp / "cache").string();
        result                = load_mods(options);
        set_active_mods(result.mods);
        set_active_native_mods(result.native);
    }
    ~BundledNativeMods()
    {
        set_active_mods(previous);
        set_active_native_mods(previous_native);
        boost::system::error_code ec;
        fs::remove_all(temp, ec);
    }
};

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

DynamicPrintConfig config_with(const std::string &mod_settings)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"mod_settings", mod_settings}});
    return config;
}

const std::function<bool()> never_cancelled = [] { return false; };

} // namespace

TEST_CASE("The bundled layer time blending mod fills the time plan slot", "[LayerTimeBlend]")
{
    BundledNativeMods mods;
    REQUIRE(mods.result.native);
    const std::string first_error = mods.result.errors.empty() ? std::string() : mods.result.errors.front();
    INFO(first_error);
    REQUIRE(mods.result.native->find("layer_time_blend") != nullptr);
    const std::shared_ptr<const TimePlanner> planner = find_time_planner("blend");
    REQUIRE(planner);
    CHECK(planner->mod_id == "layer_time_blend");
    CHECK(find_time_planner("") == nullptr);
    CHECK(find_time_planner("no_such_planner") == nullptr);
}

TEST_CASE("The time plan slot carries the sites to the mod and its plan back", "[LayerTimeBlend]")
{
    BundledNativeMods mods;
    const std::shared_ptr<const TimePlanner> planner = find_time_planner("blend");
    REQUIRE(planner);
    // Layers 30 30 10 60 s; start_layer 2 leaves layer 1 (site 1) out. With D = 5 s the layers before the
    // 60 s one ramp 50, 55, 60: site 2 (30 s) waits 20 s, site 3 (10 s) 45 s.
    const std::vector<WaitSite> sites = sites_from({30, 30, 10, 60});
    const WaitPlan plan = planner->plan(sites, config_with(R"({"layer_time_blend.max_diff": 5, "layer_time_blend.warn_total_pct": 0, "layer_time_blend.warn_single": 0})"),
                                        never_cancelled, 1000.);
    REQUIRE(plan.added_s.size() == sites.size());
    CHECK(plan.added_s[1] == 0.);
    CHECK_THAT(plan.added_s[2], WithinAbs(20., 1e-9));
    CHECK_THAT(plan.added_s[3], WithinAbs(45., 1e-9));
    CHECK(plan.added_s[4] == 0.);
    CHECK_THAT(plan.target_s[3], WithinAbs(55., 1e-9));
    CHECK_THAT(plan.dwell_s[3], WithinAbs(45., 1e-9));
    CHECK(plan.tower_end == sites.size()); // no tower blocks
    CHECK(plan.warnings.empty());

    // The parameters are the mod's own, read through the host: D = 10 s ramps 40, 50, 60.
    const WaitPlan wider = planner->plan(sites, config_with(R"({"layer_time_blend.max_diff": 10, "layer_time_blend.warn_total_pct": 0, "layer_time_blend.warn_single": 0})"),
                                         never_cancelled, 1000.);
    CHECK_THAT(wider.added_s[2], WithinAbs(10., 1e-9));
    CHECK_THAT(wider.added_s[3], WithinAbs(40., 1e-9));
}

TEST_CASE("The mod's warnings and park choices cross the slot", "[LayerTimeBlend]")
{
    BundledNativeMods mods;
    const std::shared_ptr<const TimePlanner> planner = find_time_planner("blend");
    REQUIRE(planner);
    // Layer 4 takes 200 s among 30 s layers, D = 5 s: layer 3 (site 3) waits 200 - 5 - 30 = 165 s, more than
    // the 60 s single-wait limit. Park trips of 4 s and waits at or above 2 s park ("park", in_place_max 2).
    std::vector<WaitSite> sites = sites_from({30, 30, 30, 200, 30});
    for (WaitSite &site : sites)
        site.park_trip_s = 4.;
    const WaitPlan plan = planner->plan(sites, config_with(R"({"layer_time_blend.max_diff": 5, "layer_time_blend.wait_at": "park", "layer_time_blend.in_place_max": 2, "layer_time_blend.warn_total_pct": 0})"),
                                        never_cancelled, 1000.);
    CHECK_THAT(plan.added_s[3], WithinAbs(165., 1e-9));
    CHECK(plan.use_park[3]);
    CHECK_THAT(plan.dwell_s[3], WithinAbs(165. - 4., 1e-9));
    REQUIRE(plan.warnings.size() == 1);
    CHECK(plan.warnings[0].kind == WaitWarning::Kind::SingleWait);
    CHECK_THAT(plan.warnings[0].value, WithinAbs(165., 1e-9));
    CHECK(plan.warnings[0].source_site == 4);
}
