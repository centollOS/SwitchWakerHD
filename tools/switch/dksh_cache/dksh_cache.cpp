// The offline DKSH cache for the deko3d renderer (docs/deko3d-plan.md section 2): every GLSL source of a
// shadercache_gl.bin (WGS1) converted with gfxdk::glsl_to_deko and compiled with uamlib, written as a
// shadercache_dksh.bin (WDK1, runtime/src/gfx/deko/shader_files.h). Build: tools/switch/dksh_cache/build.sh.
//
//     dksh_cache build <shadercache_gl.bin> <shadercache_dksh.bin>
//         converts + compiles every source (one thread: uam is not reentrant), writes the WDK1 file, reads it
//         back and prints counts, failures (grouped by their first error line), times and the file size
//     dksh_cache coverage <console shadercache_gl.bin> <harvest shadercache_gl.bin>
//         how many of the console's sources (and linked pairs) the harvest has, by WGS1 hash: the hit rate an
//         offline cache built from the harvest would have had on that console
//     dksh_cache dump <shadercache_dksh.bin> <dir>
//         writes each record's DKSH to <dir>/<hash>_vs.dksh or _ps.dksh (comparisons with the uam CLI)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "glsl_convert.h"
#include "shader_files.h"
#include "uam_api.h"

using namespace gfxdk;

namespace {
std::vector<uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}
bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return bool(f);
}
std::vector<Wgs1Source> load_wgs1(const char* path, std::vector<std::pair<uint64_t, uint64_t>>* pairs = nullptr) {
    std::vector<uint8_t> data = read_file(path);
    std::vector<Wgs1Source> src;
    std::string err;
    if (data.empty() || !read_wgs1(data, &src, &err, pairs)) {
        fprintf(stderr, "%s: %s\n", path, data.empty() ? "cannot read" : err.c_str());
        exit(1);
    }
    return src;
}
// "0:123(4): error: ..." -> "error: ...", so that equal errors on different lines group together
std::string first_error(const std::string& log) {
    std::istringstream in(log);
    std::string line, first;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (first.empty()) first = line;
        if (line.find("error") != std::string::npos) {
            first = line;
            break;
        }
    }
    size_t e = first.find("error");
    if (e != std::string::npos) first = first.substr(e);
    return first.empty() ? "(no output)" : first;
}
double pct(const std::vector<double>& sorted, double p) {
    return sorted.empty() ? 0 : sorted[std::min(sorted.size() - 1, size_t(sorted.size() * p))];
}

int build(const char* in, const char* out) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Wgs1Source> src = load_wgs1(in);
    const uint64_t uamId = dksh_uam_id();
    std::vector<uint8_t> file = wdk1_header(uamId);
    std::map<std::string, std::pair<int, uint64_t>> failures;  // first error -> count, an example hash
    std::vector<double> ms;
    size_t ok = 0, vs = 0, dkshBytes = 0, convertFail = 0, compileFail = 0;
    uam::init(true);
    for (size_t i = 0; i < src.size(); i++) {
        const Wgs1Source& s = src[i];
        DkshRecord r;
        r.stage = uint8_t(s.vertex ? uam::Stage::Vertex : uam::Stage::Fragment);
        r.glslHash = s.hash;
        vs += s.vertex;
        ConvertedBindings b;
        const std::string glsl = glsl_to_deko(s.glsl, s.vertex, &b);
        if (glsl.empty()) {
            convertFail++;
            auto& f = failures["glsl_to_deko: " + b.error];
            if (!f.first++) f.second = s.hash;
        } else {
            r.set_bindings(b);
            const auto c0 = std::chrono::steady_clock::now();
            uam::Result res = uam::compile(s.vertex ? uam::Stage::Vertex : uam::Stage::Fragment, glsl.c_str());
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count());
            if (res.ok && !res.dksh.empty()) {
                r.dksh = std::move(res.dksh);
                dkshBytes += r.dksh.size();
                ok++;
            } else {
                compileFail++;
                auto& f = failures["uam: " + first_error(res.log)];
                if (!f.first++) f.second = s.hash;
            }
        }
        append_wdk1_record(&file, r);  // a failure too (size 0): the renderer does not retry it
        if ((i + 1) % 1000 == 0) fprintf(stderr, "  %zu / %zu\n", i + 1, src.size());
    }
    uam::shutdown();
    if (!write_file(out, file)) {
        fprintf(stderr, "%s: cannot write\n", out);
        return 1;
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // read back what was written
    std::vector<DkshRecord> back;
    uint64_t backId = 0;
    std::string err;
    size_t backOk = 0;
    if (!read_wdk1(read_file(out), &back, &backId, &err)) {
        fprintf(stderr, "read back: %s\n", err.c_str());
        return 1;
    }
    for (const DkshRecord& r : back) backOk += !r.dksh.empty();
    const bool same = back.size() == src.size() && backOk == ok && backId == uamId;

    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double x : ms) sum += x;
    printf("%s: %zu sources (%zu vertex, %zu pixel)\n", in, src.size(), vs, src.size() - vs);
    printf("compiled %zu / %zu; failed: %zu in glsl_to_deko, %zu in uam\n", ok, src.size(), convertFail, compileFail);
    for (auto& [why, f] : failures)
        printf("  %5d x %s (e.g. %016llx)\n", f.first, why.c_str(), (unsigned long long)f.second);
    printf("uam per shader: mean %.1f ms, p50 %.1f, p90 %.1f, p99 %.1f, max %.1f; compile sum %.1f s, wall %.1f s\n",
           ms.empty() ? 0 : sum / ms.size(), pct(ms, .5), pct(ms, .9), pct(ms, .99), ms.empty() ? 0 : ms.back(),
           sum / 1000, wall);
    printf("%s: %zu bytes (%.2f MiB), DKSH %zu bytes (mean %zu per shader), uamId %016llx\n", out, file.size(),
           file.size() / 1048576.0, dkshBytes, ok ? dkshBytes / ok : 0, (unsigned long long)uamId);
    printf("read back: %zu records, %zu with DKSH, uamId %s: %s\n", back.size(), backOk,
           backId == uamId ? "same" : "DIFFERENT", same ? "OK" : "MISMATCH");
    return same && !convertFail && !compileFail ? 0 : 2;
}

int coverage(const char* console, const char* harvest) {
    std::vector<std::pair<uint64_t, uint64_t>> cPairs, hPairs;
    std::vector<Wgs1Source> c = load_wgs1(console, &cPairs), h = load_wgs1(harvest, &hPairs);
    std::unordered_set<uint64_t> have;
    for (auto& s : h) have.insert(s.hash);
    size_t hit[2] = {}, total[2] = {};
    std::vector<uint64_t> missing;
    for (auto& s : c) {
        total[s.vertex]++;
        if (have.count(s.hash)) hit[s.vertex]++;
        else missing.push_back(s.hash);
    }
    // pairs: distinct (vs, ps) the console linked, both stages in the harvest
    std::unordered_set<uint64_t> seenPair;
    size_t pairHit = 0, pairTotal = 0;
    for (auto& [v, p] : cPairs) {
        if (!seenPair.insert(v * 0x9E3779B97F4A7C15ull ^ p).second) continue;
        pairTotal++;
        pairHit += have.count(v) && have.count(p);
    }
    const size_t all = total[0] + total[1], allHit = hit[0] + hit[1];
    auto rate = [](size_t a, size_t b) { return b ? 100.0 * a / b : 0.0; };
    printf("console %s: %zu sources (%zu vertex, %zu pixel), %zu distinct pairs\n", console, all, total[1], total[0],
           pairTotal);
    printf("harvest %s: %zu sources\n", harvest, h.size());
    printf("sources in the harvest: %zu / %zu = %.1f%% (vertex %zu / %zu = %.1f%%, pixel %zu / %zu = %.1f%%)\n", allHit,
           all, rate(allHit, all), hit[1], total[1], rate(hit[1], total[1]), hit[0], total[0], rate(hit[0], total[0]));
    printf("expected miss rate of the offline cache: %.1f%% of sources (%zu)\n", 100 - rate(allHit, all),
           missing.size());
    printf("pairs with both stages in the harvest: %zu / %zu = %.1f%%\n", pairHit, pairTotal, rate(pairHit, pairTotal));
    for (size_t i = 0; i < missing.size() && i < 20; i++) printf("  missing %016llx\n", (unsigned long long)missing[i]);
    return 0;
}

int dump(const char* in, const char* dir) {
    std::vector<DkshRecord> recs;
    uint64_t id = 0;
    std::string err;
    if (!read_wdk1(read_file(in), &recs, &id, &err)) {
        fprintf(stderr, "%s: %s\n", in, err.c_str());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    size_t n = 0;
    for (const DkshRecord& r : recs) {
        if (r.dksh.empty()) continue;
        char name[64];
        snprintf(name, sizeof name, "/%016llx_%s.dksh", (unsigned long long)r.glslHash,
                 r.stage == uint8_t(uam::Stage::Vertex) ? "vs" : "ps");
        n += write_file(dir + std::string(name), r.dksh);
    }
    printf("%zu DKSH files written to %s (uamId %016llx, this build %016llx)\n", n, dir, (unsigned long long)id,
           (unsigned long long)dksh_uam_id());
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && !strcmp(argv[1], "build")) return build(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "coverage")) return coverage(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "dump")) return dump(argv[2], argv[3]);
    fprintf(stderr,
            "usage: dksh_cache build <shadercache_gl.bin> <shadercache_dksh.bin>\n"
            "       dksh_cache coverage <console shadercache_gl.bin> <harvest shadercache_gl.bin>\n"
            "       dksh_cache dump <shadercache_dksh.bin> <dir>\n");
    return 1;
}
