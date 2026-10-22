#include "NetPermission.hpp"

#include "ModSet.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <sstream>

namespace Slic3r {
namespace ModHost {

namespace {

// Four decimal octets "a.b.c.d", each 0..255. False for anything else.
bool parse_ipv4(const std::string &text, std::array<int, 4> &octets)
{
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t end = i < 3 ? text.find('.', pos) : text.size();
        if (end == std::string::npos || end == pos || end - pos > 3)
            return false;
        const std::string part = text.substr(pos, end - pos);
        if (part.find_first_not_of("0123456789") != std::string::npos)
            return false;
        octets[i] = std::atoi(part.c_str());
        if (octets[i] > 255)
            return false;
        pos = end + 1;
    }
    return true;
}

} // namespace

bool is_lan_host(const std::string &host_in)
{
    std::string host = boost::algorithm::to_lower_copy(host_in);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    std::array<int, 4> o{};
    if (parse_ipv4(host, o))
        return o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168) ||
               (o[0] == 169 && o[1] == 254);
    if (host.find(':') != std::string::npos) {
        // IPv6: the first group decides. fe80::/10 is fe80..febf, fc00::/7 is fc00..fdff.
        const std::string first = host.substr(0, host.find(':'));
        if (first.empty() || first.size() > 4 || first.find_first_not_of("0123456789abcdef") != std::string::npos)
            return false;
        const unsigned long g = std::strtoul(first.c_str(), nullptr, 16);
        return (g >= 0xfe80 && g <= 0xfebf) || (g >= 0xfc00 && g <= 0xfdff);
    }
    return host.size() > 6 && boost::algorithm::ends_with(host, ".local");
}

bool network_allows(const ModNetwork &network, const std::string &host_in, uint16_t port)
{
    const std::string host = boost::algorithm::to_lower_copy(host_in);
    if (host.empty())
        return false;
    for (const ModNetworkEndpoint &ep : network.endpoints) {
        if (std::find(ep.ports.begin(), ep.ports.end(), port) == ep.ports.end())
            continue;
        const std::string pattern = boost::algorithm::to_lower_copy(ep.host);
        if (pattern == "lan") {
            if (is_lan_host(host))
                return true;
        } else if (boost::algorithm::starts_with(pattern, "*.")) {
            // ".example.com" must end the name, with at least one label before it.
            const std::string suffix = pattern.substr(1);
            if (host.size() > suffix.size() && boost::algorithm::ends_with(host, suffix))
                return true;
        } else if (host == pattern) {
            return true;
        }
    }
    return false;
}

bool url_host_port(const std::string &url, std::string &host, uint16_t &port)
{
    const std::string lower = boost::algorithm::to_lower_copy(url);
    size_t            start = 0;
    uint16_t          default_port = 0;
    if (boost::algorithm::starts_with(lower, "http://")) {
        start = 7;
        default_port = 80;
    } else if (boost::algorithm::starts_with(lower, "https://")) {
        start = 8;
        default_port = 443;
    } else
        return false;
    size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    // user:password@host: the host is what follows the last '@'.
    if (size_t at = authority.rfind('@'); at != std::string::npos)
        authority = authority.substr(at + 1);
    std::string port_text;
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos)
            return false;
        host = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':')
                return false;
            port_text = authority.substr(close + 2);
        }
    } else if (size_t colon = authority.rfind(':'); colon != std::string::npos) {
        host      = authority.substr(0, colon);
        port_text = authority.substr(colon + 1);
    } else
        host = authority;
    if (host.empty())
        return false;
    if (port_text.empty()) {
        port = default_port;
        return true;
    }
    if (port_text.size() > 5 || port_text.find_first_not_of("0123456789") != std::string::npos)
        return false;
    const unsigned long p = std::strtoul(port_text.c_str(), nullptr, 10);
    if (p == 0 || p > 65535)
        return false;
    port = uint16_t(p);
    return true;
}

std::string network_consent_key(const Mod &mod)
{
    std::ostringstream out;
    out << mod.id << '@' << mod.version;
    for (const ModNetworkEndpoint &ep : mod.network.endpoints) {
        out << ';' << boost::algorithm::to_lower_copy(ep.host);
        for (uint16_t p : ep.ports)
            out << ',' << p;
    }
    return out.str();
}

} // namespace ModHost
} // namespace Slic3r
