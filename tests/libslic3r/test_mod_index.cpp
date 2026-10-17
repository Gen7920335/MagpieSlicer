#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libslic3r/ModHost/ModIndex.hpp>
#include <libslic3r/ModHost/ModSet.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <sstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::ModHost;
using Catch::Matchers::ContainsSubstring;
namespace fs = boost::filesystem;

namespace {

const std::string sample_mod = R"({
    "id": "slow_first_layer",
    "name": "Slow first layer",
    "version": "1.0.0",
    "params": { "speed": { "type": "float", "default": 20, "label": "First layer speed" } },
    "rules": [ { "target": "initial_layer_speed", "value": "slow_first_layer.speed" } ]
})";

std::string index_with(const std::string &id, const std::string &sha, const std::string &trust = "community")
{
    return R"({"version": 1, "mods": [{"id": ")" + id + R"(", "name": "Slow first layer", "version": "1.0.0",
        "description": "Prints the first layer slowly", "author": "Magpie", "tags": ["speed", "adhesion"],
        "url": "file:///mods/)" + id + R"(.json", "sha256": ")" + sha + R"(", "trust": ")" + trust + R"("}]})";
}

// A fresh directory under the system temp directory, removed when the test ends.
struct TempDir
{
    fs::path path = fs::temp_directory_path() / fs::unique_path("magpie-mods-%%%%-%%%%");
    TempDir() { fs::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }
};

std::string read_file(const fs::path &p)
{
    boost::nowide::ifstream in(p.string(), std::ios::binary);
    std::stringstream       s;
    s << in.rdbuf();
    return s.str();
}

void write_file(const fs::path &p, const std::string &text)
{
    boost::nowide::ofstream out(p.string(), std::ios::binary);
    out << text;
}

} // namespace

TEST_CASE("SHA-256 of a known string matches its published digest", "[ModIndex]")
{
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("A mod index reads every field of its entries", "[ModIndex]")
{
    const auto entries = parse_mod_index(index_with("slow_first_layer", sha256_hex(sample_mod), "verified"));
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].id == "slow_first_layer");
    CHECK(entries[0].version == "1.0.0");
    CHECK(entries[0].trust == ModTrust::Verified);
    CHECK(entries[0].tags.size() == 2);
}

TEST_CASE("A malformed mod index is rejected whole", "[ModIndex]")
{
    const std::string sha = sha256_hex(sample_mod);
    CHECK_THROWS_AS(parse_mod_index("{\"mods\": 3}"), ModError);
    CHECK_THROWS_AS(parse_mod_index(index_with("Bad-Id", sha)), ModError);
    CHECK_THROWS_AS(parse_mod_index(index_with("slow_first_layer", "abc")), ModError);
    CHECK_THROWS_AS(parse_mod_index(index_with("slow_first_layer", sha, "trusted")), ModError);
    std::string ftp = index_with("slow_first_layer", sha);
    ftp.replace(ftp.find("file://"), 7, "ftp://");
    CHECK_THROWS_AS(parse_mod_index(ftp), ModError);
}

TEST_CASE("Searching a mod index matches every word in any field", "[ModIndex]")
{
    const auto entries = parse_mod_index(index_with("slow_first_layer", sha256_hex(sample_mod)));
    CHECK(search_mod_index(entries, "").size() == 1);
    CHECK(search_mod_index(entries, "ADHESION slow").size() == 1);
    CHECK(search_mod_index(entries, "slow ironing").empty());
}

TEST_CASE("Installing a mod writes its definition after checking hash, id and conflicts", "[ModIndex]")
{
    TempDir  root;
    fs::path installed = root.path / "mods";
    auto     entry     = parse_mod_index(index_with("slow_first_layer", sha256_hex(sample_mod))).front();

    SECTION("a matching file is installed and loads") {
        const std::string path = install_mod(entry, sample_mod, installed, {installed});
        CHECK(read_file(path) == sample_mod);
        std::vector<std::string> errors;
        const ModSet set = ModSet::load_dir(installed, errors);
        CHECK(errors.empty());
        REQUIRE(set.mods().size() == 1);
        CHECK(set.mods().front().id == "slow_first_layer");
    }
    SECTION("a file that differs from the index is refused") {
        std::string tampered = sample_mod;
        tampered.replace(tampered.find("20"), 2, "90");
        CHECK_THROWS_WITH(install_mod(entry, tampered, installed, {installed}), ContainsSubstring("SHA-256"));
        CHECK_FALSE(fs::exists(installed / "slow_first_layer.json"));
    }
    SECTION("a file defining another id is refused") {
        auto other = parse_mod_index(index_with("other_mod", sha256_hex(sample_mod))).front();
        CHECK_THROWS_WITH(install_mod(other, sample_mod, installed, {installed}), ContainsSubstring("not \"other_mod\""));
    }
    SECTION("a mod that conflicts with a loaded one is refused") {
        // Same setting, same priority: the two cannot load together.
        fs::path bundled = root.path / "bundled";
        fs::create_directories(bundled);
        write_file(bundled / "fast.json", R"({"id": "fast_first_layer", "version": "1",
            "rules": [{"target": "initial_layer_speed", "value": "50"}]})");
        CHECK_THROWS_AS(install_mod(entry, sample_mod, installed, {bundled, installed}), ModError);
        CHECK_FALSE(fs::exists(installed / "slow_first_layer.json"));
    }
    SECTION("a bundled mod's id cannot be taken over") {
        fs::path bundled = root.path / "bundled";
        fs::create_directories(bundled);
        write_file(bundled / "slow_first_layer.json", sample_mod);
        CHECK_THROWS_WITH(install_mod(entry, sample_mod, installed, {bundled, installed}), ContainsSubstring("bundled"));
    }
    SECTION("an installed copy is replaced by a newer one") {
        install_mod(entry, sample_mod, installed, {installed});
        std::string v2 = sample_mod;
        v2.replace(v2.find("1.0.0"), 5, "1.1.0");
        auto entry2 = entry;
        entry2.sha256 = sha256_hex(v2);
        install_mod(entry2, v2, installed, {installed});
        CHECK(read_file(installed / "slow_first_layer.json") == v2);
    }
}

TEST_CASE("A project's mod settings name the mods it needs that are not loaded", "[ModIndex]")
{
    DynamicPrintConfig config;
    config.set_key_value("mod_settings", new ConfigOptionString(R"({"slow_first_layer.speed": 15, "half_layer.outer_walls": true})"));
    config.set_key_value("printer_mod_settings", new ConfigOptionString(R"({"brush.count": 3})"));
    config.set_key_value("filament_mod_settings", new ConfigOptionStrings({R"({"dry.hours": 4})", ""}));
    CHECK(missing_mod_ids(config, {"half_layer"}) == std::vector<std::string>{"brush", "dry", "slow_first_layer"});
    CHECK(missing_mod_ids(config, {"half_layer", "brush", "dry", "slow_first_layer"}).empty());
}
