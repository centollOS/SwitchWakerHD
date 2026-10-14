// Graphics > Shader cache > "Export shader template" (issue #177): runs the release's
// tools/shaderprep.py export on this player's live shader caches. The output is a code-free template
// (hashes, register values, pipeline settings; the script refuses anything its release guard rejects)
// in the screenshots folder, to attach to issue #177. Raw caches and SPIR-V never leave their folder.
#pragma once
#include "../gfx/shader_prebuild.h"
#include "../mods/mod_json.h"
#include "../platform/host.h"
#include "../platform/process.h"
#include "../screenshot.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace template_export {
namespace fs = std::filesystem;

struct Status { bool running = false; std::string line; };

// (python command, shaderprep.py): setup's guest-sdk.json names its Python and the release's
// tools/installer/setup.py; otherwise the release folder above the game's data folder, or the source
// tree above the build folder, with the bundled Windows Python or python3 from PATH.
inline bool locate(std::vector<std::string>& python, fs::path& script) {
    auto with_python = [&](const fs::path& tools) {
        script = tools / "shaderprep.py";
        if (!fs::is_regular_file(script)) return false;
        if (python.empty()) {
            const auto bundled = tools / "python" / "python.exe";
#ifdef _WIN32
            python = {fs::is_regular_file(bundled) ? bundled.string() : std::string("python")};
#else
            python = {"python3"};
#endif
        }
        return true;
    };
    try {
        const char* config = std::getenv("WWHD_GUEST_BUILD_CONFIG");
        if (config && fs::is_regular_file(config) && fs::file_size(config) < 1024 * 1024) {
            std::ifstream input(config);
            std::string text{std::istreambuf_iterator<char>(input), {}};
            auto json = mods::json::parse(text);
            const auto base = fs::absolute(config).parent_path();
            const auto& command = json.get("python");
            if (command.type == mods::json::Value::Array && !command.array.empty()) {
                for (const auto& arg : command.array) python.push_back(arg.string());
                fs::path executable(python.front());
                if (executable.is_relative() && executable.has_parent_path())
                    python.front() = (base / executable).lexically_normal().string();
            }
            fs::path setup(json.get("setup").string());
            if (!setup.empty()) {
                if (setup.is_relative()) setup = base / setup;
                if (with_python(setup.lexically_normal().parent_path().parent_path())) return true;
            }
        }
    } catch (...) { python.clear(); }
    std::vector<fs::path> roots;
    if (const char* install = std::getenv("WWHD_INSTALL_DIR")) roots.push_back(fs::path(install).parent_path());
    for (fs::path dir = fs::path(host::exe_dir()); !dir.empty(); dir = dir.parent_path()) {
        roots.push_back(dir);
        if (dir == dir.parent_path() || roots.size() > 6) break;
    }
    for (const auto& root : roots)
        if (with_python(root / "tools")) return true;
    return false;
}

inline std::mutex& mutex() { static std::mutex m; return m; }
inline Status& state() { static Status s; return s; }
inline Status status() { std::lock_guard lock(mutex()); return state(); }

inline void start() {
    {
        std::lock_guard lock(mutex());
        if (state().running) return;
        state() = {true, "Exporting the shader template..."};
    }
    std::thread([] {
        std::string line;
        try {
            std::vector<std::string> python;
            fs::path script;
            if (!locate(python, script)) {
                line = "Export needs the release's tools folder (tools/shaderprep.py); see tools/shadercache/README.md.";
            } else {
                const fs::path folder = fs::u8path(screenshot::dir());
                fs::create_directories(folder);
                const auto out = folder / "my-template.bin";
                auto command = python;
                command.insert(command.end(), {script.string(), "export", out.string(), "--from",
                                               (shaderprebuild::root() / "live").string()});
                const auto result = host::run_process(command);
                std::istringstream lines(result.output);
                std::string text, last;
                unsigned long states = 0, pipelines = 0;
                bool done = false;
                while (std::getline(lines, text)) {
                    if (!text.empty() && text.back() == '\r') text.pop_back();
                    if (text.rfind("WWHD-EXPORT ", 0) == 0)
                        done = std::sscanf(text.c_str(), "WWHD-EXPORT %lu %lu", &states, &pipelines) == 2;
                    else if (!text.empty()) last = text;
                }
                if (result.code == 0 && done)
                    line = "Saved " + out.string() + " (" + std::to_string(states) + " states, " +
                           std::to_string(pipelines) + " pipelines). Attach it to issue #177.";
                else
                    line = "Export failed: " + (!result.error.empty() ? result.error : last.empty() ? std::string("no output") : last);
            }
        } catch (const std::exception& e) {
            line = std::string("Export failed: ") + e.what();
        }
        std::fprintf(stderr, "[shader template] %s\n", line.c_str());
        std::lock_guard lock(mutex());
        state() = {false, line};
    }).detach();
}
}  // namespace template_export
