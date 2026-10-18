// gcode_processor_bench: times GCodeProcessor on an exported G-code file the way G-code export
// feeds it (one buffer per layer through process_buffer), to see where the processor spends its
// time. Splits the run into tokenizing alone (GCodeReader with an empty callback) and the whole
// processor, then finalize(false).
//
//   gcode_processor_bench <file.gcode> [repeats] [samples.txt]
//
// With samples.txt, a sampling thread records the main thread's call stack (as module-relative
// addresses, from the x64 unwind tables) about every millisecond during process_buffer; resolve
// them against the linker map (gcode_processor_bench.map) to see which functions take the time.

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/StageTimer.hpp"

#include <boost/nowide/fstream.hpp>

// nanosvg is header-only and libslic3r references it without carrying the implementation.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

using namespace Slic3r;

#ifdef _WIN32
// Samples the call stack of `thread` until `stop` is set; one line of hex RVAs per sample, leaf first.
static void sample_stacks(HANDLE thread, std::atomic<bool> &stop, std::vector<std::vector<DWORD64>> &samples)
{
    const DWORD64 image = DWORD64(GetModuleHandleW(nullptr));
    timeBeginPeriod(1);
    while (! stop.load()) {
        Sleep(1);
        if (SuspendThread(thread) == DWORD(-1))
            break;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        std::vector<DWORD64> stack;
        if (GetThreadContext(thread, &ctx)) {
            for (int depth = 0; depth < 64 && ctx.Rip != 0; ++ depth) {
                stack.push_back(ctx.Rip - image);
                DWORD64 image_base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
                if (fn == nullptr) {
                    // Leaf function without unwind data: return address is on top of the stack.
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                } else {
                    void *handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
                }
            }
        }
        ResumeThread(thread);
        samples.emplace_back(std::move(stack));
    }
    timeEndPeriod(1);
}
#endif

static double seconds_since(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: gcode_processor_bench <file.gcode> [repeats]\n");
        return 1;
    }
    const std::string path    = argv[1];
    const int         repeats = argc > 2 ? std::max(1, std::atoi(argv[2])) : 3;
    const char       *samples_path = argc > 3 ? argv[3] : nullptr;
    std::vector<std::vector<unsigned long long>> samples;
    CNumericLocalesSetter locales_setter;

    std::string text;
    {
        boost::nowide::ifstream f(path, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    // One buffer per layer, as GCode::process_layers() hands them to the output stream.
    std::vector<std::string> buffers;
    for (size_t begin = 0; begin < text.size();) {
        size_t next = text.find("\n;LAYER_CHANGE", begin + 1);
        size_t end  = next == std::string::npos ? text.size() : next + 1;
        buffers.emplace_back(text, begin, end - begin);
        begin = end;
    }

    DynamicPrintConfig dconfig;
    dconfig.apply(FullPrintConfig::defaults());
    dconfig.load_from_gcode_file(path, ForwardCompatibilitySubstitutionRule::EnableSilent);
    PrintConfig config;
    config.apply(dconfig, true);

    std::printf("%s: %zu bytes, %zu buffers\n", path.c_str(), text.size(), buffers.size());
    for (int r = 0; r < repeats; ++ r) {
        auto t0 = std::chrono::steady_clock::now();
        size_t lines = 0;
        {
            GCodeReader reader;
            for (const std::string &b : buffers)
                reader.parse_buffer(b, [&lines](GCodeReader &, const GCodeReader::GCodeLine &) { ++ lines; });
        }
        const double tokenize = seconds_since(t0);

        GCodeProcessor processor;
        processor.apply_config(config);
        processor.initialize(path + ".bench.tmp");
        // MAGPIE_BENCH_RESERVE=<n>: reserve the move list up front, to measure what its growth costs.
        if (const char *reserve = std::getenv("MAGPIE_BENCH_RESERVE"); reserve != nullptr)
            processor.result().moves.reserve(size_t(std::atoll(reserve)));
        t0 = std::chrono::steady_clock::now();
#ifdef _WIN32
        std::atomic<bool> stop { false };
        std::thread       sampler;
        HANDLE            main_thread = nullptr;
        std::vector<std::vector<DWORD64>> run_samples;
        if (samples_path) {
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
            sampler = std::thread(sample_stacks, main_thread, std::ref(stop), std::ref(run_samples));
        }
#endif
        for (const std::string &b : buffers)
            processor.process_buffer(b);
        const double process = seconds_since(t0);
#ifdef _WIN32
        if (samples_path) {
            stop = true;
            sampler.join();
            CloseHandle(main_thread);
            for (auto &st : run_samples)
                samples.emplace_back(st.begin(), st.end());
        }
#endif
        t0 = std::chrono::steady_clock::now();
        processor.finalize(false);
        const double finalize = seconds_since(t0);
        std::printf("run %d: lines %zu  tokenize %.3f s  process_buffer %.3f s  finalize %.3f s  moves %zu\n", r, lines, tokenize,
                    process, finalize, processor.result().moves.size());
    }
    if (samples_path) {
        FILE *f = std::fopen(samples_path, "w");
        for (const auto &st : samples) {
            for (unsigned long long a : st)
                std::fprintf(f, "%llx ", a);
            std::fprintf(f, "\n");
        }
        std::fclose(f);
        std::printf("%zu stack samples written to %s\n", samples.size(), samples_path);
    }
#ifdef MAGPIE_SLICING_TIMING
    StageTimer::dump(std::string());
#endif
    return 0;
}
