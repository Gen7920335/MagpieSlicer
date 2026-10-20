// MAGPIE: the batch accelerator extension point (src/libslic3r/Accel/BatchAccel.hpp). The core, not the
// accelerator mod, decides whether a result is used: without an accelerator nothing is offered, in
// Verify mode a batch that differs from the CPU reference is thrown away, and only an official mod may
// skip that check. The test accelerator (mods/test_mod/test_accel.cpp) answers wrong on request, so
// this runs without a GPU.
#include <catch2/catch_test_macros.hpp>

#include <libslic3r/Accel/BatchAccel.hpp>
#include <libslic3r/ModHost/ModPackage.hpp>
#include <libslic3r/ModHost/ModSet.hpp>
#include <libslic3r/ModHost/NativeMods.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <boost/filesystem.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::ModHost;
namespace fs = boost::filesystem;

namespace {

// The test's own statement of the facet layer range rule, not the slicer's code.
Accel::LayerRange expected_range(const std::array<float, 3> &z, const std::vector<float> &zs)
{
    const float lo = std::min({z[0], z[1], z[2]}), hi = std::max({z[0], z[1], z[2]});
    uint32_t    first = 0;
    while (first < zs.size() && zs[first] < lo)
        ++first;
    uint32_t last = first;
    while (last < zs.size() && !(hi < zs[last]))
        ++last;
    return {first, last, uint32_t(z[1] == lo ? 1 : (z[2] == lo ? 2 : 0))};
}

const std::vector<float>          zs = {0.f, 1.f, 2.f, 3.f, 4.f};
const std::vector<std::array<float, 3>> facets = {{0.5f, 1.5f, 2.5f}, {3.2f, 0.1f, 1.f}, {2.f, 2.f, 4.f}, {-1.f, 0.f, 0.f}};

// A slicing session with the given mod parameters, the loaded set active for its lifetime.
struct Sliced
{
    std::shared_ptr<const ModSet>       previous        = active_mods();
    std::shared_ptr<const NativeModSet> previous_native = active_native_mods();
    DynamicPrintConfig                  config          = DynamicPrintConfig::full_print_config();
    std::unique_ptr<Accel::Session>     session;
    Sliced(const ModLoadResult &loaded, const std::string &mod_settings)
    {
        set_active_mods(loaded.mods);
        set_active_native_mods(loaded.native);
        config.set_deserialize_strict({{"mod_settings", mod_settings}});
        session = std::make_unique<Accel::Session>(config);
    }
    ~Sliced()
    {
        session.reset();
        set_active_mods(previous);
        set_active_native_mods(previous_native);
    }
};

int cpu_calls = 0;
Accel::LayerRange cpu(size_t i)
{
    ++cpu_calls;
    return expected_range(facets[i], zs);
}

} // namespace

TEST_CASE("Without an accelerator nothing is offered to one", "[BatchAccel]")
{
    ModLoadResult none;
    none.mods   = std::make_shared<const ModSet>(ModSet::build({}));
    none.native = std::make_shared<NativeModSet>();
    Sliced s(none, "{}");
    CHECK_FALSE(Accel::active());
    CHECK_FALSE(Accel::should_dispatch(Accel::Op::FacetLayerRanges, 100000));
    std::vector<Accel::LayerRange> out;
    cpu_calls = 0;
    CHECK_FALSE(Accel::mesh_layer_ranges(facets, zs, cpu, out));
    CHECK(out.empty());
    CHECK(cpu_calls == 0);
}

#ifdef MAGPIE_TEST_ACCEL_DIR
namespace {

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("magpie-accel-%%%%-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

// The test accelerator as a bundled (official) mod.
ModLoadResult bundled_accel()
{
    ModLoadOptions options;
    options.bundled_dir = MAGPIE_TEST_ACCEL_DIR;
    ModLoadResult loaded = load_mods(options);
    REQUIRE(loaded.errors.empty());
    REQUIRE(loaded.native);
    REQUIRE(loaded.native->find("magpie_test_accel") != nullptr);
    return loaded;
}

} // namespace

TEST_CASE("An accelerator that is off is not used", "[BatchAccel]")
{
    ModLoadResult loaded = bundled_accel();
    Sliced        s(loaded, R"({"magpie_test_accel.mode": "off"})");
    CHECK_FALSE(Accel::active());
    CHECK(std::string(Accel::mode_name()) == "Off");
}

TEST_CASE("Verify mode uses a batch only when every item matches the CPU", "[BatchAccel]")
{
    ModLoadResult loaded = bundled_accel();
    std::vector<Accel::LayerRange> out;
    SECTION("right results are used, each item checked")
    {
        Sliced s(loaded, R"({"magpie_test_accel.mode": "on"})");
        REQUIRE(std::string(Accel::mode_name()) == "Verify");
        cpu_calls = 0;
        REQUIRE(Accel::mesh_layer_ranges(facets, zs, cpu, out));
        REQUIRE(out.size() == facets.size());
        for (size_t i = 0; i < facets.size(); ++i)
            CHECK(out[i] == expected_range(facets[i], zs));
        CHECK(cpu_calls == int(facets.size()));
        CHECK(Accel::rejected_batches() == 0);
        // The batch is timed on both sides: the accelerator call and the CPU reference on the same items.
        const Accel::OpTimes t = Accel::op_times(Accel::Op::FacetLayerRanges);
        CHECK(t.batches == 1);
        CHECK(t.items == facets.size());
        CHECK(t.checked_batches == 1);
        CHECK(Accel::op_times(Accel::Op::MinDistances).batches == 0);
    }
    SECTION("one wrong item throws the whole batch away")
    {
        Sliced s(loaded, R"({"magpie_test_accel.mode": "on", "magpie_test_accel.wrong": true})");
        CHECK_FALSE(Accel::mesh_layer_ranges(facets, zs, cpu, out));
        CHECK(out.empty());
        CHECK(Accel::rejected_batches() == 1);
        // A thrown-away batch is not counted as computed.
        CHECK(Accel::op_times(Accel::Op::FacetLayerRanges).batches == 0);
    }
}

TEST_CASE("Only an official accelerator skips the CPU check", "[BatchAccel]")
{
    std::vector<Accel::LayerRange> out;
    SECTION("official (bundled): max uses the results unchecked")
    {
        ModLoadResult loaded = bundled_accel();
        Sliced        s(loaded, R"({"magpie_test_accel.mode": "max", "magpie_test_accel.wrong": true})");
        REQUIRE(std::string(Accel::mode_name()) == "Trusted");
        cpu_calls = 0;
        REQUIRE(Accel::mesh_layer_ranges(facets, zs, cpu, out));
        CHECK(cpu_calls == 0);
        CHECK(Accel::op_times(Accel::Op::FacetLayerRanges).batches == 1);
        CHECK(Accel::op_times(Accel::Op::FacetLayerRanges).checked_batches == 0);
        // The wrong item came through: nothing checked it.
        CHECK_FALSE(out.back() == expected_range(facets.back(), zs));
    }
    SECTION("verified, not official: max is checked like on")
    {
        TempDir        dir;
        const fs::path installed = dir.path / "mods";
        const fs::path package   = installed / "magpie_test_accel";
        fs::create_directories(package);
        for (fs::recursive_directory_iterator it(fs::path(MAGPIE_TEST_ACCEL_DIR) / "magpie_test_accel"), end; it != end; ++it) {
            const fs::path target = package / it->path().lexically_relative(fs::path(MAGPIE_TEST_ACCEL_DIR) / "magpie_test_accel");
            if (fs::is_directory(it->status()))
                fs::create_directories(target);
            else
                fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing);
        }
        const SigningKey key = generate_signing_key();
        add_trusted_key({"test-accel-verified", KeyRole::Verified, key.public_key_hex});
        sign_package(package.string(), key.private_pem, "test-accel-verified");
        ModLoadOptions options;
        options.installed_dir = installed.string();
        options.cache_dir     = (dir.path / "cache").string();
        ModLoadResult loaded  = load_mods(options);
        remove_trusted_key("test-accel-verified");
        REQUIRE(loaded.errors.empty());
        REQUIRE(loaded.native->find("magpie_test_accel") != nullptr);
        REQUIRE_FALSE(loaded.native->find("magpie_test_accel")->manifest().official);
        {
            Sliced s(loaded, R"({"magpie_test_accel.mode": "max", "magpie_test_accel.wrong": true})");
            CHECK(std::string(Accel::mode_name()) == "Verify");
            CHECK_FALSE(Accel::mesh_layer_ranges(facets, zs, cpu, out));
            CHECK(Accel::rejected_batches() == 1);
        }
        // The cached library must be unloaded before the folder goes.
        loaded.native->shutdown();
        loaded.native.reset();
        loaded.mods.reset();
    }
}

TEST_CASE("The session ends with the print", "[BatchAccel]")
{
    ModLoadResult loaded = bundled_accel();
    {
        Sliced s(loaded, R"({"magpie_test_accel.mode": "on"})");
        CHECK(Accel::active());
    }
    CHECK_FALSE(Accel::active());
}
#endif
