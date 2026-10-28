#include <catch2/catch_test_macros.hpp>

#include <libslic3r/MagpieReleaseVersion.hpp>

#include <string>

using namespace Slic3r;

namespace {

int cmp(const std::string &a, const std::string &b)
{
    const auto va = MagpieReleaseVersion::parse(a);
    const auto vb = MagpieReleaseVersion::parse(b);
    REQUIRE(va);
    REQUIRE(vb);
    return compare(*va, *vb);
}

} // namespace

TEST_CASE("Magpie release tags parse with five numeric fields and a label", "[MagpieVersion]")
{
    const auto v = MagpieReleaseVersion::parse("v2.5.0.2.1-beta.3");
    REQUIRE(v);
    CHECK(v->numbers == std::vector<long>{2, 5, 0, 2, 1});
    CHECK(v->prerelease == "beta.3");
    CHECK(v->to_string() == "2.5.0.2.1-beta.3");

    CHECK_FALSE(MagpieReleaseVersion::parse(""));
    CHECK_FALSE(MagpieReleaseVersion::parse("v"));
    CHECK_FALSE(MagpieReleaseVersion::parse("2"));
    CHECK_FALSE(MagpieReleaseVersion::parse("2.5."));
    CHECK_FALSE(MagpieReleaseVersion::parse("2.5.0-"));
    CHECK_FALSE(MagpieReleaseVersion::parse("2.x.0"));
    CHECK_FALSE(MagpieReleaseVersion::parse("nightly-builds"));
    // Orca's internal version is not a Magpie release; the update check reads the package version instead.
    CHECK(MagpieReleaseVersion::parse("2.5.0-modified.2.0")->numbers == std::vector<long>{2, 5, 0});
}

TEST_CASE("Magpie release versions order by every numeric field, then by label", "[MagpieVersion]")
{
    // The fifth field decides (Orca's Semver drops it).
    CHECK(cmp("2.5.0.2.1", "2.5.0.2.0") > 0);
    CHECK(cmp("2.5.0.2.10", "2.5.0.2.9") > 0);
    // The line field outranks the revision.
    CHECK(cmp("2.5.0.2.0", "2.5.0.1.9") > 0);
    CHECK(cmp("2.6.0.2.0", "2.5.0.9.9") > 0);
    CHECK(cmp("v2.5.0.2.0", "2.5.0.2.0") == 0);
    CHECK(cmp("2.5.0.2", "2.5.0.2.0") == 0);
    // A final release is newer than its own pre-releases, older than the next revision's.
    CHECK(cmp("2.5.0.2.0", "2.5.0.2.0-beta.1") > 0);
    CHECK(cmp("2.5.0.2.0-dev", "2.5.0.2.0") < 0);
    CHECK(cmp("2.5.0.2.1-beta.1", "2.5.0.2.0") > 0);
    // Labels: numbers numerically, words as text, a longer label after its prefix.
    CHECK(cmp("2.5.0.2.1-beta.10", "2.5.0.2.1-beta.2") > 0);
    CHECK(cmp("2.5.0.2.1-rc.1", "2.5.0.2.1-beta.9") > 0);
    CHECK(cmp("2.5.0.2.1-beta.1.1", "2.5.0.2.1-beta.1") > 0);
    CHECK(cmp("2.5.0.2.1-beta.2", "2.5.0.2.1-beta.2") == 0);
    // Line 1 tags still on the release list are older than any line 2 release.
    CHECK(cmp("v2.5.0.0.9-beta.7", "2.5.0.2.0") < 0);
    CHECK(cmp("v2.5.0.1.0", "2.5.0.2.0") < 0);
}
