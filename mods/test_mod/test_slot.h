/* The slot the test mods fill ("test.math" v1). Shared by mods/test_mod/test_mod.cpp and the core's
 * unit tests (tests/libslic3r/test_native_mods.cpp); no core code calls it. */
#ifndef MAGPIE_TEST_SLOT_H
#define MAGPIE_TEST_SLOT_H

#include "../sdk/include/magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct test_math_v1 {
    uint32_t size;
    /* *out = a + b */
    mp_status (*add)(void *user, int64_t a, int64_t b, int64_t *out);
    /* Dereferences a null pointer: the core's crash guard is tested with it. */
    mp_status (*crash)(void *user);
    /* *out = this mod's parameter `key` in the context, through host->param_f64. */
    mp_status (*param)(void *user, mp_ctx *ctx, const char *key, double *out);
    /* *out = `in` grown by delta_mm millimetres, converted with host->scaling_factor_mm. */
    mp_status (*grow)(void *user, const mp_expolys *in, double delta_mm, mp_expolys *out);
    /* Throws a C++ exception inside the mod; the SDK wrapper turns it into a status. */
    mp_status (*throws)(void *user);
} test_math_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_TEST_SLOT_H */
