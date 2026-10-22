#pragma once

// MAGPIE: which addresses a mod may reach through the core.net service: the hosts and ports its manifest lists
// under "network" (ModSet.hpp). Pure functions, shared by the GUI service (ModNetService) and the tests.

#include <cstdint>
#include <string>

namespace Slic3r {
namespace ModHost {

struct Mod;
struct ModNetwork;

// An IPv4 literal in 10/8, 172.16/12, 192.168/16 or 169.254/16, an IPv6 literal in fe80::/10 or fc00::/7, or a
// name ending in ".local". Loopback and public addresses are not.
bool is_lan_host(const std::string &host);

// Whether one of `network`'s endpoints covers host:port. Host names compare without case; "*.example.com" covers
// names below example.com but not example.com itself; "lan" covers is_lan_host().
bool network_allows(const ModNetwork &network, const std::string &host, uint16_t port);

// The host and port of an http(s) URL (the scheme's port when none is given). False when it is not one.
bool url_host_port(const std::string &url, std::string &host, uint16_t &port);

// A text naming the mod's version and its endpoint list; the user's agreement is stored against it, so a new
// version or a changed list asks again.
std::string network_consent_key(const Mod &mod);

} // namespace ModHost
} // namespace Slic3r
