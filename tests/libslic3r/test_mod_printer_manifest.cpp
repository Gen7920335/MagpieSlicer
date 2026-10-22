// MAGPIE: the manifest parts of printer-connection mods ("network" and "gui"."printer", ModSet.hpp), the network
// allow-list the core.net service applies (NetPermission.hpp) and the core service registry. The expected values are
// written out from the manifests below, not read back from the code under test.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libslic3r/ModHost/ModSet.hpp>
#include <libslic3r/ModHost/NativeMods.hpp>
#include <libslic3r/ModHost/NetPermission.hpp>

#include <string>

using namespace Slic3r::ModHost;
using Catch::Matchers::ContainsSubstring;

namespace {

// A package manifest with a printer.agent slot, so "gui".printer and "network" are allowed; `extra` is spliced in
// at the top level.
std::string printer_mod(const std::string &extra)
{
    return R"({"id": "demo_printer", "version": "1.2.0", "api": {"major": 1}, "binary": "demo_printer",
               "thread_safe": true,
               "slots": [{"slot": "printer.agent", "version": 1, "impl": "demo"}])" +
           (extra.empty() ? std::string() : ", " + extra) + "}";
}

const char *k_network = R"("network": {"reason": "Talks to the printer on your network.", "reason_ko": "...",
    "endpoints": [{"host": "lan", "ports": [18000, 9000]}, {"host": "*.example.com", "ports": [443]},
                  {"host": "files.example.org", "ports": [443, 8443]}]})";

} // namespace

TEST_CASE("A printer mod's network list and panel are read from its manifest", "[ModHost][PrinterAgent]")
{
    const Mod mod = ModSet::parse_mod(printer_mod(std::string(k_network) + R"(, "gui": {"printer": {
        "fields": [{"id": "host", "label": "Address"},
                   {"id": "mode", "type": "choice", "options": ["a", {"value": "b", "label": "Bee"}], "default": "a"},
                   {"id": "code", "type": "secret", "visible": "mode == \"b\""},
                   {"id": "folder", "type": "path"}],
        "actions": [{"id": "pause", "label": "Pause", "command": "pause", "row": "job"},
                    {"id": "home", "label": "Home", "command": "home", "args": {"axes": "xyz"}, "idle_only": true,
                     "confirm": "Move the head?"}],
        "panels": [{"id": "camera", "label": "Camera", "page": "web/camera.html"}],
        "notices": [{"id": "lan", "text": "LAN mode on", "visible": "mode == \"a\""}]}})"));

    REQUIRE(mod.network.endpoints.size() == 3);
    CHECK(mod.network.endpoints[0].host == "lan");
    CHECK(mod.network.endpoints[0].ports == std::vector<uint16_t>{18000, 9000});
    CHECK(mod.network.reasons.at("ko") == "...");

    const ModGuiPrinter &p = mod.gui.printer;
    REQUIRE(p.fields.size() == 4);
    CHECK(p.fields[0].kind == "text");
    CHECK(p.fields[1].kind == "choice");
    CHECK(p.fields[1].options.size() == 2);
    CHECK(p.fields[1].options[0].label == "a"); // a bare option labels itself
    CHECK(p.fields[1].options[1].label == "Bee");
    CHECK(p.fields[2].kind == "secret");
    CHECK(p.fields[2].visible.has_value());
    REQUIRE(p.actions.size() == 2);
    CHECK(p.actions[1].args_json == R"({"axes":"xyz"})");
    CHECK(p.actions[1].idle_only);
    CHECK(p.actions[1].confirm == "Move the head?");
    REQUIRE(p.panels.size() == 1);
    CHECK(p.panels[0].page == "web/camera.html");
    CHECK(p.notices.size() == 1);
}

TEST_CASE("Printer mod manifests with unsafe or inconsistent parts are refused", "[ModHost][PrinterAgent]")
{
    // A panel without the slot it belongs to.
    CHECK_THROWS_WITH(ModSet::parse_mod(R"({"id": "x", "version": "1", "gui": {"printer": {"fields": []}}})"),
                      ContainsSubstring("printer.agent"));
    // Network access without native code.
    CHECK_THROWS_AS(ModSet::parse_mod(R"({"id": "x", "version": "1", "network": {"reason": "r",
        "endpoints": [{"host": "lan", "ports": [80]}]}})"), ModError);
    // A secret with a default would put the secret into the manifest.
    CHECK_THROWS_WITH(ModSet::parse_mod(printer_mod(R"("gui": {"printer": {"fields": [{"id": "pw", "type": "secret", "default": "1234"}]}})")),
                      ContainsSubstring("secret"));
    // Pages outside the mod folder.
    for (const char *page : {"../x.html", "/abs.html", "C:/x.html", "web\\\\x.html", "web//x.html", ""})
        CHECK_THROWS_AS(ModSet::parse_mod(printer_mod(std::string(R"("gui": {"printer": {"panels": [{"id": "p", "page": ")") + page + R"("}]}})")),
                        ModError);
    // Hosts: a bare wildcard, a single label, no reason, port 0 and 65536.
    for (const char *net : {R"({"reason": "r", "endpoints": [{"host": "*", "ports": [80]}]})",
                            R"({"reason": "r", "endpoints": [{"host": "printer", "ports": [80]}]})",
                            R"({"endpoints": [{"host": "lan", "ports": [80]}]})",
                            R"({"reason": "r", "endpoints": [{"host": "lan", "ports": [0]}]})",
                            R"({"reason": "r", "endpoints": [{"host": "lan", "ports": [65536]}]})",
                            R"({"reason": "r", "endpoints": []})"})
        CHECK_THROWS_AS(ModSet::parse_mod(printer_mod(std::string("\"network\": ") + net)), ModError);
    // Port 65535 is the last valid one.
    CHECK_NOTHROW(ModSet::parse_mod(printer_mod(R"("network": {"reason": "r", "endpoints": [{"host": "lan", "ports": [65535]}]})")));
    // An action's command must be a plain name, a choice needs options.
    CHECK_THROWS_AS(ModSet::parse_mod(printer_mod(R"("gui": {"printer": {"actions": [{"id": "a", "command": "G28 X"}]}})")), ModError);
    CHECK_THROWS_AS(ModSet::parse_mod(printer_mod(R"("gui": {"printer": {"fields": [{"id": "c", "type": "choice"}]}})")), ModError);
}

TEST_CASE("LAN addresses are the private, link-local and .local ones", "[ModHost][PrinterAgent]")
{
    for (const char *host : {"10.0.0.1", "10.255.255.255", "172.16.0.1", "172.31.255.254", "192.168.1.20", "169.254.3.4",
                             "fe80::1", "[fe80::1]", "fd12:3456::1", "printer.local", "Printer.LOCAL"})
        CHECK(is_lan_host(host));
    // Just outside each range, loopback, public addresses, malformed literals and look-alike names.
    for (const char *host : {"172.15.255.255", "172.32.0.1", "192.169.0.1", "169.253.0.1", "11.0.0.1", "127.0.0.1",
                             "8.8.8.8", "::1", "fec0::1", "fbff::1", "256.1.1.1", "10.0.0", "10.0.0.1.5", "local",
                             ".local", "printer.local.example.com", ""})
        CHECK_FALSE(is_lan_host(host));
}

TEST_CASE("The network list allows exactly its hosts and ports", "[ModHost][PrinterAgent]")
{
    const Mod mod = ModSet::parse_mod(printer_mod(k_network));
    const ModNetwork &n = mod.network;
    CHECK(network_allows(n, "192.168.1.20", 18000));
    CHECK(network_allows(n, "192.168.1.20", 9000));
    CHECK_FALSE(network_allows(n, "192.168.1.20", 18001)); // port not listed for lan
    CHECK_FALSE(network_allows(n, "8.8.8.8", 18000));      // public address on a lan port
    CHECK(network_allows(n, "api.example.com", 443));
    CHECK(network_allows(n, "A.B.Example.COM", 443));
    CHECK_FALSE(network_allows(n, "example.com", 443));     // the wildcard covers names below only
    CHECK_FALSE(network_allows(n, "evilexample.com", 443)); // not a label boundary
    CHECK_FALSE(network_allows(n, "api.example.com", 80));
    CHECK(network_allows(n, "files.example.org", 8443));
    CHECK_FALSE(network_allows(n, "x.files.example.org", 443)); // exact name only
    CHECK_FALSE(network_allows(n, "", 443));
    CHECK_FALSE(network_allows(ModNetwork{}, "192.168.1.20", 18000)); // no list, nothing allowed
}

TEST_CASE("URLs give the host and port the network check sees", "[ModHost][PrinterAgent]")
{
    std::string host;
    uint16_t    port = 0;
    REQUIRE(url_host_port("http://192.168.1.20:18000/info", host, port));
    CHECK(host == "192.168.1.20");
    CHECK(port == 18000);
    REQUIRE(url_host_port("https://api.example.com/path?q=1", host, port));
    CHECK(host == "api.example.com");
    CHECK(port == 443);
    REQUIRE(url_host_port("HTTP://printer.local", host, port));
    CHECK(port == 80);
    // The host is what follows user info, so "allowed.example.com@" cannot smuggle another host past the check.
    REQUIRE(url_host_port("https://api.example.com@8.8.8.8/x", host, port));
    CHECK(host == "8.8.8.8");
    REQUIRE(url_host_port("http://[fe80::1]:8080/", host, port));
    CHECK(host == "[fe80::1]");
    CHECK(port == 8080);
    REQUIRE(url_host_port("http://h.example.com:65535", host, port));
    CHECK(port == 65535);
    for (const char *bad : {"ftp://h.example.com", "h.example.com", "http://", "http://h:0/", "http://h:65536/", "http://h:x/",
                            "http://[fe80::1/"})
        CHECK_FALSE(url_host_port(bad, host, port));
}

TEST_CASE("Agreeing to a mod's network list is asked again for a new version or list", "[ModHost][PrinterAgent]")
{
    const Mod a = ModSet::parse_mod(printer_mod(k_network));
    Mod       newer = a;
    newer.version   = "1.3.0";
    Mod wider       = a;
    wider.network.endpoints[0].ports.push_back(80);
    CHECK(network_consent_key(a) == network_consent_key(ModSet::parse_mod(printer_mod(k_network))));
    CHECK(network_consent_key(a) != network_consent_key(newer));
    CHECK(network_consent_key(a) != network_consent_key(wider));
}

TEST_CASE("A registered core service is offered to mods by name and version", "[ModHost][PrinterAgent]")
{
    static const struct { uint32_t size; int marker; } table_v2 = {8, 42};
    register_core_service("test.service_registry", 2, &table_v2, sizeof(table_v2));
    uint32_t size = 0;
    CHECK(core_service("test.service_registry", 1, &size) == &table_v2);
    CHECK(size == sizeof(table_v2));
    CHECK(core_service("test.service_registry", 2, nullptr) == &table_v2);
    CHECK(core_service("test.service_registry", 3, nullptr) == nullptr); // newer than registered
    CHECK(core_service("test.no_such_service", 1, nullptr) == nullptr);
    // core.geometry stays the core's own.
    CHECK(core_service("core.geometry", 1, nullptr) != nullptr);
}
