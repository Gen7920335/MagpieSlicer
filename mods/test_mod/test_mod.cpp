// The empty test mod: loads, fills the test slot "test.math" and offers the service "<id>.echo", so
// the core's loader, ABI checks, crash guard and host table can be tested without a real feature.
// Built three times (mods/CMakeLists.txt): as is, claiming API major 2, and claiming a fast
// floating-point mode; the core must load the first and refuse the other two.
#include <magpie_mod.hpp>

#include "test_slot.h"

#include <stdexcept>

#ifndef MAGPIE_TEST_MOD_ID
#error "MAGPIE_TEST_MOD_ID must name the mod"
#endif

namespace {

mp_status add(void *, int64_t a, int64_t b, int64_t *out)
{
    *out = a + b;
    return {MP_OK, nullptr};
}

mp_status crash(void *)
{
    volatile int *null_pointer = nullptr;
    *null_pointer = 1;
    return {MP_OK, nullptr};
}

mp_status param(void *, mp_ctx *ctx, const char *key, double *out)
{
    const int rc = magpie::host()->param_f64(ctx, key, out);
    return {rc, rc == MP_OK ? nullptr : "param_f64 failed"};
}

mp_status grow(void *, const mp_expolys *in, double delta_mm, mp_expolys *out)
{
    // The integer unit is read from the host at each call, never assumed to be 1e-6 mm.
    const double delta_scaled = delta_mm / magpie::host()->scaling_factor_mm;
    const int    rc           = magpie::host()->offset(in, delta_scaled, MP_JOIN_MITER, out);
    return {rc, rc == MP_OK ? nullptr : "offset failed"};
}

mp_status throws(void *)
{
    return magpie::guarded([] { throw std::runtime_error("thrown inside the test mod"); });
}

const test_math_v1 math_table = {sizeof(test_math_v1), add, crash, param, grow, throws};

struct echo_v1
{
    uint32_t size;
    int64_t (*echo)(int64_t v);
};
int64_t      echo(int64_t v) { return v; }
const echo_v1 echo_table = {sizeof(echo_v1), echo};

} // namespace

int32_t magpie::mod_init(const MagpieHostApi &host, MagpieRegistrar &registrar)
{
    if (int rc = registrar.register_slot(&registrar, "test.math", 1, "test", &math_table, sizeof(math_table), nullptr); rc != MP_OK)
        return rc;
    host.log(MP_LOG_INFO, MAGPIE_TEST_MOD_ID, "test mod initialised");
    return registrar.export_service(&registrar, MAGPIE_TEST_MOD_ID ".echo", 1, &echo_table, sizeof(echo_table), nullptr);
}

void magpie::mod_shutdown() {}

#if defined(MAGPIE_TEST_MOD_FAKE_MAJOR)
// Claims another API major: the core must refuse it before calling init.
extern "C" MAGPIE_EXPORT const char *magpie_mod_query(void) { return MAGPIE_TEST_MOD_ID " 1.0.0 2 0 fp=precise"; }
extern "C" MAGPIE_EXPORT int32_t magpie_mod_init(const MagpieHostApi *, MagpieRegistrar *) { return MP_ERR_FAILED; }
extern "C" MAGPIE_EXPORT void magpie_mod_shutdown(void) {}
#elif defined(MAGPIE_TEST_MOD_FAKE_FP)
// Claims fast floating point: the core must refuse it, as its G-code would not be reproducible.
extern "C" MAGPIE_EXPORT const char *magpie_mod_query(void) { return MAGPIE_TEST_MOD_ID " 1.0.0 1 0 fp=fast"; }
extern "C" MAGPIE_EXPORT int32_t magpie_mod_init(const MagpieHostApi *, MagpieRegistrar *) { return MP_ERR_FAILED; }
extern "C" MAGPIE_EXPORT void magpie_mod_shutdown(void) {}
#else
MAGPIE_MOD_DEFINE(MAGPIE_TEST_MOD_ID, "1.0.0")
#endif
