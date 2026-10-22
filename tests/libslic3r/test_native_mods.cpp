// MAGPIE: native mods: the loader, the C ABI checks, signatures, the crash guard, the host table and
// what applies at the next start. The test mods are built from mods/test_mod (mods/CMakeLists.txt).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/ExPolygon.hpp>
#include <libslic3r/ModHost/ModIndex.hpp>
#include <libslic3r/ModHost/ModPackage.hpp>
#include <libslic3r/ModHost/ModSet.hpp>
#include <libslic3r/ModHost/ModTest.hpp>
#include <libslic3r/ModHost/NativeMods.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/libslic3r.h>

#include "../../mods/test_mod/test_slot.h"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <miniz.h>

#include <algorithm>
#include <sstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::ModHost;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = boost::filesystem;

namespace {

const fs::path test_mods_dir() { return fs::path(MAGPIE_TEST_MODS_DIR); }

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("magpie-native-%%%%-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

void copy_dir(const fs::path &from, const fs::path &to)
{
    fs::create_directories(to);
    for (fs::recursive_directory_iterator it(from), end; it != end; ++it) {
        const fs::path target = to / it->path().lexically_relative(from);
        if (fs::is_directory(it->status()))
            fs::create_directories(target);
        else
            fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing);
    }
}

void write_text(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    boost::nowide::ofstream out(path.string(), std::ios::binary);
    out << text;
}

bool any_contains(const std::vector<std::string> &lines, const std::string &text)
{
    return std::any_of(lines.begin(), lines.end(), [&text](const std::string &l) { return l.find(text) != std::string::npos; });
}

std::string joined(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &l : lines)
        out += l + "\n";
    return out;
}

// The bundled test mods loaded with native code; nothing installed.
ModLoadResult load_bundled_test_mods(const TempDir &dir)
{
    ModLoadOptions options;
    options.bundled_dir   = test_mods_dir().string();
    options.installed_dir = (dir.path / "mods").string();
    options.cache_dir     = (dir.path / "cache").string();
    return load_mods(options);
}

const test_math_v1 &math_table(const NativeEntry &entry)
{
    REQUIRE(entry.table_size >= sizeof(test_math_v1));
    return *static_cast<const test_math_v1 *>(entry.table);
}

// Makes a set of mods the active one for the test's lifetime.
struct ActiveMods
{
    std::shared_ptr<const ModSet>       previous        = active_mods();
    std::shared_ptr<const NativeModSet> previous_native = active_native_mods();
    ActiveMods(std::shared_ptr<const ModSet> mods, std::shared_ptr<const NativeModSet> native)
    {
        set_active_mods(std::move(mods));
        set_active_native_mods(std::move(native));
    }
    ~ActiveMods()
    {
        set_active_mods(previous);
        set_active_native_mods(previous_native);
    }
};

// A trusted key for the test's lifetime.
struct TestKey
{
    SigningKey  key = generate_signing_key();
    std::string id;
    TestKey(const std::string &id, KeyRole role) : id(id) { add_trusted_key({id, role, key.public_key_hex}); }
    ~TestKey() { remove_trusted_key(id); }
};

} // namespace

TEST_CASE("The bundled test mod loads and fills its slot; builds for another API or a fast float mode are refused", "[NativeMods]")
{
    TempDir             dir;
    const ModLoadResult result = load_bundled_test_mods(dir);
    REQUIRE(result.native);
    INFO(joined(result.errors));

    const NativeMod *mod = result.native->find("magpie_test_mod");
    REQUIRE(mod != nullptr);
    CHECK(mod->query() == "magpie_test_mod 1.0.0 1 1 fp=precise");
    CHECK(result.native->find_slot("test.math", "test", 1) != nullptr);
    CHECK(result.native->find_slot("test.math", "test", 2) == nullptr); // no newer interface version
    CHECK(result.native->find_service("magpie_test_mod.echo", 1) != nullptr);
    CHECK(result.mods->find_mod("magpie_test_mod") != nullptr);
#ifdef _WIN32
    // A bundled library loads where it is, so it is locked while loaded: the counterpart of the cached
    // copy an installed mod runs from (next test case).
    CHECK(mod->loaded_path() == native_library_path(*result.mods->find_mod("magpie_test_mod")));
    CHECK_FALSE(boost::nowide::ofstream(mod->loaded_path(), std::ios::binary | std::ios::app).good());
#endif

    // The two refused builds are left out entirely, manifest and parameters included.
    CHECK(result.native->find("magpie_test_mod_api2") == nullptr);
    CHECK(result.mods->find_mod("magpie_test_mod_api2") == nullptr);
    CHECK(any_contains(result.errors, "magpie_test_mod_api2: it was built for mod API 2"));
    CHECK(result.mods->find_mod("magpie_test_mod_fast_fp") == nullptr);
    CHECK(any_contains(result.errors, "magpie_test_mod_fast_fp: it was compiled with floating-point mode \"fast\""));
}

TEST_CASE("A slot call runs in the mod, a C++ exception comes back as a status", "[NativeMods]")
{
    TempDir             dir;
    const ModLoadResult result = load_bundled_test_mods(dir);
    REQUIRE(result.native);
    const NativeEntry *entry = result.native->find_slot("test.math", "test", 1);
    REQUIRE(entry != nullptr);
    const test_math_v1 &math = math_table(*entry);

    int64_t sum = 0;
    CHECK(entry->mod->guarded_call([&] { return math.add(entry->user, 2, 3, &sum).code; }) == MP_OK);
    CHECK(sum == 5);

    mp_status thrown{};
    CHECK(entry->mod->guarded_call([&] { thrown = math.throws(entry->user); return thrown.code; }) == MP_ERR_FAILED);
    CHECK_THAT(std::string(thrown.message), ContainsSubstring("thrown inside the test mod"));
    CHECK_FALSE(entry->mod->crashed());
}

#ifdef _WIN32
TEST_CASE("A crash in a mod is caught, reported with the mod's id, and disables the mod for the run", "[NativeMods]")
{
    TempDir             dir;
    const ModLoadResult result = load_bundled_test_mods(dir);
    REQUIRE(result.native);
    const NativeEntry *entry = result.native->find_slot("test.math", "test", 1);
    REQUIRE(entry != nullptr);
    const test_math_v1 &math = math_table(*entry);

    std::string message;
    CHECK(entry->mod->guarded_call([&] { return math.crash(entry->user).code; }, &message) == MP_ERR_FAILED);
    CHECK(entry->mod->crashed());
    CHECK_THAT(message, ContainsSubstring("mod 'magpie_test_mod' 1.0.0 crashed: access violation"));

    // Not called again in this run, not even the harmless function.
    int64_t sum = 0;
    CHECK(entry->mod->guarded_call([&] { return math.add(entry->user, 2, 3, &sum).code; }, &message) == MP_ERR_FAILED);
    CHECK(sum == 0);
    CHECK_THAT(message, ContainsSubstring("disabled for this run"));
}
#endif

TEST_CASE("A mod reads its parameter from the region, then the object, then the process", "[NativeMods]")
{
    TempDir             dir;
    const ModLoadResult result = load_bundled_test_mods(dir);
    REQUIRE(result.native);
    ActiveMods                active(result.mods, result.native);
    const NativeEntry        *entry = result.native->find_slot("test.math", "test", 1);
    REQUIRE(entry != nullptr);
    const test_math_v1 &math = math_table(*entry);
    auto read_gap = [&](mp_ctx &ctx) {
        double gap = -1.;
        REQUIRE(entry->mod->guarded_call([&] { return math.param(entry->user, &ctx, "gap", &gap).code; }) == MP_OK);
        return gap;
    };

    DynamicPrintConfig print = DynamicPrintConfig::full_print_config();
    PrintObjectConfig  object;
    PrintRegionConfig  region;
    mp_ctx             ctx;
    ctx.mod          = entry->mod;
    ctx.print_config = &print;
    // The manifest's default, 1.5 mm.
    CHECK_THAT(read_gap(ctx), WithinRel(1.5, 1e-12));
    print.set_key_value("mod_settings", new ConfigOptionString(R"({"magpie_test_mod.gap": 2.0})"));
    CHECK_THAT(read_gap(ctx), WithinRel(2.0, 1e-12));
    object.object_mod_settings.value = R"({"magpie_test_mod.gap": 2.5})";
    ctx.object_config                = &object;
    CHECK_THAT(read_gap(ctx), WithinRel(2.5, 1e-12));
    region.region_mod_settings.value = R"({"magpie_test_mod.gap": 3.5})";
    ctx.region_config                = &region;
    CHECK_THAT(read_gap(ctx), WithinRel(3.5, 1e-12));
    // A region that sets nothing falls through to its object.
    region.region_mod_settings.value = R"({"other_mod.x": 1})";
    CHECK_THAT(read_gap(ctx), WithinRel(2.5, 1e-12));
}

TEST_CASE("A mod converts millimetres with the coordinate unit the host gives at the time", "[NativeMods]")
{
    TempDir             dir;
    const ModLoadResult result = load_bundled_test_mods(dir);
    REQUIRE(result.native);
    const NativeEntry *entry = result.native->find_slot("test.math", "test", 1);
    REQUIRE(entry != nullptr);
    const test_math_v1 &math = math_table(*entry);

    const double saved = SCALING_FACTOR;
    // A 10 mm square grown by 1 mm with mitred corners is 12 mm square: 144 mm2, whatever the unit.
    for (const double unit_mm : {SCALING_FACTOR_INTERNAL, SCALING_FACTOR_INTERNAL_LARGE_PRINTER}) {
        SCALING_FACTOR = unit_mm;
        native_host_api(); // the core refreshes the table before slicing
        const coord_t   side = coord_t(10. / unit_mm);
        const mp_point  square[4] = {{0, 0}, {side, 0}, {side, side}, {0, side}};
        const mp_ring   ring{square, 4};
        const mp_expoly poly{&ring, 1};
        const mp_expolys in{&poly, 1};
        mp_expolys       out{};
        REQUIRE(entry->mod->guarded_call([&] { return math.grow(entry->user, &in, 1.0, &out).code; }) == MP_OK);
        REQUIRE(out.n == 1);
        REQUIRE(out.items[0].n == 1);
        Polygon grown;
        for (uint32_t i = 0; i < out.items[0].rings[0].n; ++i)
            grown.points.emplace_back(out.items[0].rings[0].pts[i].x, out.items[0].rings[0].pts[i].y);
        native_host_api().free_expolys(&out);
        // Offset vertices round to whole units: up to about perimeter x unit = 48 mm x 1e-5 mm off.
        CHECK_THAT(std::abs(grown.area()) * unit_mm * unit_mm, WithinAbs(144., 1e-3));
    }
    SCALING_FACTOR = saved;
    native_host_api();
}

TEST_CASE("Installed native code loads with a trusted signature, or unverified once the user accepted it, from a cached copy", "[NativeMods]")
{
    TempDir        dir;
    const fs::path installed = dir.path / "mods";
    const fs::path package   = installed / "magpie_test_mod";
    copy_dir(test_mods_dir() / "magpie_test_mod", package);
    ModLoadOptions options;
    options.installed_dir = installed.string();
    options.cache_dir     = (dir.path / "cache").string();
    // The questions asked, and the answer given.
    std::vector<UnverifiedNativeMod> asked;
    bool                             answer = false;
    auto ask = [&](const UnverifiedNativeMod &mod) {
        asked.push_back(mod);
        return answer;
    };

    SECTION("unsigned, without anyone to ask (the command line)") {
        const ModLoadResult result = load_mods(options);
        CHECK(result.native->find("magpie_test_mod") == nullptr);
        CHECK(any_contains(result.errors, "magpie_test_mod: unverified native code not accepted yet"));
    }
    SECTION("unsigned, declined") {
        options.consent = ask;
        const ModLoadResult result = load_mods(options);
        CHECK(result.native->find("magpie_test_mod") == nullptr);
        CHECK(any_contains(result.notes, "magpie_test_mod: unverified, not loaded this time"));
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].id == "magpie_test_mod");
        CHECK(asked[0].version == "1.0.0");
        CHECK(asked[0].fingerprint.size() == 64);
        CHECK(any_contains({asked[0].reason}, "no SIGNATURE"));
        // Declining is not remembered: the next start asks again.
        load_mods(options);
        CHECK(asked.size() == 2);
        CHECK_FALSE(unverified_mod_accepted(options.installed_dir, asked[0]));
    }
    SECTION("unsigned, accepted once, then loaded without asking until its library changes") {
        options.consent = ask;
        answer          = true;
        {
            const ModLoadResult result = load_mods(options);
            INFO(joined(result.errors));
            const NativeMod *mod = result.native->find("magpie_test_mod");
            REQUIRE(mod != nullptr);
            const Mod *manifest = result.mods->find_mod("magpie_test_mod");
            REQUIRE(manifest != nullptr);
            CHECK_FALSE(manifest->verified);
            CHECK_FALSE(manifest->official);
            // From the cache, as a signed mod.
            CHECK(fs::path(mod->loaded_path()).lexically_relative(options.cache_dir).string().rfind("..", 0) != 0);
        }
        REQUIRE(asked.size() == 1);
        CHECK(unverified_mod_accepted(options.installed_dir, asked[0]));
        // Accepted: the command line loads it too.
        options.consent = nullptr;
        {
            const ModLoadResult result = load_mods(options);
            INFO(joined(result.errors));
            CHECK(result.native->find("magpie_test_mod") != nullptr);
        }
        // A file added to the library folder is code the user did not accept: asked again.
        write_text(package / "bin" / native_platform() / "extra.dll", "x");
        options.consent = ask;
        answer          = false;
        const ModLoadResult result = load_mods(options);
        CHECK(result.native->find("magpie_test_mod") == nullptr);
        CHECK(asked.size() == 2);
        CHECK(asked[1].fingerprint != asked[0].fingerprint);
    }
    SECTION("signed by a key this application does not trust: unverified") {
        sign_package(package.string(), generate_signing_key().private_pem, "unknown-key");
        options.consent = ask;
        const ModLoadResult result = load_mods(options);
        CHECK(result.native->find("magpie_test_mod") == nullptr);
        REQUIRE(asked.size() == 1);
        CHECK(any_contains({asked[0].reason}, "does not trust"));
    }
    SECTION("signed by a trusted key") {
        TestKey key("test-verified", KeyRole::Verified);
        sign_package(package.string(), key.key.private_pem, key.id);
        const ModLoadResult result = load_mods(options);
        INFO(joined(result.errors));
        const NativeMod *mod = result.native->find("magpie_test_mod");
        REQUIRE(mod != nullptr);
        const Mod *manifest = result.mods->find_mod("magpie_test_mod");
        REQUIRE(manifest != nullptr);
        CHECK(manifest->verified);
        CHECK_FALSE(manifest->official); // a verified key is not an official one
        // The library runs from the cache, so the installed file can be replaced while the application runs.
        CHECK(fs::path(mod->loaded_path()).lexically_relative(options.cache_dir).string().rfind("..", 0) != 0);
        // Windows refuses to open a loaded library for writing (checked on the bundled one above); the
        // installed one stays writable.
        boost::nowide::ofstream original(native_library_path(*manifest), std::ios::binary | std::ios::app);
        CHECK(original.good());
    }
    SECTION("changed after signing") {
        TestKey key("test-official", KeyRole::Official);
        sign_package(package.string(), key.key.private_pem, key.id);
        boost::nowide::ofstream(( package / "mod.json").string(), std::ios::app) << "\n";
        const ModLoadResult result = load_mods(options);
        CHECK(result.mods->find_mod("magpie_test_mod") == nullptr);
        CHECK(any_contains(result.errors, "the files differ from what \"test-official\" signed"));
    }
    SECTION("a damaged cached copy is copied again") {
        TestKey key("test-verified", KeyRole::Verified);
        sign_package(package.string(), key.key.private_pem, key.id);
        std::string cached;
        {
            const ModLoadResult result = load_mods(options);
            INFO(joined(result.errors));
            REQUIRE(result.native->find("magpie_test_mod") != nullptr);
            cached = result.native->find("magpie_test_mod")->loaded_path();
        }
        // Cut short as a crash during the copy leaves it. Before, a cached file was never checked again, so the
        // mod failed to load on every start.
        const auto size = fs::file_size(cached);
        fs::resize_file(cached, 1000);
        const ModLoadResult result = load_mods(options);
        INFO(joined(result.errors));
        REQUIRE(result.native->find("magpie_test_mod") != nullptr);
        CHECK(fs::file_size(cached) == size);
    }
}

TEST_CASE("A bundled package loads unsigned, and with a signature only when it verifies", "[NativeMods]")
{
    // The installer signs the official mods it bundles; a file changed in the installation afterwards is caught.
    TempDir        dir;
    const fs::path bundled = dir.path / "bundled";
    const fs::path package = bundled / "magpie_test_mod";
    copy_dir(test_mods_dir() / "magpie_test_mod", package);
    ModLoadOptions options;
    options.bundled_dir   = bundled.string();
    options.installed_dir = (dir.path / "mods").string();
    options.cache_dir     = (dir.path / "cache").string();

    SECTION("unsigned") {
        const ModLoadResult result = load_mods(options);
        INFO(joined(result.errors));
        CHECK(result.native->find("magpie_test_mod") != nullptr);
    }
    SECTION("signed") {
        TestKey key("test-official", KeyRole::Official);
        sign_package(package.string(), key.key.private_pem, key.id);
        const ModLoadResult result = load_mods(options);
        INFO(joined(result.errors));
        CHECK(result.native->find("magpie_test_mod") != nullptr);
    }
    SECTION("changed after signing") {
        TestKey key("test-official", KeyRole::Official);
        sign_package(package.string(), key.key.private_pem, key.id);
        boost::nowide::ofstream((package / "mod.json").string(), std::ios::app) << "\n";
        const ModLoadResult result = load_mods(options);
        CHECK(result.mods->find_mod("magpie_test_mod") == nullptr);
        CHECK(any_contains(result.errors, "its signature does not verify"));
    }
}

TEST_CASE("The application trusts Magpie's official key and the verified key Gen signs reviewed mods with", "[NativeMods]")
{
    const std::vector<TrustedKey> keys = trusted_keys();
    auto key = [&keys](const std::string &id) {
        auto it = std::find_if(keys.begin(), keys.end(), [&id](const TrustedKey &k) { return k.id == id; });
        return it == keys.end() ? nullptr : &*it;
    };
    const TrustedKey *official = key("magpie-official-2026");
    const TrustedKey *verified = key("magpie-verified-2026");
    REQUIRE(official != nullptr);
    REQUIRE(verified != nullptr);
    CHECK(official->role == KeyRole::Official);
    // A verified signature gives the verified tag, not an official mod.
    CHECK(verified->role == KeyRole::Verified);
    CHECK(verified->public_key_hex.size() == 64);
    CHECK(verified->public_key_hex != official->public_key_hex);
}

TEST_CASE("A signature covers every file of the package", "[NativeMods]")
{
    TempDir dir;
    write_text(dir.path / "mod.json", R"({"id": "x", "version": "1"})");
    write_text(dir.path / "i18n" / "ko.json", "{}");
    TestKey key("test-files", KeyRole::Official);
    sign_package(dir.path.string(), key.key.private_pem, key.id);
    const SignatureCheck good = check_package_signature(dir.path.string());
    CHECK(good.valid);
    CHECK(good.role == KeyRole::Official);
    CHECK(good.hashes.count("i18n/ko.json") == 1);
    CHECK(good.hashes.count("SIGNATURE") == 0);

    SECTION("an added file") {
        write_text(dir.path / "bin" / "extra.dll", "x");
        CHECK_FALSE(check_package_signature(dir.path.string()).valid);
    }
    SECTION("a changed file") {
        write_text(dir.path / "i18n" / "ko.json", "{ }");
        CHECK_FALSE(check_package_signature(dir.path.string()).valid);
    }
}

TEST_CASE("Turning a mod off, removing it and updating it apply at the next start", "[NativeMods]")
{
    TempDir        dir;
    const fs::path installed = dir.path / "mods";
    write_text(installed / "aaa.json", R"({"id": "aaa", "version": "1.0"})");
    write_text(installed / "bbb" / "mod.json", R"({"id": "bbb", "version": "1.0"})");
    ModLoadOptions options;
    options.installed_dir = installed.string();
    options.load_native   = false;
    // The version of a loaded mod, "" when it does not load. (A Mod pointer would outlive its set.)
    auto loaded = [&](const std::string &id) {
        const ModLoadResult r = load_mods(options);
        const Mod *mod = r.mods->find_mod(id);
        return mod ? mod->version : std::string();
    };
    REQUIRE(!loaded("aaa").empty());

    set_mod_enabled(installed.string(), "aaa", false);
    CHECK(loaded("aaa").empty());
    CHECK(any_contains(load_mods(options).notes, "aaa: turned off"));
    set_mod_enabled(installed.string(), "aaa", true);
    CHECK(!loaded("aaa").empty());

    schedule_mod_removal(installed.string(), "aaa");
    CHECK(!loaded("aaa").empty()); // still there until the scheduled changes are applied
    CHECK(any_contains(apply_pending_mod_operations(installed.string()), "removed mod aaa"));
    CHECK_FALSE(fs::exists(installed / "aaa.json"));

    write_text(installed / "bbb.pending" / "mod.json", R"({"id": "bbb", "version": "2.0"})");
    CHECK(mod_update_pending(installed.string(), "bbb"));
    CHECK(any_contains(apply_pending_mod_operations(installed.string()), "installed mod bbb 2.0"));
        CHECK(loaded("bbb") == "2.0");
    CHECK_FALSE(fs::exists(installed / "bbb.pending"));

    // A broken update is dropped and the installed version kept.
    write_text(installed / "bbb.pending" / "mod.json", R"({"id": "ccc", "version": "3.0"})");
    CHECK(any_contains(apply_pending_mod_operations(installed.string()), "keeping the installed version"));
    CHECK(loaded("bbb") == "2.0");
    CHECK_FALSE(fs::exists(installed / "bbb.pending"));
}

TEST_CASE("Mods load after the mods they depend on, and not without them", "[NativeMods]")
{
    TempDir        dir;
    const fs::path installed = dir.path / "mods";
    // "aaa" sorts first but needs "zzz".
    write_text(installed / "aaa.json", R"({"id": "aaa", "version": "1.0", "depends": [ {"id": "zzz", "min": "1.2"} ]})");
    write_text(installed / "zzz.json", R"({"id": "zzz", "version": "1.10"})");
    write_text(installed / "old.json", R"({"id": "old", "version": "1.0", "depends": [ {"id": "zzz", "min": "2"} ]})");
    write_text(installed / "core.json", R"({"id": "needs_api", "version": "1.0", "depends": [ {"id": "core", "min": "9.0"} ]})");
    ModLoadOptions options;
    options.installed_dir = installed.string();
    options.load_native   = false;
    const ModLoadResult result = load_mods(options);
    REQUIRE(result.mods->mods().size() == 2);
    CHECK(result.mods->mods()[0].id == "zzz"); // 1.10 is newer than 1.2
    CHECK(result.mods->mods()[1].id == "aaa");
    CHECK(any_contains(result.errors, "old: needs zzz 2"));
    CHECK(any_contains(result.errors, "needs_api: needs core 9.0"));
}

TEST_CASE("Installed JSON mods may write printer G-code in every tier", "[NativeMods]")
{
    TempDir        dir;
    const fs::path installed = dir.path / "mods";
    const char    *mod_json  = R"({"id": "start_tweak", "version": "1.0",
        "rules": [ { "target": "machine_start_gcode", "value": "\"G28\"" } ] })";
    ModLoadOptions options;
    options.installed_dir = installed.string();
    options.load_native   = false;

    SECTION("a single file (unverified)") {
        write_text(installed / "start_tweak.json", mod_json);
        const ModLoadResult result = load_mods(options);
        INFO(joined(result.errors));
        const Mod *mod = result.mods->find_mod("start_tweak");
        REQUIRE(mod != nullptr);
        CHECK_FALSE(mod->verified);
    }
    SECTION("a package signed by a verified key") {
        write_text(installed / "start_tweak" / "mod.json", mod_json);
        TestKey key("test-verified-gcode", KeyRole::Verified);
        sign_package((installed / "start_tweak").string(), key.key.private_pem, key.id);
        CHECK(load_mods(options).mods->find_mod("start_tweak") != nullptr);
    }
    SECTION("a package signed by an official key") {
        write_text(installed / "start_tweak" / "mod.json", mod_json);
        TestKey key("test-official-gcode", KeyRole::Official);
        sign_package((installed / "start_tweak").string(), key.key.private_pem, key.id);
        CHECK(load_mods(options).mods->find_mod("start_tweak") != nullptr);
    }
}

TEST_CASE("A downloaded package is checked before it waits for the next start", "[NativeMods]")
{
    TempDir dir;
    auto zip_of = [](const std::vector<std::pair<std::string, std::string>> &files) {
        mz_zip_archive zip;
        mz_zip_zero_struct(&zip);
        REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
        for (const auto &[name, data] : files)
            REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION));
        void  *buffer = nullptr;
        size_t size   = 0;
        REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size));
        std::string bytes(static_cast<const char *>(buffer), size);
        mz_zip_writer_end(&zip);
        return bytes;
    };
    ModIndexEntry entry;
    entry.id = "zipped";
    const std::string mods = (dir.path / "mods").string();

    const std::string good = zip_of({{"mod.json", R"({"id": "zipped", "version": "1.0"})"}});
    entry.sha256           = sha256_hex(good);
    CHECK(fs::is_regular_file(fs::path(stage_mod_package(entry, good, mods)) / "mod.json"));
    CHECK(mod_update_pending(mods, "zipped"));

    entry.sha256 = std::string(64, '0');
    CHECK_THROWS_WITH(stage_mod_package(entry, good, mods), ContainsSubstring("does not match the index"));

    const std::string escaping = zip_of({{"mod.json", R"({"id": "zipped", "version": "1.0"})"}, {"../evil.txt", "x"}});
    entry.sha256               = sha256_hex(escaping);
    CHECK_THROWS_WITH(stage_mod_package(entry, escaping, mods), ContainsSubstring("outside the package"));
    CHECK_FALSE(fs::exists(dir.path / "evil.txt"));

    const std::string native = zip_of({{"mod.json", R"({"id": "zipped", "version": "1.0", "api": {"major": 1}, "binary": "zipped"})"}});
    entry.sha256             = sha256_hex(native);
    CHECK_THROWS_WITH(stage_mod_package(entry, native, mods), ContainsSubstring("not signed with a key this application trusts"));
}

TEST_CASE("The G-code check finds moves off the bed, impossible temperatures, non-finite numbers and runaway extrusion", "[NativeMods]")
{
    GCodeBounds bounds;
    bounds.max_x_mm = bounds.max_y_mm = bounds.max_z_mm = 200.;
    CHECK(check_gcode("G28\nM104 S210\nM83\nG1 X10 Y10 Z0.2 E0.5 F1200\nSET_PRINT_STATS_INFO CURRENT_LAYER=1\n", bounds).empty());
    const std::vector<std::string> problems = check_gcode(
        "G1 X201.5 Y10\n"   // 1.5 mm past the edge, more than the 1 mm slack
        "G1 X200.5 Y-0.5\n" // within the slack
        "M109 S400\n"
        "G1 X10 Y nan\n"
        "M83\nG1 X20 E50.5\n",
        bounds);
    REQUIRE(problems.size() == 4);
    CHECK_THAT(problems[0], ContainsSubstring("1: X off the bed"));
    CHECK_THAT(problems[1], ContainsSubstring("3: temperature out of range"));
    CHECK_THAT(problems[2], ContainsSubstring("4: non-finite number"));
    CHECK_THAT(problems[3], ContainsSubstring("6: E move too long"));

    const GCodeBounds read = bounds_from_gcode("; HEADER\n; printable_area = 0x0,270x0,270x270,0x270\n; printable_height = 250\n");
    CHECK_THAT(read.max_x_mm, WithinRel(270., 1e-12));
    CHECK_THAT(read.max_z_mm, WithinRel(250., 1e-12));
}

TEST_CASE("The mod test slices each case twice and checks the G-code", "[NativeMods]")
{
    TempDir             dir;
    const ModLoadResult loaded = load_bundled_test_mods(dir);
    REQUIRE(loaded.native);
    // A copy of the test mod with a tests folder, loaded as an official package.
    const fs::path bundled = dir.path / "bundled";
    copy_dir(test_mods_dir() / "magpie_test_mod", bundled / "magpie_test_mod");
    write_text(bundled / "magpie_test_mod" / "tests" / "cube.json", R"({"args": ["cube.stl"], "contains": ["G1 X10"], "not_contains": ["M600"]})");
    write_text(bundled / "magpie_test_mod" / "tests" / "cube.stl", "solid\nendsolid\n");
    ModLoadOptions options;
    options.bundled_dir = bundled.string();
    options.cache_dir   = (dir.path / "cache").string();
    options.load_native = false;
    options.loaded_native = loaded.native;
    ActiveMods active(load_mods(options).mods, loaded.native);

    const std::string header = "; generated by Magpie on 2026-10-20\n; printable_area = 0x0,200x0,200x200,0x200\n; printable_height = 200\n";
    std::vector<std::string> args;
    SECTION("deterministic and clean") {
        const ModTestReport report = run_mod_test("magpie_test_mod", [&](const ModTestCase &c, int run) {
            args = c.args;
            // The producer line differs between runs and is not compared.
            return "; generated by Magpie on run " + std::to_string(run) + "\n" + header + "G1 X10 Y10\n";
        });
        INFO(joined(report.lines));
        CHECK(report.passed);
        REQUIRE(args.size() == 1);
        CHECK(fs::path(args[0]).filename() == "cube.stl");
        CHECK(fs::path(args[0]).is_absolute());
    }
    SECTION("two slices differ") {
        const ModTestReport report = run_mod_test("magpie_test_mod",
                                                  [&](const ModTestCase &, int run) { return header + "G1 X" + std::to_string(run) + "0 Y10\n"; });
        CHECK_FALSE(report.passed);
        CHECK(any_contains(report.lines, "not deterministic"));
    }
    SECTION("an expected text is missing and an unwanted one is there") {
        const ModTestReport report = run_mod_test("magpie_test_mod", [&](const ModTestCase &, int) { return header + "M600\n"; });
        CHECK_FALSE(report.passed);
        CHECK(any_contains(report.lines, "lacks \"G1 X10\""));
        CHECK(any_contains(report.lines, "has \"M600\""));
    }
}

TEST_CASE("Test case arguments naming files of the tests folder are made absolute, lists too", "[NativeMods]")
{
    TempDir dir;
    // Presets in a subfolder: every .json directly in tests/ is a test case.
    write_text(dir.path / "tests" / "presets" / "machine.json", "{}");
    write_text(dir.path / "tests" / "presets" / "process.json", "{}");
    write_text(dir.path / "tests" / "model.obj", "");
    write_text(dir.path / "tests" / "case.json",
               R"({"args": ["--load-settings", "presets/machine.json;presets/process.json", "model.obj", "presets/machine.json;missing.json"]})");
    const std::vector<ModTestCase> cases = read_mod_test_cases(dir.path.string());
    REQUIRE(cases.size() == 1);
    const fs::path tests = dir.path / "tests";
    REQUIRE(cases[0].args.size() == 4);
    CHECK(cases[0].args[0] == "--load-settings");
    CHECK(cases[0].args[1] == (tests / "presets/machine.json").string() + ";" + (tests / "presets/process.json").string());
    CHECK(cases[0].args[2] == (tests / "model.obj").string());
    // One part that is not a file of the folder leaves the argument as written.
    CHECK(cases[0].args[3] == "presets/machine.json;missing.json");
}

TEST_CASE("A native mod turned off keeps its manifest until the next start", "[NativeMods]")
{
    TempDir        dir;
    const fs::path installed = dir.path / "mods";
    const fs::path package   = installed / "magpie_test_mod";
    copy_dir(test_mods_dir() / "magpie_test_mod", package);
    ModLoadOptions options;
    options.installed_dir = installed.string();
    options.cache_dir     = (dir.path / "cache").string();
    TestKey key("test-verified", KeyRole::Verified);
    sign_package(package.string(), key.key.private_pem, key.id);
    const ModLoadResult started = load_mods(options);
    INFO(joined(started.errors));
    REQUIRE(started.native->find("magpie_test_mod") != nullptr);
    REQUIRE(started.mods->find_mod("magpie_test_mod") != nullptr);
    // Turned off while running, then a reload (a JSON mod was installed): the code stays loaded, so the manifest
    // does too. Before (review 2026-10-22) the reload dropped the manifest while the code ran on, and the settings
    // dropped the mod's parameters.
    set_mod_enabled(installed.string(), "magpie_test_mod", false);
    options.load_native   = false;
    options.loaded_native = started.native;
    const ModLoadResult reloaded = load_mods(options);
    INFO(joined(reloaded.errors));
    CHECK(reloaded.mods->find_mod("magpie_test_mod") != nullptr);
    CHECK(any_contains(reloaded.notes, "magpie_test_mod: turns off at the next start"));
    // The next start, without the running code, leaves it off.
    options.load_native   = true;
    options.loaded_native = nullptr;
    const ModLoadResult next = load_mods(options);
    CHECK(next.mods->find_mod("magpie_test_mod") == nullptr);
    CHECK(any_contains(next.notes, "magpie_test_mod: turned off"));
}
