#include "ModNetService.hpp"

// Sockets first: winsock2.h must come before anything that may pull in windows.h.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static const socket_t k_no_socket = INVALID_SOCKET;
static void close_socket(socket_t s) { ::closesocket(s); }
static bool socket_would_block() { const int e = ::WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
static void set_nonblocking(socket_t s, bool on) { u_long mode = on ? 1 : 0; ::ioctlsocket(s, FIONBIO, &mode); }
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using socket_t = int;
static const socket_t k_no_socket = -1;
static void close_socket(socket_t s) { ::close(s); }
static bool socket_would_block() { return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN; }
static void set_nonblocking(socket_t s, bool on)
{
    const int flags = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif

#include "Http.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ModHost/ModSet.hpp"
#include "libslic3r/ModHost/NativeMods.hpp"
#include "libslic3r/ModHost/NetPermission.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "../../../mods/sdk/include/magpie_net.h"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <wx/app.h>
#include <wx/secretstore.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>


namespace Slic3r {
namespace ModNet {

using ModHost::Mod;
using ModHost::NativeMod;

namespace {

const char *const k_consent_section = "mod_network_consent";

std::atomic<bool> g_shutting_down{false};

// The agreements, mod id -> the network_consent_key the user agreed to. Read by worker threads, written on the GUI
// thread; AppConfig itself is not thread safe, so the service keeps its own copy.
std::mutex                         g_consent_mutex;
std::map<std::string, std::string> g_consent;

// Text into a caller's buffer (cap bytes with the NUL); MP_ERR_BUFFER when cut.
int32_t copy_out(const std::string &text, char *buf, uint32_t cap)
{
    if (buf == nullptr || cap == 0)
        return MP_ERR_ARGUMENT;
    const size_t n = std::min<size_t>(text.size(), cap - 1);
    std::memcpy(buf, text.data(), n);
    buf[n] = '\0';
    return n == text.size() ? MP_OK : MP_ERR_BUFFER;
}

char *dup_text(const std::string &text)
{
    char *out = static_cast<char *>(std::malloc(text.size() + 1));
    if (out != nullptr) {
        std::memcpy(out, text.data(), text.size());
        out[text.size()] = '\0';
    }
    return out;
}

const NativeMod *caller(void *svc) { return static_cast<const NativeMod *>(svc); }

// Why `mod` may not reach host:port now, or "" when it may.
std::string address_refusal(const NativeMod *mod, const std::string &host, uint16_t port)
{
    if (mod == nullptr)
        return "core.net: unknown caller";
    if (g_shutting_down.load())
        return "core.net: the application is closing";
    if (!ModHost::network_allows(mod->manifest().network, host, port))
        return "core.net: " + host + ":" + std::to_string(port) + " is not in the mod's \"network\" list";
    if (!has_consent(mod->manifest()))
        return "core.net: the user has not agreed to this mod's network access";
    return {};
}

// A host as the network check sees it: brackets of an IPv6 literal kept, as url_host_port gives it.
std::string bare_host(const std::string &host)
{
    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        return host.substr(1, host.size() - 2);
    return host;
}

// ---------------------------------------------------------------------------------------------------------------
// HTTP

int32_t net_http(void *svc, const mp_http_request *req, mp_http_response *resp)
{
    if (req == nullptr || resp == nullptr || req->size < sizeof(mp_http_request) || req->url == nullptr)
        return MP_ERR_ARGUMENT;
    *resp = mp_http_response{};
    auto fail = [resp](const std::string &why) {
        resp->error = dup_text(why);
        return int32_t(MP_ERR_FAILED);
    };

    const std::string url = req->url;
    std::string       host;
    uint16_t          port = 0;
    if (!ModHost::url_host_port(url, host, port))
        return fail("core.net: not an http or https URL");
    if (std::string why = address_refusal(caller(svc), host, port); !why.empty())
        return fail(why);
    const bool lan = ModHost::is_lan_host(host);

    const int bodies = (req->body != nullptr ? 1 : 0) + (req->body_file != nullptr ? 1 : 0) + (req->n_parts > 0 ? 1 : 0);
    if (bodies > 1 || (req->n_parts > 0 && (req->parts == nullptr || req->method != MP_HTTP_POST)) ||
        (req->body_file != nullptr && req->method != MP_HTTP_PUT) ||
        (req->body != nullptr && req->method != MP_HTTP_POST && req->method != MP_HTTP_DELETE) ||
        (req->method == MP_HTTP_GET && bodies > 0) || (req->n_headers > 0 && req->headers == nullptr))
        return MP_ERR_ARGUMENT;

    if (req->method < MP_HTTP_GET || req->method > MP_HTTP_DELETE)
        return MP_ERR_ARGUMENT;
    Http http = req->method == MP_HTTP_POST ? Http::post(url) :
                req->method == MP_HTTP_PUT  ? Http::put(url) :
                req->method == MP_HTTP_DELETE ? Http::del(url) : Http::get(url);

    // An internet address is always checked against the system's certificates; only a LAN printer may present a
    // self-signed one. A redirect would reach an address the check above did not see.
    http.tls_verify(req->verify_tls != 0 || !lan).follow_redirects(false);
    http.timeout_connect(req->connect_timeout_s > 0 ? long(req->connect_timeout_s) : 10L);
    if (req->total_timeout_s > 0)
        http.timeout_max(long(req->total_timeout_s));
    // Response size in bytes; 16 MiB unless the mod asks otherwise.
    http.size_limit(req->max_response_bytes > 0 ? size_t(req->max_response_bytes) : size_t(16) << 20);
    for (uint32_t i = 0; i < req->n_headers; ++i)
        if (req->headers[i].name != nullptr && req->headers[i].value != nullptr)
            http.header(req->headers[i].name, req->headers[i].value);
    if (req->body != nullptr) {
        const std::string body(req->body, size_t(req->body_len));
        if (req->method == MP_HTTP_POST)
            http.set_post_body(body);
        else
            http.set_del_body(body);
    }
    if (req->body_file != nullptr)
        http.set_put_body(boost::filesystem::path(req->body_file));
    for (uint32_t i = 0; i < req->n_parts; ++i) {
        const mp_http_part &part = req->parts[i];
        if (part.name == nullptr)
            return MP_ERR_ARGUMENT;
        if (part.file_path != nullptr) {
            const boost::filesystem::path path(part.file_path);
            if (part.file_name != nullptr)
                http.form_add_file(part.name, path, std::string(part.file_name));
            else
                http.form_add_file(part.name, path);
        } else
            http.form_add(part.name, part.value != nullptr ? part.value : "");
    }

    std::string body, error;
    unsigned    status    = 0;
    bool        cancelled = false;
    // Statuses outside 2xx and 4xx/5xx (a redirect, now never followed) reach neither callback: the last status
    // line seen tells them.
    unsigned header_status = 0;
    http.on_header_callback([&header_status](std::string line) {
        static const std::regex status_line(R"(^HTTP/[0-9.]+ ([0-9]{3}))");
        std::smatch             m;
        if (std::regex_search(line, m, status_line))
            header_status = unsigned(std::stoul(m[1].str()));
    });
    http.on_complete([&](std::string b, unsigned s) {
        body   = std::move(b);
        status = s;
    });
    http.on_error([&](std::string b, std::string e, unsigned s) {
        body   = std::move(b);
        error  = std::move(e);
        status = s;
    });
    if (req->progress != nullptr) {
        auto progress = req->progress;
        void *user    = req->progress_user;
        http.on_progress([progress, user, &cancelled](Http::Progress p, bool &cancel) {
            if (g_shutting_down.load() || progress(user, uint64_t(p.ulnow), uint64_t(p.ultotal)) != 0) {
                cancel    = true;
                cancelled = true;
            }
        });
    }
    http.perform_sync();

    if (cancelled)
        return int32_t(MP_ERR_CANCELLED);
    if (status == 0 && error.empty())
        status = header_status;
    resp->status   = int32_t(status);
    resp->body     = dup_text(body);
    resp->body_len = body.size();
    if (status == 0) {
        resp->error = dup_text(error.empty() ? std::string("no response") : error);
        return MP_ERR_FAILED;
    }
    return MP_OK;
}

void net_free_response(mp_http_response *resp)
{
    if (resp == nullptr)
        return;
    std::free(resp->body);
    std::free(resp->error);
    *resp = mp_http_response{};
}

// ---------------------------------------------------------------------------------------------------------------
// TLS sockets

// One connection. OpenSSL objects are not safe to use from two threads at once, so every SSL call takes `ssl_mutex`;
// waiting for data (select) happens outside it, so a writer is not held up by a reader that waits.
struct TlsConn
{
    const NativeMod *mod = nullptr;
    socket_t         sock = k_no_socket;
    SSL_CTX         *ctx  = nullptr;
    SSL             *ssl  = nullptr;
    std::mutex       ssl_mutex;
    bool             closed = false;

    ~TlsConn()
    {
        if (ssl != nullptr) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (ctx != nullptr)
            SSL_CTX_free(ctx);
        if (sock != k_no_socket)
            close_socket(sock);
    }
};

std::mutex                                     g_conns_mutex;
std::map<uint64_t, std::shared_ptr<TlsConn>>   g_conns;
uint64_t                                       g_next_conn = 1;

std::shared_ptr<TlsConn> find_conn(void *svc, uint64_t id)
{
    std::lock_guard<std::mutex> lock(g_conns_mutex);
    auto it = g_conns.find(id);
    if (it == g_conns.end() || it->second->mod != caller(svc))
        return nullptr;
    return it->second;
}

// Waits until `s` can be read (read) or written (!read), at most timeout_ms milliseconds. 1 ready, 0 timeout, -1 error.
int wait_socket(socket_t s, bool read, uint32_t timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec  = long(timeout_ms / 1000);
    tv.tv_usec = long((timeout_ms % 1000) * 1000);
    const int n = ::select(int(s) + 1, read ? &set : nullptr, read ? nullptr : &set, nullptr, &tv);
    return n > 0 ? 1 : n == 0 ? 0 : -1;
}

std::string ssl_error_text()
{
    const unsigned long e = ERR_get_error();
    if (e == 0)
        return "TLS failure";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return buf;
}

// A TCP connection to host:port within timeout_ms, or k_no_socket with `error` set. A ".local" name must resolve to a
// LAN address: the manifest's "lan" covers it by name, and the check is only meaningful if the address agrees.
socket_t tcp_connect(const std::string &host, uint16_t port, uint32_t timeout_ms, std::string &error)
{
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found   = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || found == nullptr) {
        error = "cannot resolve the printer's address";
        return k_no_socket;
    }
    const bool must_be_lan = ModHost::is_lan_host(host);
    socket_t   result      = k_no_socket;
    for (addrinfo *ai = found; ai != nullptr && result == k_no_socket; ai = ai->ai_next) {
        char text[INET6_ADDRSTRLEN] = {};
        if (::getnameinfo(ai->ai_addr, socklen_t(ai->ai_addrlen), text, sizeof(text), nullptr, 0, NI_NUMERICHOST) != 0)
            continue;
        if (must_be_lan && !ModHost::is_lan_host(text)) {
            error = "the LAN name resolved to an address outside the LAN";
            continue;
        }
        socket_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == k_no_socket)
            continue;
        set_nonblocking(s, true);
        const int rc = ::connect(s, ai->ai_addr, int(ai->ai_addrlen));
        if (rc == 0 || (socket_would_block() && wait_socket(s, false, timeout_ms) == 1)) {
            int       so_error = 0;
            socklen_t len      = sizeof(so_error);
            ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &len);
            if (so_error == 0) {
                result = s;
                break;
            }
        }
        error = "cannot connect to the printer";
        close_socket(s);
    }
    ::freeaddrinfo(found);
    return result;
}

int32_t net_tls_connect(void *svc, const mp_tls_options *opt, uint64_t *conn_id, char *err, uint32_t err_cap)
{
    auto fail = [err, err_cap](const std::string &why, int32_t code = MP_ERR_FAILED) {
        if (err != nullptr && err_cap > 0)
            copy_out(why, err, err_cap);
        return code;
    };
    if (opt == nullptr || conn_id == nullptr || opt->size < sizeof(mp_tls_options) || opt->host == nullptr || opt->port == 0)
        return fail("core.net: bad arguments", MP_ERR_ARGUMENT);
    const std::string host = bare_host(opt->host);
    if (std::string why = address_refusal(caller(svc), opt->host, opt->port); !why.empty())
        return fail(why);
    // Only a LAN printer may skip the server check (its broker certificate is self-signed).
    const bool verify = opt->verify_server != 0 || !ModHost::is_lan_host(host);

    auto        conn = std::make_shared<TlsConn>();
    std::string error;
    conn->mod  = caller(svc);
    conn->sock = tcp_connect(host, opt->port, opt->connect_timeout_ms > 0 ? opt->connect_timeout_ms : 10000, error);
    if (conn->sock == k_no_socket)
        return fail(error);

    conn->ctx = SSL_CTX_new(TLS_client_method());
    if (conn->ctx == nullptr)
        return fail(ssl_error_text());
    SSL_CTX_set_min_proto_version(conn->ctx, TLS1_2_VERSION);
    if (verify) {
        SSL_CTX_set_verify(conn->ctx, SSL_VERIFY_PEER, nullptr);
        const bool loaded = opt->ca_file != nullptr ? SSL_CTX_load_verify_locations(conn->ctx, opt->ca_file, nullptr) == 1 :
                                                      SSL_CTX_set_default_verify_paths(conn->ctx) == 1;
        if (!loaded)
            return fail("cannot load the certificates to check the server against");
    } else
        SSL_CTX_set_verify(conn->ctx, SSL_VERIFY_NONE, nullptr);
    if (opt->client_cert_file != nullptr || opt->client_key_file != nullptr) {
        if (opt->client_cert_file == nullptr || opt->client_key_file == nullptr ||
            SSL_CTX_use_certificate_chain_file(conn->ctx, opt->client_cert_file) != 1 ||
            SSL_CTX_use_PrivateKey_file(conn->ctx, opt->client_key_file, SSL_FILETYPE_PEM) != 1)
            return fail("cannot load the client certificate");
    }
    conn->ssl = SSL_new(conn->ctx);
    if (conn->ssl == nullptr)
        return fail(ssl_error_text());
    SSL_set_fd(conn->ssl, int(conn->sock));
    // SNI and the name check apply to names only; a printer is reached by its address literal.
    const bool is_name = host.find_first_not_of("0123456789.") != std::string::npos && host.find(':') == std::string::npos;
    if (is_name)
        SSL_set_tlsext_host_name(conn->ssl, host.c_str());
    if (verify) {
        if (is_name)
            SSL_set1_host(conn->ssl, host.c_str());
        else
            X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(conn->ssl), host.c_str());
    }

    // The handshake on the non-blocking socket, with the connect timeout as its budget.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(opt->connect_timeout_ms > 0 ? opt->connect_timeout_ms : 10000);
    for (;;) {
        const int rc = SSL_connect(conn->ssl);
        if (rc == 1)
            break;
        const int e = SSL_get_error(conn->ssl, rc);
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) || left <= 0)
            return fail(left <= 0 ? std::string("TLS handshake timed out") : ssl_error_text());
        if (wait_socket(conn->sock, e == SSL_ERROR_WANT_READ, uint32_t(left)) < 0)
            return fail("TLS handshake failed");
    }

    std::lock_guard<std::mutex> lock(g_conns_mutex);
    *conn_id = g_next_conn++;
    g_conns.emplace(*conn_id, std::move(conn));
    return MP_OK;
}

int32_t net_tls_read(void *svc, uint64_t id, void *buf, uint32_t cap, uint32_t timeout_ms, uint32_t *got)
{
    if (buf == nullptr || got == nullptr || cap == 0)
        return MP_ERR_ARGUMENT;
    *got = 0;
    std::shared_ptr<TlsConn> conn = find_conn(svc, id);
    if (!conn || g_shutting_down.load())
        return MP_ERR_FAILED;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(conn->ssl_mutex);
            if (conn->closed)
                return MP_ERR_FAILED;
            const int rc = SSL_read(conn->ssl, buf, int(std::min<uint32_t>(cap, 1u << 30)));
            if (rc > 0) {
                *got = uint32_t(rc);
                return MP_OK;
            }
            const int e = SSL_get_error(conn->ssl, rc);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
                conn->closed = true;
                return MP_ERR_FAILED;
            }
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            return MP_OK; // a timeout: nothing arrived
        // At most 200 ms per wait, so a close from another thread or the application ending is seen soon.
        if (wait_socket(conn->sock, true, uint32_t(std::min<long long>(left, 200))) < 0) {
            std::lock_guard<std::mutex> lock(conn->ssl_mutex);
            conn->closed = true;
            return MP_ERR_FAILED;
        }
    }
}

int32_t net_tls_write(void *svc, uint64_t id, const void *buf, uint32_t len)
{
    if (buf == nullptr && len > 0)
        return MP_ERR_ARGUMENT;
    std::shared_ptr<TlsConn> conn = find_conn(svc, id);
    if (!conn || g_shutting_down.load())
        return MP_ERR_FAILED;
    const char *p    = static_cast<const char *>(buf);
    uint32_t    left = len;
    // A write that cannot make progress for 10 s fails: the broker stopped reading.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (left > 0) {
        int e = SSL_ERROR_NONE;
        {
            std::lock_guard<std::mutex> lock(conn->ssl_mutex);
            if (conn->closed)
                return MP_ERR_FAILED;
            const int rc = SSL_write(conn->ssl, p, int(left));
            if (rc > 0) {
                p += rc;
                left -= uint32_t(rc);
                continue;
            }
            e = SSL_get_error(conn->ssl, rc);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
                conn->closed = true;
                return MP_ERR_FAILED;
            }
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (ms <= 0 || wait_socket(conn->sock, e == SSL_ERROR_WANT_READ, uint32_t(std::min<long long>(ms, 200))) < 0)
            return MP_ERR_FAILED;
    }
    return MP_OK;
}

void net_tls_close(void *svc, uint64_t id)
{
    std::shared_ptr<TlsConn> conn;
    {
        std::lock_guard<std::mutex> lock(g_conns_mutex);
        auto it = g_conns.find(id);
        if (it == g_conns.end() || it->second->mod != caller(svc))
            return;
        conn = std::move(it->second);
        g_conns.erase(it);
    }
    std::lock_guard<std::mutex> lock(conn->ssl_mutex);
    conn->closed = true;
    // The last owner (this one, or a reader still inside tls_read) frees the connection.
}

// ---------------------------------------------------------------------------------------------------------------
// Secrets: the operating system's credential store, one entry per mod and key.

bool valid_secret_key(const char *key)
{
    if (key == nullptr)
        return false;
    const std::string k = key;
    return !k.empty() && k.size() <= 128 &&
           std::all_of(k.begin(), k.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':'; });
}

wxString secret_service(const std::string &mod_id, const char *key)
{
    return wxString::FromUTF8(("MagpieSlicerNext/mod/" + mod_id + "/" + key).c_str());
}

// The store operations by mod id, shared by the service (the calling mod) and the printer panel (put_secret).
int32_t store_put(const std::string &mod_id, const char *key, const char *value)
{
    if (!valid_secret_key(key) || value == nullptr)
        return MP_ERR_ARGUMENT;
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return MP_ERR_FAILED;
    return store.Save(secret_service(mod_id, key), wxString::FromUTF8(key), wxSecretValue(std::strlen(value), value)) ? MP_OK : MP_ERR_FAILED;
}

int32_t store_get(const std::string &mod_id, const char *key, std::string &value)
{
    if (!valid_secret_key(key))
        return MP_ERR_ARGUMENT;
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return MP_ERR_FAILED;
    wxString      user;
    wxSecretValue secret;
    if (!store.Load(secret_service(mod_id, key), user, secret) || !secret.IsOk())
        return MP_ERR_NOT_FOUND;
    value.assign(static_cast<const char *>(secret.GetData()), secret.GetSize());
    return MP_OK;
}

int32_t store_delete(const std::string &mod_id, const char *key)
{
    if (!valid_secret_key(key))
        return MP_ERR_ARGUMENT;
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return MP_ERR_FAILED;
    store.Delete(secret_service(mod_id, key));
    return MP_OK;
}

int32_t net_secret_put(void *svc, const char *key, const char *value)
{
    const NativeMod *mod = caller(svc);
    return mod == nullptr ? int32_t(MP_ERR_ARGUMENT) : store_put(mod->id(), key, value);
}

int32_t net_secret_get(void *svc, const char *key, char *buf, uint32_t cap)
{
    const NativeMod *mod = caller(svc);
    if (mod == nullptr || buf == nullptr || cap == 0)
        return MP_ERR_ARGUMENT;
    std::string   value;
    const int32_t rc = store_get(mod->id(), key, value);
    return rc == MP_OK ? copy_out(value, buf, cap) : rc;
}

int32_t net_secret_delete(void *svc, const char *key)
{
    const NativeMod *mod = caller(svc);
    return mod == nullptr ? int32_t(MP_ERR_ARGUMENT) : store_delete(mod->id(), key);
}

// ---------------------------------------------------------------------------------------------------------------
// The GUI thread and mod threads

int32_t net_queue_on_main(void *svc, void (*fn)(void *), void *user)
{
    const NativeMod *mod = caller(svc);
    if (mod == nullptr || fn == nullptr)
        return MP_ERR_ARGUMENT;
    if (g_shutting_down.load() || wxTheApp == nullptr)
        return MP_ERR_FAILED;
    wxTheApp->CallAfter([mod, fn, user]() {
        if (g_shutting_down.load())
            return;
        mod->guarded_call([&]() {
            fn(user);
            return int32_t(MP_OK);
        });
    });
    return MP_OK;
}

std::mutex                              g_threads_mutex;
std::map<uint64_t, std::pair<const NativeMod *, std::thread>> g_threads;
uint64_t                                g_next_thread = 1;

int32_t net_thread_start(void *svc, void (*fn)(void *), void *user, uint64_t *thread_id)
{
    const NativeMod *mod = caller(svc);
    if (mod == nullptr || fn == nullptr || thread_id == nullptr)
        return MP_ERR_ARGUMENT;
    if (g_shutting_down.load())
        return MP_ERR_FAILED;
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    const uint64_t id = g_next_thread++;
    // A crash inside fn disables the mod (guarded_call) and ends this thread instead of the application.
    g_threads.emplace(id, std::make_pair(mod, std::thread([mod, fn, user]() {
                                             mod->guarded_call([&]() {
                                                 fn(user);
                                                 return int32_t(MP_OK);
                                             });
                                         })));
    *thread_id = id;
    return MP_OK;
}

void net_thread_join(void *svc, uint64_t id)
{
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(g_threads_mutex);
        auto it = g_threads.find(id);
        if (it == g_threads.end() || it->second.first != caller(svc))
            return;
        t = std::move(it->second.second);
        g_threads.erase(it);
    }
    if (t.joinable() && t.get_id() != std::this_thread::get_id())
        t.join();
    else if (t.joinable())
        t.detach(); // a thread joining itself
}

const mp_net_v1 g_net = {sizeof(mp_net_v1), net_http,          net_free_response, net_tls_connect, net_tls_read,
                         net_tls_write,     net_tls_close,     net_secret_put,    net_secret_get,  net_secret_delete,
                         net_queue_on_main, net_thread_start,  net_thread_join};

} // namespace

void register_service()
{
#ifdef _WIN32
    // libcurl starts Winsock for its own use; the TLS sockets here may come first.
    WSADATA wsa;
    ::WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (AppConfig *config = GUI::wxGetApp().app_config; config != nullptr && config->has_section(k_consent_section)) {
        std::lock_guard<std::mutex> lock(g_consent_mutex);
        g_consent = config->get_section(k_consent_section);
    }
    ModHost::register_core_service(MP_NET_SERVICE, MP_NET_VERSION, &g_net, sizeof(g_net));
}

void shutdown()
{
    g_shutting_down.store(true);
    // Readers and writers see the flag within one wait (200 ms); closing every connection makes them return at once.
    {
        std::lock_guard<std::mutex> lock(g_conns_mutex);
        for (auto &[id, conn] : g_conns) {
            std::lock_guard<std::mutex> l(conn->ssl_mutex);
            conn->closed = true;
        }
        g_conns.clear();
    }
    std::map<uint64_t, std::pair<const NativeMod *, std::thread>> threads;
    {
        std::lock_guard<std::mutex> lock(g_threads_mutex);
        threads.swap(g_threads);
    }
    // A mod joins its own threads when its sessions end; these are the ones it left. Its core.net calls now fail: a
    // TLS read within one 200 ms wait, an HTTP request at its next progress callback or its own timeout.
    for (auto &[id, t] : threads)
        if (t.second.joinable())
            t.second.join();
}

bool has_consent(const Mod &mod)
{
    std::lock_guard<std::mutex> lock(g_consent_mutex);
    auto it = g_consent.find(mod.id);
    return it != g_consent.end() && it->second == ModHost::network_consent_key(mod);
}

void set_consent(const Mod &mod, bool agreed)
{
    const std::string key = ModHost::network_consent_key(mod);
    {
        std::lock_guard<std::mutex> lock(g_consent_mutex);
        if (agreed)
            g_consent[mod.id] = key;
        else
            g_consent.erase(mod.id);
    }
    if (AppConfig *config = GUI::wxGetApp().app_config; config != nullptr) {
        if (agreed)
            config->set(k_consent_section, mod.id, key);
        else
            config->erase(k_consent_section, mod.id);
    }
}

bool put_secret(const std::string &mod_id, const std::string &key, const std::string &value)
{
    return store_put(mod_id, key.c_str(), value.c_str()) == MP_OK;
}

bool has_secret(const std::string &mod_id, const std::string &key)
{
    std::string value;
    return store_get(mod_id, key.c_str(), value) == MP_OK;
}

void delete_secret(const std::string &mod_id, const std::string &key) { store_delete(mod_id, key.c_str()); }

std::string redact(const std::string &text)
{
    // Query and form values: ?s=..., &token=..., sign=..., password=..., key=..., did=...
    static const std::regex query(R"(([?&;](?:s|token|access_token|sign|signature|password|passwd|pwd|key|secret|did|nonce)=)[^&#\s"']*)",
                                  std::regex::icase);
    // JSON members: "token": "...", "password": "...", "devicepk": "..." and alike.
    static const std::regex member(
        R"#(("(?:token|access_token|local_token|password|passwd|username|secret|devicecrt|devicepk|info|sign|signature)"\s*:\s*")(?:[^"\\]|\\.)*")#",
        std::regex::icase);
    static const std::regex bearer(R"((Bearer\s+)[A-Za-z0-9._~+/=-]+)", std::regex::icase);
    std::string out = std::regex_replace(text, query, "$1***");
    out             = std::regex_replace(out, member, "$1***\"");
    return std::regex_replace(out, bearer, "$1***");
}

} // namespace ModNet
} // namespace Slic3r
