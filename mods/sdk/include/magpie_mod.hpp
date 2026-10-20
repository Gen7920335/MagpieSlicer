// magpie_mod.hpp -- header-only C++ helpers for writing a native mod. Compiled into the mod only;
// the core never includes it. Everything here sits on top of magpie_mod_api.h.
//
//   #include <magpie_mod.hpp>
//   MAGPIE_MOD_DEFINE("my_mod", "1.0.0")
//   int32_t magpie::mod_init(const MagpieHostApi &host, MagpieRegistrar &registrar) { ... return MP_OK; }
//   void magpie::mod_shutdown() {}
#pragma once

#include "magpie_mod_api.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace magpie {

// The host table the core handed to magpie_mod_init, valid until magpie_mod_shutdown returns.
inline const MagpieHostApi *&host()
{
    static const MagpieHostApi *h = nullptr;
    return h;
}

// Whether the host table carries `member` (a newer mod on an older core does not get later entries).
#define MAGPIE_HOST_HAS(h, member) (offsetof(MagpieHostApi, member) + sizeof(((MagpieHostApi *) 0)->member) <= (h)->size)

// Polygons owned by the mod, convertible to the borrowed C view.
using Ring    = std::vector<mp_point>;
using ExPoly  = std::vector<Ring>; // [0] contour, others holes
using ExPolys = std::vector<ExPoly>;

// A C view of `polys`. Valid while `polys` and the returned holder live.
class ExPolysView
{
public:
    explicit ExPolysView(const ExPolys &polys)
    {
        m_rings.reserve(polys.size());
        for (const ExPoly &p : polys) {
            std::vector<mp_ring> rings;
            for (const Ring &r : p)
                rings.push_back({r.data(), uint32_t(r.size())});
            m_rings.push_back(std::move(rings));
        }
        for (const std::vector<mp_ring> &rings : m_rings)
            m_items.push_back({rings.data(), uint32_t(rings.size())});
        m_view = {m_items.data(), uint32_t(m_items.size())};
    }
    const mp_expolys *get() const { return &m_view; }

private:
    std::vector<std::vector<mp_ring>> m_rings;
    std::vector<mp_expoly>            m_items;
    mp_expolys                        m_view{};
};

inline ExPolys to_expolys(const mp_expolys &in)
{
    ExPolys out;
    out.reserve(in.n);
    for (uint32_t i = 0; i < in.n; ++i) {
        ExPoly p;
        for (uint32_t r = 0; r < in.items[i].n; ++r) {
            const mp_ring &ring = in.items[i].rings[r];
            p.emplace_back(ring.pts, ring.pts + ring.n);
        }
        out.push_back(std::move(p));
    }
    return out;
}

// Offsets polygons with the host's Clipper; delta in integer units.
inline ExPolys offset(const ExPolys &in, double delta_scaled, int32_t join = MP_JOIN_MITER)
{
    ExPolysView view(in);
    mp_expolys  out{};
    if (host()->offset(view.get(), delta_scaled, join, &out) != MP_OK)
        throw std::runtime_error("host offset failed");
    ExPolys result = to_expolys(out);
    host()->free_expolys(&out);
    return result;
}

// Parameter values of this mod in the context `ctx`; nothing when unset or of another type.
inline std::optional<double> param_f64(mp_ctx *ctx, const char *key)
{
    double v = 0.;
    return host()->param_f64(ctx, key, &v) == MP_OK ? std::optional<double>(v) : std::nullopt;
}
inline std::optional<int64_t> param_i64(mp_ctx *ctx, const char *key)
{
    int64_t v = 0;
    return host()->param_i64(ctx, key, &v) == MP_OK ? std::optional<int64_t>(v) : std::nullopt;
}
inline std::optional<std::string> param_str(mp_ctx *ctx, const char *key)
{
    std::vector<char> buf(256);
    for (;;) {
        const int rc = host()->param_str(ctx, key, buf.data(), uint32_t(buf.size()));
        if (rc == MP_OK)
            return std::string(buf.data());
        if (rc != MP_ERR_BUFFER || buf.size() > (1u << 24))
            return std::nullopt;
        buf.resize(buf.size() * 4);
    }
}

inline void log(int32_t level, const char *mod_id, const std::string &text) { host()->log(level, mod_id, text.c_str()); }

// Runs `fn` and turns any exception into a status, so no exception crosses into the core. The
// message lives in a thread-local buffer until the next failing call on the same thread.
template<class Fn> mp_status guarded(Fn &&fn) noexcept
{
    static thread_local std::string message;
    try {
        fn();
        return {MP_OK, nullptr};
    } catch (const std::bad_alloc &) {
        message = "out of memory";
    } catch (const std::exception &e) {
        message = e.what();
    } catch (...) {
        message = "unknown exception";
    }
    return {MP_ERR_FAILED, message.c_str()};
}

// Defined by the mod.
int32_t mod_init(const MagpieHostApi &host, MagpieRegistrar &registrar);
void    mod_shutdown();

} // namespace magpie

// Defines the three exports. The mod defines magpie::mod_init and magpie::mod_shutdown.
#define MAGPIE_MOD_DEFINE(ID, VERSION)                                                                  \
    extern "C" MAGPIE_EXPORT const char *magpie_mod_query(void) { return MAGPIE_MOD_QUERY_STRING(ID, VERSION); } \
    extern "C" MAGPIE_EXPORT int32_t magpie_mod_init(const MagpieHostApi *host, MagpieRegistrar *registrar) \
    {                                                                                                     \
        if (host == nullptr || registrar == nullptr || host->major != MAGPIE_MOD_API_MAJOR)                \
            return MP_ERR_ARGUMENT;                                                                       \
        magpie::host() = host;                                                                            \
        int32_t rc = MP_ERR_FAILED;                                                                       \
        const mp_status st = magpie::guarded([&] { rc = magpie::mod_init(*host, *registrar); });          \
        if (st.code != MP_OK) {                                                                           \
            host->log(MP_LOG_ERROR, ID, st.message);                                                      \
            return st.code;                                                                               \
        }                                                                                                 \
        return rc;                                                                                        \
    }                                                                                                     \
    extern "C" MAGPIE_EXPORT void magpie_mod_shutdown(void)                                              \
    {                                                                                                     \
        magpie::guarded([] { magpie::mod_shutdown(); });                                                  \
        magpie::host() = nullptr;                                                                         \
    }
