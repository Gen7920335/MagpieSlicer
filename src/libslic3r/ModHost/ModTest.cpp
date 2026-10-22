#include "ModTest.hpp"
#include "ModSet.hpp"
#include "NativeMods.hpp"

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <regex>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <boost/nowide/convert.hpp>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Slic3r {
namespace ModHost {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

std::string read_text(const fs::path &path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    if (!in)
        throw ModError("cannot read " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> string_list(const json &j, const char *key, const std::string &where)
{
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it == j.end())
        return out;
    if (!it->is_array())
        throw ModError(where + ": \"" + key + "\" must be a list of strings");
    for (const json &v : *it) {
        if (!v.is_string())
            throw ModError(where + ": \"" + key + "\" must be a list of strings");
        out.push_back(v.get<std::string>());
    }
    return out;
}

} // namespace

std::vector<std::string> check_gcode(const std::string &gcode, const GCodeBounds &b, size_t limit)
{
    std::vector<std::string> problems;
    bool                     relative_e = false;
    std::istringstream       in(gcode);
    std::string              raw;
    static const std::regex  number(R"(([XYZEFS])(-?[0-9.]+(?:e-?[0-9]+)?|nan|inf|-inf))", std::regex::icase);
    for (size_t n = 1; std::getline(in, raw) && problems.size() < limit; ++n) {
        std::string line = raw.substr(0, raw.find(';'));
        line.erase(0, line.find_first_not_of(" \t\r"));
        line.erase(line.find_last_not_of(" \t\r") + 1);
        if (line.empty())
            continue;
        std::string word = line.substr(0, line.find_first_of(" \t"));
        std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        if (word == "M83")
            relative_e = true;
        else if (word == "M82")
            relative_e = false;
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        // Macro names such as SET_PRINT_STATS_INFO hold "inf" without being a number.
        for (size_t at; (at = lower.find("info")) != std::string::npos;)
            lower.erase(at, 4);
        if (lower.find("nan") != std::string::npos || lower.find("inf") != std::string::npos) {
            problems.push_back(std::to_string(n) + ": non-finite number: " + line);
            continue;
        }
        const bool move = word == "G0" || word == "G1" || word == "G2" || word == "G3";
        const bool temp = word == "M104" || word == "M109" || word == "M140" || word == "M190";
        if (!move && !temp)
            continue;
        std::map<char, double> values;
        for (std::sregex_iterator it(line.begin(), line.end(), number), end; it != end; ++it)
            values[char(std::toupper((*it)[1].str()[0]))] = std::strtod((*it)[2].str().c_str(), nullptr);
        if (move) {
            if (auto x = values.find('X'); x != values.end() && !(b.min_x_mm - b.slack_mm <= x->second && x->second <= b.max_x_mm + b.slack_mm))
                problems.push_back(std::to_string(n) + ": X off the bed: " + line);
            if (auto y = values.find('Y'); y != values.end() && !(b.min_y_mm - b.slack_mm <= y->second && y->second <= b.max_y_mm + b.slack_mm))
                problems.push_back(std::to_string(n) + ": Y off the bed: " + line);
            if (auto z = values.find('Z'); z != values.end() && !(0. <= z->second && z->second <= b.max_z_mm))
                problems.push_back(std::to_string(n) + ": Z out of range: " + line);
            if (auto e = values.find('E'); relative_e && e != values.end() && std::abs(e->second) > b.max_e_move_mm)
                problems.push_back(std::to_string(n) + ": E move too long: " + line);
        } else if (auto s = values.find('S'); s != values.end() && !(0. <= s->second && s->second <= b.max_temp_c)) {
            problems.push_back(std::to_string(n) + ": temperature out of range: " + line);
        }
    }
    return problems;
}

GCodeBounds bounds_from_gcode(const std::string &gcode)
{
    GCodeBounds b;
    b.min_x_mm = b.min_y_mm = std::numeric_limits<double>::max();
    b.max_x_mm = b.max_y_mm = std::numeric_limits<double>::lowest();
    auto value_of = [&gcode](const std::string &key) -> std::string {
        const std::string tag = "; " + key + " = ";
        const size_t      at  = gcode.find("\n" + tag);
        if (at == std::string::npos)
            return {};
        const size_t start = at + 1 + tag.size();
        return gcode.substr(start, gcode.find('\n', start) - start);
    };
    // printable_area = 0x0,270x0,270x270,0x270 (mm)
    std::stringstream area(value_of("printable_area"));
    for (std::string point; std::getline(area, point, ',');) {
        const size_t x = point.find('x');
        if (x == std::string::npos)
            continue;
        const double px = std::strtod(point.substr(0, x).c_str(), nullptr), py = std::strtod(point.substr(x + 1).c_str(), nullptr);
        b.min_x_mm = std::min(b.min_x_mm, px);
        b.max_x_mm = std::max(b.max_x_mm, px);
        b.min_y_mm = std::min(b.min_y_mm, py);
        b.max_y_mm = std::max(b.max_y_mm, py);
    }
    if (b.min_x_mm > b.max_x_mm)
        throw ModError("the G-code has no printable_area in its config block");
    const std::string height = value_of("printable_height");
    b.max_z_mm = height.empty() ? std::max(b.max_x_mm, b.max_y_mm) : std::strtod(height.c_str(), nullptr);
    return b;
}

std::string normalize_gcode_for_comparison(const std::string &gcode)
{
    static const std::regex producer(R"(^; generated by .*$)");
    static const std::regex object_id(R"((; (?:printing|stop printing) object .*?) id:\d+)");
    std::istringstream in(gcode);
    std::string        out, line;
    while (std::getline(in, line)) {
        line = std::regex_replace(line, producer, "; generated by <normalized>");
        line = std::regex_replace(line, object_id, "$1 id:<n>");
        out += line + '\n';
    }
    return out;
}

std::vector<ModTestCase> read_mod_test_cases(const std::string &package_dir)
{
    std::vector<ModTestCase> cases;
    const fs::path tests = fs::path(package_dir) / "tests";
    if (!fs::is_directory(tests))
        return cases;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(tests), end; it != end; ++it)
        if (fs::is_regular_file(it->status()))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const fs::path &file : files) {
        const std::string ext = file.extension().string();
        ModTestCase       c;
        c.name = file.stem().string();
        if (boost::iequals(ext, ".3mf")) {
            // A bare project, unless a case file of the same name says how to slice it.
            if (fs::exists(fs::path(file).replace_extension(".json")))
                continue;
            c.args = {file.string()};
        } else if (boost::iequals(ext, ".json")) {
            const std::string where = "tests/" + file.filename().string();
            json j;
            try {
                j = json::parse(read_text(file));
            } catch (const json::parse_error &e) {
                throw ModError(where + ": " + e.what());
            }
            if (!j.is_object())
                throw ModError(where + ": a test case must be a JSON object");
            for (std::string arg : string_list(j, "args", where)) {
                // Arguments naming files of the tests folder, alone or as a ';' list ("--load-settings
                // machine.json;process.json"), are made absolute.
                if (!arg.empty() && arg[0] != '-') {
                    std::vector<std::string> parts;
                    boost::split(parts, arg, boost::is_any_of(";"));
                    if (std::all_of(parts.begin(), parts.end(), [&tests](const std::string &p) { return !p.empty() && fs::exists(tests / p); })) {
                        for (std::string &p : parts)
                            p = (tests / p).string();
                        arg = boost::join(parts, ";");
                    }
                }
                c.args.push_back(arg);
            }
            if (c.args.empty())
                throw ModError(where + ": \"args\" is empty");
            c.contains     = string_list(j, "contains", where);
            c.not_contains = string_list(j, "not_contains", where);
        } else {
            continue;
        }
        cases.push_back(std::move(c));
    }
    return cases;
}

#ifdef _WIN32
// Quotes one argument the way CommandLineToArgvW reads it back.
static std::wstring quote_argument(const std::wstring &arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"')
            out.append(backslashes * 2 + 1, L'\\');
        else
            out.append(backslashes, L'\\');
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
    return out;
}
#endif

int run_child_process(const std::string &exe, const std::vector<std::string> &args, const std::string &output_file)
{
#ifdef _WIN32
    std::wstring command = quote_argument(boost::nowide::widen(exe));
    for (const std::string &a : args)
        command += L" " + quote_argument(boost::nowide::widen(a));
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE out = ::CreateFileW(boost::nowide::widen(output_file).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE)
        return -1;
    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out;
    si.hStdError  = out;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    const BOOL started = ::CreateProcessW(boost::nowide::widen(exe).c_str(), buffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                          nullptr, nullptr, &si, &pi);
    ::CloseHandle(out);
    if (!started)
        return -1;
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return int(code);
#else
    const pid_t pid = ::fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        const int fd = ::open(output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ::dup2(fd, 1);
            ::dup2(fd, 2);
        }
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(exe.c_str()));
        for (const std::string &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(exe.c_str(), argv.data());
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

ModTestReport run_mod_test(const std::string &id, const ModTestSlice &slice)
{
    ModTestReport report;
    auto fail = [&report](const std::string &line) {
        report.passed = false;
        report.lines.push_back("FAIL " + line);
    };
    auto pass = [&report](const std::string &line) { report.lines.push_back("ok   " + line); };

    std::shared_ptr<const ModSet> mods = active_mods();
    const Mod *mod = mods ? mods->find_mod(id) : nullptr;
    if (mod == nullptr) {
        fail("mod '" + id + "' is not loaded; the application log says why (\"Mod not loaded\")");
        return report;
    }
    pass("loaded " + id + " " + mod->version + (mod->official ? " (official)" : mod->verified ? " (verified)" : " (unverified)"));
    if (!mod->binary.empty()) {
        std::shared_ptr<const NativeModSet> native = active_native_mods();
        const NativeMod *lib = native ? native->find(id) : nullptr;
        if (lib == nullptr) {
            fail("its native library is not loaded");
            return report;
        }
        NativeQuery q;
        if (!parse_native_query(lib->query(), q) || !native_query_problem(q, *mod).empty())
            fail("query \"" + lib->query() + "\" is not accepted");
        else
            pass("library " + lib->loaded_path() + ": \"" + lib->query() + "\"");
        for (const ModSlotDecl &d : mod->slots)
            if (native->find_slot(d.slot, d.impl, d.version) != nullptr)
                pass("fills slot " + d.slot + " v" + std::to_string(d.version) + " \"" + d.impl + "\"");
            else
                fail("slot " + d.slot + " \"" + d.impl + "\" is declared but not registered");
    }
    if (mod->dir.empty()) {
        pass("a single-file mod has no tests folder");
        return report;
    }
    std::vector<ModTestCase> cases;
    try {
        cases = read_mod_test_cases(mod->dir);
    } catch (const std::exception &e) {
        fail(e.what());
        return report;
    }
    if (cases.empty())
        report.lines.push_back("note no test cases in " + (fs::path(mod->dir) / "tests").string());
    for (const ModTestCase &c : cases) {
        std::string first, second;
        try {
            first  = slice(c, 1);
            second = slice(c, 2);
        } catch (const std::exception &e) {
            fail(c.name + ": slicing failed: " + e.what());
            continue;
        }
        bool ok = true;
        if (normalize_gcode_for_comparison(first) != normalize_gcode_for_comparison(second)) {
            fail(c.name + ": two slices of the same input differ (not deterministic)");
            ok = false;
        }
        try {
            for (const std::string &p : check_gcode(first, bounds_from_gcode(first))) {
                fail(c.name + ": " + p);
                ok = false;
            }
        } catch (const std::exception &e) {
            fail(c.name + ": " + e.what());
            ok = false;
        }
        for (const std::string &text : c.contains)
            if (first.find(text) == std::string::npos) {
                fail(c.name + ": G-code lacks \"" + text + "\"");
                ok = false;
            }
        for (const std::string &text : c.not_contains)
            if (first.find(text) != std::string::npos) {
                fail(c.name + ": G-code has \"" + text + "\"");
                ok = false;
            }
        if (ok)
            pass(c.name);
    }
    return report;
}

} // namespace ModHost
} // namespace Slic3r
