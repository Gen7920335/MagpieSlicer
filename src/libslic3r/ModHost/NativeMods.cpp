#include "NativeMods.hpp"
#include "SlotChoice.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r_version.h"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <tbb/parallel_for.h>

#include <atomic>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <malloc.h>
#else
#include <dlfcn.h>
#endif

extern "C" int32_t magpie_native_guarded_call(int32_t (*fn)(void *), void *context, uint32_t *code_out);
extern "C" void    magpie_native_raise(uint32_t code);

namespace Slic3r {
namespace ModHost {

// The host hands the core's own polygons to mods without copying: a Point is two int64_t in a row,
// exactly an mp_point.
static_assert(sizeof(Point) == sizeof(mp_point), "Point must have mp_point's layout");
static_assert(sizeof(coord_t) == sizeof(int64_t), "coordinates must be 64-bit for mp_point");
static_assert(std::is_trivially_copyable<mp_point>::value, "mp_point must be plain data");

namespace {

std::mutex g_active_native_mutex;
// Never destroyed: at process exit the mod libraries may already be detached, so a destructor calling
// magpie_mod_shutdown then could crash. The application calls shutdown_native_mods() while it can.
std::shared_ptr<const NativeModSet> &g_active_native = *new std::shared_ptr<const NativeModSet>();

// ---------------------------------------------------------------------------------------------------
// Libraries

void *open_library(const std::string &path, std::string &error)
{
#ifdef _WIN32
    const std::wstring wide = boost::nowide::widen(path);
    // Dependencies resolve from the mod's own folder and System32 only, never from the current
    // directory or PATH, so a planted DLL next to a project file cannot be picked up.
    HMODULE module = ::LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr)
        error = "the library could not be loaded (Windows error " + std::to_string(::GetLastError()) + ")";
    return module;
#else
    void *handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char *e = ::dlerror();
        error = std::string("the library could not be loaded: ") + (e ? e : "unknown error");
    }
    return handle;
#endif
}

void *library_symbol(void *library, const char *name)
{
#ifdef _WIN32
    return reinterpret_cast<void *>(::GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return ::dlsym(library, name);
#endif
}

void close_library(void *library)
{
    if (library == nullptr)
        return;
#ifdef _WIN32
    ::FreeLibrary(static_cast<HMODULE>(library));
#else
    ::dlclose(library);
#endif
}

// ---------------------------------------------------------------------------------------------------
// Guarded calls

struct GuardedCall
{
    const std::function<int32_t()> *call;
};

int32_t run_guarded_call(void *context) { return (*static_cast<GuardedCall *>(context)->call)(); }

std::string exception_name(uint32_t code)
{
#ifdef _WIN32
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case 0xE06D7363u: return "C++ exception thrown across the mod boundary";
    default: break;
    }
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "exception 0x%08X", code);
    return buf;
}

// ---------------------------------------------------------------------------------------------------
// Host table

thread_local std::string t_param_key; // reused buffer for "<mod id>.<name>"

// Splits "name[3]" into ("name", 3); "name" into ("name", nothing). False when malformed.
bool split_param_key(const char *key, std::string &name, std::optional<size_t> &index)
{
    if (key == nullptr)
        return false;
    const std::string k(key);
    const size_t      open = k.find('[');
    if (open == std::string::npos) {
        name = k;
        index.reset();
        return !name.empty();
    }
    if (k.back() != ']' || open == 0)
        return false;
    const std::string digits = k.substr(open + 1, k.size() - open - 2);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos)
        return false;
    name  = k.substr(0, open);
    index = size_t(std::strtoull(digits.c_str(), nullptr, 10));
    return true;
}

const std::string *string_option(const ConfigBase *config, const char *key)
{
    if (config == nullptr)
        return nullptr;
    const auto *opt = dynamic_cast<const ConfigOptionString *>(config->option(key));
    return opt ? &opt->value : nullptr;
}

int param_lookup(mp_ctx *ctx, const char *key, ExprValue &out, std::string *type = nullptr)
{
    if (ctx == nullptr || ctx->mod == nullptr)
        return MP_ERR_ARGUMENT;
    std::string           name;
    std::optional<size_t> index;
    if (!split_param_key(key, name, index))
        return MP_ERR_ARGUMENT;
    std::shared_ptr<const ModSet> mods = active_mods();
    t_param_key = ctx->mod->id() + "." + name;
    const ModParam *param = mods ? mods->find_param(t_param_key) : nullptr;
    if (param == nullptr)
        return MP_ERR_NOT_FOUND;
    ModSet::ValueSources sources;
    sources.print         = ctx->print_config;
    sources.object_values = string_option(ctx->object_config, "object_mod_settings");
    sources.region_values = string_option(ctx->region_config, "region_mod_settings");
    std::optional<ExprValue> v = mods->value_of(t_param_key, sources, index);
    if (!v)
        return (param->scope == "filament") != index.has_value() ? MP_ERR_ARGUMENT : MP_ERR_TYPE;
    out = *v;
    if (type)
        *type = param->type;
    return MP_OK;
}

int copy_string(const std::string &value, char *buf, uint32_t cap)
{
    if (buf == nullptr || cap == 0)
        return MP_ERR_ARGUMENT;
    const size_t n = std::min<size_t>(value.size(), cap - 1);
    std::memcpy(buf, value.data(), n);
    buf[n] = '\0';
    return n < value.size() ? MP_ERR_BUFFER : MP_OK;
}

int host_param_f64(mp_ctx *ctx, const char *key, double *out)
{
    if (out == nullptr)
        return MP_ERR_ARGUMENT;
    ExprValue   v;
    std::string type;
    if (int rc = param_lookup(ctx, key, v, &type); rc != MP_OK)
        return rc;
    if (const double *d = std::get_if<double>(&v); d && type != "float_or_percent") {
        *out = *d;
        return MP_OK;
    }
    if (const bool *b = std::get_if<bool>(&v)) {
        *out = *b ? 1. : 0.;
        return MP_OK;
    }
    return MP_ERR_TYPE;
}

int host_param_i64(mp_ctx *ctx, const char *key, int64_t *out)
{
    if (out == nullptr)
        return MP_ERR_ARGUMENT;
    ExprValue   v;
    std::string type;
    if (int rc = param_lookup(ctx, key, v, &type); rc != MP_OK)
        return rc;
    if (const double *d = std::get_if<double>(&v); d && type == "int") {
        *out = int64_t(*d);
        return MP_OK;
    }
    if (const bool *b = std::get_if<bool>(&v)) {
        *out = *b ? 1 : 0;
        return MP_OK;
    }
    return MP_ERR_TYPE;
}

int host_param_str(mp_ctx *ctx, const char *key, char *buf, uint32_t cap)
{
    ExprValue   v;
    std::string type;
    if (int rc = param_lookup(ctx, key, v, &type); rc != MP_OK)
        return rc;
    if (const std::string *s = std::get_if<std::string>(&v))
        return copy_string(*s, buf, cap);
    if (const double *d = std::get_if<double>(&v); d && type == "float_or_percent")
        return copy_string(expr_to_string(*d), buf, cap);
    return MP_ERR_TYPE;
}

// The option `key` as the context sees it: the region's, else the object's, else the print's.
const ConfigOption *context_option(mp_ctx *ctx, const char *key)
{
    if (ctx == nullptr || key == nullptr)
        return nullptr;
    for (const ConfigBase *config : {ctx->region_config, ctx->object_config, static_cast<const ConfigBase *>(ctx->print_config)})
        if (config != nullptr)
            if (const ConfigOption *opt = config->option(key))
                return opt;
    return nullptr;
}

int host_config_f64(mp_ctx *ctx, const char *key, int32_t index, double *out)
{
    const ConfigOption *opt = context_option(ctx, key);
    if (opt == nullptr || out == nullptr)
        return opt == nullptr ? MP_ERR_NOT_FOUND : MP_ERR_ARGUMENT;
    if (!opt->is_vector()) {
        if (index != -1)
            return MP_ERR_ARGUMENT;
        switch (opt->type()) {
        case coFloat: case coPercent: case coFloatOrPercent: *out = static_cast<const ConfigOptionFloat *>(opt)->value; return MP_OK;
        case coInt: *out = double(static_cast<const ConfigOptionInt *>(opt)->value); return MP_OK;
        case coBool: *out = static_cast<const ConfigOptionBool *>(opt)->value ? 1. : 0.; return MP_OK;
        case coEnum: *out = double(opt->getInt()); return MP_OK;
        default: return MP_ERR_TYPE;
        }
    }
    const auto *vec = static_cast<const ConfigOptionVectorBase *>(opt);
    if (index < 0 || size_t(index) >= vec->size())
        return MP_ERR_ARGUMENT;
    const size_t i = size_t(index);
    if (auto *v = dynamic_cast<const ConfigOptionFloats *>(opt)) { *out = v->values[i]; return MP_OK; }
    if (auto *v = dynamic_cast<const ConfigOptionFloatsNullable *>(opt)) { *out = v->values[i]; return MP_OK; }
    if (auto *v = dynamic_cast<const ConfigOptionFloatsOrPercents *>(opt)) { *out = v->values[i].value; return MP_OK; }
    if (auto *v = dynamic_cast<const ConfigOptionFloatsOrPercentsNullable *>(opt)) { *out = v->values[i].value; return MP_OK; }
    if (auto *v = dynamic_cast<const ConfigOptionInts *>(opt)) { *out = double(v->values[i]); return MP_OK; }
    if (auto *v = dynamic_cast<const ConfigOptionIntsNullable *>(opt)) { *out = double(v->values[i]); return MP_OK; }
    if (auto *v = dynamic_cast<const ConfigOptionBools *>(opt)) { *out = v->values[i] ? 1. : 0.; return MP_OK; }
    if (auto *v = dynamic_cast<const ConfigOptionBoolsNullable *>(opt)) { *out = v->values[i] ? 1. : 0.; return MP_OK; }
    return MP_ERR_TYPE;
}

int host_config_str(mp_ctx *ctx, const char *key, int32_t index, char *buf, uint32_t cap)
{
    // "support_generator" is no setting any more: the host gives the support implementation the active mods pick for
    // the context's object ("" for Orca's own), which a mod reads to know whether the object's support is its own.
    if (ctx != nullptr && key != nullptr && std::strcmp(key, "support_generator") == 0 && ctx->print_config != nullptr) {
        if (index != -1)
            return MP_ERR_ARGUMENT;
        try {
            return copy_string(ctx->object_config != nullptr ? support_generator_name(*ctx->print_config, *ctx->object_config) : std::string(),
                               buf, cap);
        } catch (const std::exception &) {
            return copy_string(std::string(), buf, cap);
        }
    }
    const ConfigOption *opt = context_option(ctx, key);
    if (opt == nullptr)
        return MP_ERR_NOT_FOUND;
    if (!opt->is_vector()) {
        if (index != -1)
            return MP_ERR_ARGUMENT;
        // A text setting as it is (a start G-code with its line breaks), not in its escaped file form.
        if (const auto *text = dynamic_cast<const ConfigOptionString *>(opt))
            return copy_string(text->value, buf, cap);
        return copy_string(opt->serialize(), buf, cap);
    }
    const std::vector<std::string> items = static_cast<const ConfigOptionVectorBase *>(opt)->vserialize();
    if (index < 0 || size_t(index) >= items.size())
        return MP_ERR_ARGUMENT;
    return copy_string(items[size_t(index)], buf, cap);
}

ExPolygons to_expolygons(const mp_expolys *in)
{
    ExPolygons out;
    if (in == nullptr)
        return out;
    out.reserve(in->n);
    for (uint32_t i = 0; i < in->n; ++i) {
        const mp_expoly &e = in->items[i];
        if (e.n == 0)
            continue;
        ExPolygon ex;
        for (uint32_t r = 0; r < e.n; ++r) {
            Polygon poly;
            poly.points.reserve(e.rings[r].n);
            for (uint32_t k = 0; k < e.rings[r].n; ++k)
                poly.points.emplace_back(e.rings[r].pts[k].x, e.rings[r].pts[k].y);
            if (r == 0)
                ex.contour = std::move(poly);
            else
                ex.holes.push_back(std::move(poly));
        }
        out.push_back(std::move(ex));
    }
    return out;
}

// Copies `polys` into host-allocated arrays that host_free_expolys releases.
void from_expolygons(const ExPolygons &polys, mp_expolys *out)
{
    auto *items = static_cast<mp_expoly *>(std::calloc(std::max<size_t>(polys.size(), 1), sizeof(mp_expoly)));
    for (size_t i = 0; i < polys.size(); ++i) {
        const ExPolygon &ex    = polys[i];
        const size_t     n     = 1 + ex.holes.size();
        auto            *rings = static_cast<mp_ring *>(std::calloc(n, sizeof(mp_ring)));
        for (size_t r = 0; r < n; ++r) {
            const Polygon &poly = r == 0 ? ex.contour : ex.holes[r - 1];
            auto          *pts  = static_cast<mp_point *>(std::malloc(std::max<size_t>(poly.points.size(), 1) * sizeof(mp_point)));
            for (size_t k = 0; k < poly.points.size(); ++k)
                pts[k] = {poly.points[k].x(), poly.points[k].y()};
            rings[r] = {pts, uint32_t(poly.points.size())};
        }
        items[i] = {rings, uint32_t(n)};
    }
    *out = {items, uint32_t(polys.size())};
}

void host_free_expolys(mp_expolys *polys)
{
    if (polys == nullptr || polys->items == nullptr)
        return;
    for (uint32_t i = 0; i < polys->n; ++i) {
        for (uint32_t r = 0; r < polys->items[i].n; ++r)
            std::free(const_cast<mp_point *>(polys->items[i].rings[r].pts));
        std::free(const_cast<mp_ring *>(polys->items[i].rings));
    }
    std::free(const_cast<mp_expoly *>(polys->items));
    *polys = {nullptr, 0};
}

int host_offset(const mp_expolys *in, double delta_scaled, int32_t join, mp_expolys *out)
{
    if (in == nullptr || out == nullptr)
        return MP_ERR_ARGUMENT;
    const JoinType type = join == MP_JOIN_ROUND ? jtRound : join == MP_JOIN_SQUARE ? jtSquare : jtMiter;
    try {
        from_expolygons(offset_ex(to_expolygons(in), float(delta_scaled), type), out);
    } catch (const std::exception &) {
        return MP_ERR_FAILED;
    }
    return MP_OK;
}

int host_boolean(int32_t op, const mp_expolys *a, const mp_expolys *b, mp_expolys *out)
{
    if (a == nullptr || out == nullptr || (b == nullptr && op != MP_OP_UNION))
        return MP_ERR_ARGUMENT;
    try {
        ExPolygons pa = to_expolygons(a), pb = to_expolygons(b);
        switch (op) {
        case MP_OP_UNION: append(pa, std::move(pb)); from_expolygons(union_ex(pa), out); return MP_OK;
        case MP_OP_INTERSECTION: from_expolygons(intersection_ex(pa, pb), out); return MP_OK;
        case MP_OP_DIFFERENCE: from_expolygons(diff_ex(pa, pb), out); return MP_OK;
        case MP_OP_XOR: from_expolygons(xor_ex(pa, pb), out); return MP_OK;
        default: return MP_ERR_ARGUMENT;
        }
    } catch (const std::exception &) {
        return MP_ERR_FAILED;
    }
}

void *host_alloc(size_t bytes) { return std::malloc(bytes == 0 ? 1 : bytes); }
void  host_free(void *ptr) { std::free(ptr); }

void host_log(int32_t level, const char *mod_id, const char *text)
{
    const std::string line = std::string("Mod ") + (mod_id ? mod_id : "?") + ": " + (text ? text : "");
    switch (level) {
    case MP_LOG_DEBUG: BOOST_LOG_TRIVIAL(debug) << line; break;
    case MP_LOG_INFO: BOOST_LOG_TRIVIAL(info) << line; break;
    case MP_LOG_WARNING: BOOST_LOG_TRIVIAL(warning) << line; break;
    default: BOOST_LOG_TRIVIAL(error) << line; break;
    }
}

void host_warn(mp_ctx *ctx, int32_t level, const char *text)
{
    if (ctx == nullptr)
        return;
    std::lock_guard<std::mutex> lock(ctx->warnings_mutex);
    ctx->warnings.emplace_back(level, text ? text : "");
}

int host_cancelled(mp_ctx *ctx) { return ctx != nullptr && ctx->cancelled && ctx->cancelled() ? 1 : 0; }

struct ParallelForCall
{
    void (*fn)(void *user, uint32_t i);
    void    *user;
    uint32_t i;
};
int32_t run_parallel_for_call(void *context)
{
    const ParallelForCall &c = *static_cast<ParallelForCall *>(context);
    c.fn(c.user, c.i);
    return 0;
}

void host_parallel_for(mp_ctx * /* ctx */, uint32_t n, void (*fn)(void *user, uint32_t i), void *user)
{
    if (fn == nullptr)
        return;
    // Each worker runs under the same guard as the calling thread; a fault there is raised again on the calling
    // thread once the loop is over, where the mod's guard turns it into a failure of the call.
    std::atomic<uint32_t> fault{0};
    tbb::parallel_for(uint32_t(0), n, [fn, user, &fault](uint32_t i) {
        if (fault.load() != 0)
            return;
        ParallelForCall call{fn, user, i};
        uint32_t        code = 0;
        if (magpie_native_guarded_call(run_parallel_for_call, &call, &code) == INT32_MIN)
            fault.store(code != 0 ? code : 1u);
    });
    if (const uint32_t code = fault.load(); code != 0)
        magpie_native_raise(code);
}

MagpieHostApi make_host_api()
{
    MagpieHostApi h{};
    h.size              = sizeof(MagpieHostApi);
    h.major             = MAGPIE_MOD_API_MAJOR;
    h.minor             = MAGPIE_MOD_API_MINOR;
    h.scaling_factor_mm = SCALING_FACTOR;
    h.app_version       = SoftFever_VERSION;
    h.param_f64         = host_param_f64;
    h.param_i64         = host_param_i64;
    h.param_str         = host_param_str;
    h.config_f64        = host_config_f64;
    h.config_str        = host_config_str;
    h.offset            = host_offset;
    h.boolean           = host_boolean;
    h.free_expolys      = host_free_expolys;
    h.alloc             = host_alloc;
    h.free              = host_free;
    h.log               = host_log;
    h.warn              = host_warn;
    h.cancelled         = host_cancelled;
    h.parallel_for      = host_parallel_for;
    return h;
}

MagpieHostApi g_host_api = make_host_api();

} // namespace

const MagpieHostApi &native_host_api()
{
    // The application switches SCALING_FACTOR for very large beds, outside slicing, and calls this to
    // refresh the table; mods read the field in each call.
    g_host_api.scaling_factor_mm = SCALING_FACTOR;
    return g_host_api;
}

// ---------------------------------------------------------------------------------------------------
// NativeMod

NativeMod::~NativeMod() { close_library(m_library); }

int32_t NativeMod::guarded_call(const std::function<int32_t()> &call, std::string *message) const
{
    if (m_crashed.load()) {
        if (message)
            *message = "mod '" + id() + "' is disabled for this run after it crashed";
        return MP_ERR_FAILED;
    }
    std::unique_lock<std::mutex> lock(m_call_mutex, std::defer_lock);
    if (!m_manifest.thread_safe)
        lock.lock();
    GuardedCall context{&call};
    uint32_t    code   = 0;
    const int32_t rc   = magpie_native_guarded_call(run_guarded_call, &context, &code);
    if (rc != INT32_MIN || code == 0)
        return rc;
#ifdef _WIN32
    if (code == EXCEPTION_STACK_OVERFLOW)
        _resetstkoflw();
#endif
    // PERMANENT DIAGNOSTIC: which mod crashed and how, in the log and for the slicing error. The state
    // of the process after a crash is not trusted, so the mod is not called again in this run.
    {
        std::lock_guard<std::mutex> crash_lock(m_crash_mutex);
        m_crash_report = "mod '" + id() + "' " + m_manifest.version + " crashed: " + exception_name(code) +
                         ". It is disabled until the application restarts; restart before slicing again.";
    }
    m_crashed.store(true);
    BOOST_LOG_TRIVIAL(error) << m_crash_report << " (library " << m_loaded_path << ")";
    if (message)
        *message = m_crash_report;
    return MP_ERR_FAILED;
}

// ---------------------------------------------------------------------------------------------------
// Loading

struct NativeLoader
{
    struct State
    {
        NativeModSet *set = nullptr;
        NativeMod    *mod = nullptr;
        std::string   error;
    };

    static State &state_of(MagpieRegistrar *self) { return *static_cast<State *>(self->host_data); }

    static int register_slot(MagpieRegistrar *self, const char *slot, uint32_t slot_version, const char *impl_name,
                             const void *table, uint32_t table_size, void *user)
    {
        if (self == nullptr || slot == nullptr || impl_name == nullptr || table == nullptr || table_size < sizeof(uint32_t))
            return MP_ERR_ARGUMENT;
        State &st = state_of(self);
        // A mod fills only the slots its manifest declares, so what a mod does is visible before it runs.
        const auto &decls = st.mod->m_manifest.slots;
        if (std::none_of(decls.begin(), decls.end(), [&](const ModSlotDecl &d) {
                return d.slot == slot && d.impl == impl_name && d.version == slot_version;
            })) {
            st.error = std::string("registered slot ") + slot + " v" + std::to_string(slot_version) + " \"" + impl_name +
                       "\", which its mod.json does not declare";
            return MP_ERR_ARGUMENT;
        }
        for (const NativeEntry &e : st.set->m_slots)
            if (e.name == slot && e.impl == impl_name) {
                st.error = std::string("slot ") + slot + " \"" + impl_name + "\" is already filled by mod '" + e.mod->id() + "'";
                return MP_ERR_ARGUMENT;
            }
        st.set->m_slots.push_back({st.mod, slot, impl_name, slot_version, table, table_size, user});
        return MP_OK;
    }

    static int export_service(MagpieRegistrar *self, const char *name, uint32_t version, const void *table, uint32_t table_size, void *user)
    {
        if (self == nullptr || name == nullptr || table == nullptr || table_size < sizeof(uint32_t))
            return MP_ERR_ARGUMENT;
        State &st = state_of(self);
        if (core_service(name, 0, nullptr) != nullptr) {
            st.error = std::string("service ") + name + " is the core's own";
            return MP_ERR_ARGUMENT;
        }
        for (const NativeEntry &e : st.set->m_services)
            if (e.name == name) {
                st.error = std::string("service ") + name + " is already offered by mod '" + e.mod->id() + "'";
                return MP_ERR_ARGUMENT;
            }
        st.set->m_services.push_back({st.mod, name, std::string(), version, table, table_size, user});
        return MP_OK;
    }

    static const void *import_service(MagpieRegistrar *self, const char *name, uint32_t min_version, uint32_t *table_size, void **user)
    {
        if (self == nullptr || name == nullptr)
            return nullptr;
        // The core's own services first: no mod can stand in for them.
        if (const void *table = core_service(name, min_version, table_size)) {
            // The importing mod itself, so a service knows its caller (core.net checks the caller's "network").
            if (user)
                *user = static_cast<void *>(state_of(self).mod);
            return table;
        }
        const NativeEntry *e = state_of(self).set->find_service(name, min_version);
        if (e == nullptr)
            return nullptr;
        if (table_size)
            *table_size = e->table_size;
        if (user)
            *user = e->user;
        return e->table;
    }
};

bool parse_native_query(const std::string &text, NativeQuery &out)
{
    std::istringstream in(text);
    std::string        major, minor, fp;
    if (!(in >> out.id >> out.version >> major >> minor >> fp))
        return false;
    if (major.find_first_not_of("0123456789") != std::string::npos || minor.find_first_not_of("0123456789") != std::string::npos ||
        fp.compare(0, 3, "fp=") != 0)
        return false;
    out.major   = uint32_t(std::strtoul(major.c_str(), nullptr, 10));
    out.minor   = uint32_t(std::strtoul(minor.c_str(), nullptr, 10));
    out.fp_mode = fp.substr(3);
    return true;
}

std::string native_query_problem(const NativeQuery &query, const Mod &manifest)
{
    if (query.id != manifest.id)
        return "the library is mod '" + query.id + "', not '" + manifest.id + "'";
    if (query.version != manifest.version)
        return "the library is version " + query.version + " but mod.json says " + manifest.version;
    if (query.major != MAGPIE_MOD_API_MAJOR || int(query.major) != manifest.api_major)
        return "it was built for mod API " + std::to_string(query.major) + " (mod.json: " + std::to_string(manifest.api_major) +
               "); this application has API " + std::to_string(MAGPIE_MOD_API_MAJOR) + ". Get a version built for it";
    if (manifest.api_min_minor > MAGPIE_MOD_API_MINOR)
        return "it needs mod API " + std::to_string(manifest.api_major) + "." + std::to_string(manifest.api_min_minor) +
               "; this application has " + std::to_string(MAGPIE_MOD_API_MAJOR) + "." + std::to_string(MAGPIE_MOD_API_MINOR) +
               ". Update the application";
    // The same source compiled with fast math or FMA contraction prints different last digits, and the
    // G-code changes with them; only the core's own mode keeps a mod's output reproducible.
    if (query.fp_mode != "precise")
        return "it was compiled with floating-point mode \"" + query.fp_mode +
               "\"; mods must be built with the SDK template (precise, no contraction)";
    return {};
}

const char *native_platform()
{
#if defined(_WIN32)
#if defined(_M_ARM64)
    return "win-arm64";
#else
    return "win-x64";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "macos-arm64";
#else
    return "macos-x64";
#endif
#else
#if defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x64";
#endif
#endif
}

std::string native_library_path(const Mod &mod)
{
    if (mod.dir.empty() || mod.binary.empty())
        return {};
#if defined(_WIN32)
    const std::string file = mod.binary + ".dll";
#elif defined(__APPLE__)
    const std::string file = "lib" + mod.binary + ".dylib";
#else
    const std::string file = "lib" + mod.binary + ".so";
#endif
    return (boost::filesystem::path(mod.dir) / "bin" / native_platform() / file).string();
}

std::string NativeModSet::load(const Mod &manifest, const std::string &library_path)
{
    if (m_shut_down)
        return "the native mods were already shut down";
    if (find(manifest.id) != nullptr)
        return "a native mod with this id is already loaded";
    std::unique_ptr<NativeMod> mod(new NativeMod);
    mod->m_manifest    = manifest;
    mod->m_loaded_path = library_path;
    std::string error;
    mod->m_library = open_library(library_path, error);
    if (mod->m_library == nullptr)
        return error;
    auto query    = reinterpret_cast<magpie_mod_query_fn>(library_symbol(mod->m_library, "magpie_mod_query"));
    auto init     = reinterpret_cast<magpie_mod_init_fn>(library_symbol(mod->m_library, "magpie_mod_init"));
    mod->m_shutdown = reinterpret_cast<magpie_mod_shutdown_fn>(library_symbol(mod->m_library, "magpie_mod_shutdown"));
    if (query == nullptr || init == nullptr || mod->m_shutdown == nullptr)
        return "the library does not export magpie_mod_query, magpie_mod_init and magpie_mod_shutdown";

    const char *answer = nullptr;
    std::string message;
    if (mod->guarded_call([&]() { answer = query(); return int32_t(MP_OK); }, &message) != MP_OK)
        return message;
    mod->m_query = answer ? answer : "";
    NativeQuery q;
    if (!parse_native_query(mod->m_query, q))
        return "magpie_mod_query answered \"" + mod->m_query + "\", not \"<id> <version> <major> <minor> fp=<mode>\"";
    if (std::string problem = native_query_problem(q, manifest); !problem.empty())
        return problem;

    // Initialise: what it registers is kept only when init succeeds and fills every declared slot.
    const size_t           slots_before    = m_slots.size();
    const size_t           services_before = m_services.size();
    NativeLoader::State    state{this, mod.get(), {}};
    MagpieRegistrar        registrar{};
    registrar.size           = sizeof(MagpieRegistrar);
    registrar.register_slot  = NativeLoader::register_slot;
    registrar.export_service = NativeLoader::export_service;
    registrar.import_service = NativeLoader::import_service;
    registrar.host_data      = &state;
    const MagpieHostApi &host = native_host_api();
    const int32_t        rc   = mod->guarded_call([&]() { return init(&host, &registrar); }, &message);
    auto roll_back = [&]() {
        m_slots.resize(slots_before);
        m_services.resize(services_before);
    };
    if (rc != MP_OK) {
        roll_back();
        if (mod->crashed())
            return message;
        // A failed init may have registered half; shut down what it set up before unloading.
        mod->guarded_call([&]() { mod->m_shutdown(); return int32_t(MP_OK); });
        return "magpie_mod_init failed (code " + std::to_string(rc) + ")" + (state.error.empty() ? std::string() : ": " + state.error);
    }
    mod->m_initialised = true;
    for (const ModSlotDecl &decl : manifest.slots)
        if (find_slot(decl.slot, decl.impl, decl.version) == nullptr) {
            roll_back();
            mod->guarded_call([&]() { mod->m_shutdown(); return int32_t(MP_OK); });
            return "mod.json declares slot " + decl.slot + " v" + std::to_string(decl.version) + " \"" + decl.impl +
                   "\" but magpie_mod_init did not register it";
        }
    m_mods.push_back(std::move(mod));
    return {};
}

NativeModSet::~NativeModSet() { shutdown(); }

void NativeModSet::shutdown()
{
    if (m_shut_down)
        return;
    m_shut_down = true;
    m_slots.clear();
    m_services.clear();
    for (auto it = m_mods.rbegin(); it != m_mods.rend(); ++it)
        if ((*it)->m_initialised && !(*it)->crashed())
            (*it)->guarded_call([&]() { (*it)->m_shutdown(); return int32_t(MP_OK); });
    // Libraries unload in the same order, as each NativeMod goes.
    while (!m_mods.empty())
        m_mods.pop_back();
}

const NativeMod *NativeModSet::find(const std::string &id) const
{
    for (const auto &m : m_mods)
        if (m->id() == id)
            return m.get();
    return nullptr;
}

const NativeEntry *NativeModSet::find_slot(const std::string &slot, const std::string &impl, uint32_t min_version) const
{
    for (const NativeEntry &e : m_slots)
        if (e.name == slot && e.impl == impl && e.version >= min_version)
            return &e;
    return nullptr;
}

std::vector<const NativeEntry *> NativeModSet::slots(const std::string &slot) const
{
    std::vector<const NativeEntry *> out;
    for (const NativeEntry &e : m_slots)
        if (e.name == slot)
            out.push_back(&e);
    return out;
}

const NativeEntry *NativeModSet::find_service(const std::string &name, uint32_t min_version) const
{
    for (const NativeEntry &e : m_services)
        if (e.name == name && e.version >= min_version)
            return &e;
    return nullptr;
}

std::shared_ptr<const NativeModSet> active_native_mods()
{
    std::lock_guard<std::mutex> lock(g_active_native_mutex);
    return g_active_native;
}

void set_active_native_mods(std::shared_ptr<const NativeModSet> mods)
{
    std::lock_guard<std::mutex> lock(g_active_native_mutex);
    g_active_native = std::move(mods);
}

std::string native_mod_summary(const std::set<std::string> &used)
{
    std::string out;
    if (std::shared_ptr<const NativeModSet> mods = active_native_mods())
        for (const auto &m : mods->mods())
            if (used.count(m->id()) > 0)
                out += (out.empty() ? "" : ", ") + m->id() + " " + m->manifest().version;
    return out;
}

void shutdown_native_mods()
{
    std::shared_ptr<const NativeModSet> mods = active_native_mods();
    if (mods)
        const_cast<NativeModSet &>(*mods).shutdown();
}

} // namespace ModHost
} // namespace Slic3r
