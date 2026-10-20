// The test accelerator (slot accel.batch): computes facet layer ranges with its own statement of the
// rule and declines every other batch. With its parameter "wrong" set, the last item of each batch is
// off by one layer, so the core's CPU check can be tested without a GPU.
#include <magpie_mod.hpp>
#include <magpie_accel.h>

#include <atomic>
#include <string>

MAGPIE_MOD_DEFINE("magpie_test_accel", "1.0.0")

namespace {

std::atomic<bool> g_wrong{false};

mp_status begin(void *, mp_ctx *ctx, int32_t *mode)
{
    return magpie::guarded([&] {
        const std::string value = magpie::param_str(ctx, "mode").value_or("off");
        *mode = value == "on" ? MP_ACCEL_VERIFY : value == "max" ? MP_ACCEL_TRUSTED : MP_ACCEL_OFF;
        g_wrong = magpie::param_f64(ctx, "wrong").value_or(0.) != 0.;
    });
}

void    end(void *) {}
int32_t accepts(void *, uint32_t op, uint64_t items) { return op == MP_ACCEL_OP_FACET_LAYER_RANGES && items > 0 ? 1 : 0; }
int32_t available(void *, char *, uint32_t) { return 1; }

mp_status facet_layer_ranges(void *, const float *facet_z, uint64_t n, const float *zs, uint64_t n_zs, uint32_t *out)
{
    for (uint64_t i = 0; i < n; ++i) {
        const float *z  = facet_z + 3 * i;
        const float  lo = z[0] < z[1] ? (z[0] < z[2] ? z[0] : z[2]) : (z[1] < z[2] ? z[1] : z[2]);
        const float  hi = z[0] > z[1] ? (z[0] > z[2] ? z[0] : z[2]) : (z[1] > z[2] ? z[1] : z[2]);
        uint32_t first = 0;
        while (first < n_zs && zs[first] < lo)
            ++first;
        uint32_t last = first;
        while (last < n_zs && !(hi < zs[last]))
            ++last;
        out[3 * i]     = first;
        out[3 * i + 1] = last;
        out[3 * i + 2] = z[1] == lo ? 1 : (z[2] == lo ? 2 : 0);
    }
    if (g_wrong && n > 0)
        ++out[3 * (n - 1) + 1];
    return {MP_OK, nullptr};
}

mp_status declined3(void *, const int64_t *, uint64_t, int64_t *, uint32_t *) { return {MP_ERR_FAILED, "declined"}; }
mp_status declined4(void *, const mp_point *, uint64_t, const mp_point *, uint64_t, double *) { return {MP_ERR_FAILED, "declined"}; }
mp_status declined5(void *, const mp_point *, uint64_t, const mp_expolys *, uint8_t *) { return {MP_ERR_FAILED, "declined"}; }

const mp_accel_batch_v1 table = {sizeof(mp_accel_batch_v1), begin, end, accepts, available, facet_layer_ranges, declined3, declined4, declined5};

} // namespace

int32_t magpie::mod_init(const MagpieHostApi &, MagpieRegistrar &registrar)
{
    return registrar.register_slot(&registrar, MP_ACCEL_SLOT, MP_ACCEL_SLOT_VERSION, "test", &table, sizeof(table), nullptr);
}

void magpie::mod_shutdown() {}
