#include "StageTimer.hpp"

#ifdef MAGPIE_SLICING_TIMING

#include <boost/nowide/cstdio.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace Slic3r::StageTimer {

namespace {
std::mutex &mutex()
{
    // Never destroyed: the exit-time dump may run after function statics are gone.
    static std::mutex *m = new std::mutex;
    return *m;
}
std::map<std::string, Total> &table()
{
    static auto *t = new std::map<std::string, Total>;
    return *t;
}
long long now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void add(const char *name, double seconds)
{
    std::lock_guard<std::mutex> lock(mutex());
    Total &t = table()[name];
    t.name = name;
    t.seconds += seconds;
    ++t.calls;
}

std::vector<Total> totals()
{
    std::lock_guard<std::mutex> lock(mutex());
    std::vector<Total> out;
    for (const auto &[name, total] : table())
        out.push_back(total);
    return out;
}

void reset()
{
    std::lock_guard<std::mutex> lock(mutex());
    table().clear();
}

void dump(const std::string &path)
{
    FILE *f = path.empty() ? stderr : boost::nowide::fopen(path.c_str(), "w");
    if (f == nullptr)
        return;
    for (const Total &t : totals())
        fprintf(f, "%s\t%.6f\t%lld\n", t.name.c_str(), t.seconds, t.calls);
    if (f != stderr)
        fclose(f);
}

namespace {
// MAGPIE_STAGE_TIMING_FILE=<path> writes the totals there when the process exits.
struct DumpAtExit
{
    DumpAtExit()
    {
        if (const char *path = std::getenv("MAGPIE_STAGE_TIMING_FILE"); path != nullptr && path[0] != 0)
            std::atexit([] { dump(std::getenv("MAGPIE_STAGE_TIMING_FILE")); });
    }
} dump_at_exit;
} // namespace

Scope::Scope(const char *name) : m_name(name), m_start_ns(now_ns()) {}
Scope::~Scope() { add(m_name, double(now_ns() - m_start_ns) * 1e-9); }

} // namespace Slic3r::StageTimer

#endif
