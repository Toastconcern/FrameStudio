// Frame Studio's own updates. Every push to main replaces the GitHub release "latest", and with it latest.json:
//   { "build": "<commit>", "home": "<Frame Home's hash>", "files": ["FrameStudio-windows-x64.zip", ...] }
// A build from another commit than this one (FRAME_BUILD, set by CI) is offered. Installing downloads this computer's
// file, unpacks it, and puts each thing in it where the old one was (the old one renamed aside first: the running
// copy keeps going, and a failure puts everything back). Frame Studio then restarts into it; the next start deletes
// what was set aside. A local build (no FRAME_BUILD) never updates itself.
#pragma once

#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef FRAME_BUILD
#define FRAME_BUILD ""
#endif

namespace updater {

namespace fs = std::filesystem;

inline constexpr const char* kBuild = FRAME_BUILD;
inline constexpr const char* kReleaseUrl = "https://github.com/Toastconcern/FrameStudio/releases/download/latest/";
// Where latest.json and the downloads are (FS_UPDATE_URL: another place, ending in "/", for testing an update).
inline std::string releaseUrl() {
    const char* u = std::getenv("FS_UPDATE_URL");
    return u && *u ? std::string(u) : std::string(kReleaseUrl);
}
inline constexpr const char* kPageUrl = "https://github.com/Toastconcern/FrameStudio/releases/tag/latest";

// This computer's download in a release (as CI names them).
inline std::string assetName() {
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
    return "FrameStudio-windows-arm64.zip";
#elif defined(_M_IX86) || defined(__i386__)
    return "FrameStudio-windows-x86.zip";
#else
    return "FrameStudio-windows-x64.zip";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "FrameStudio-macos-arm64.zip";
#else
    return "FrameStudio-macos-x64.zip";
#endif
#else
#if defined(__aarch64__)
    return "FrameStudio-linux-arm64.tar.gz";
#elif defined(__i386__)
    return "FrameStudio-linux-x86.tar.gz";
#else
    return "FrameStudio-linux-x64.tar.gz";
#endif
#endif
}

// The folder the download unpacks to: the asset's name without its extension.
inline std::string assetFolder() {
    const std::string a = assetName();
    return a.substr(0, a.find('.'));
}

inline fs::path workDir() { return platform::userDataDir() / L"FrameStudio" / L"update"; }

// The string value of "key" in a small JSON object ("" when it's not there).
inline std::string jsonString(const std::string& json, const std::string& key) {
    size_t p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return {};
    p = json.find(':', p);
    if (p == std::string::npos) return {};
    p = json.find('"', p);
    if (p == std::string::npos) return {};
    const size_t e = json.find('"', p + 1);
    return e == std::string::npos ? std::string() : json.substr(p + 1, e - p - 1);
}

// What the update replaces: each thing the download holds, and where it goes. On macOS the whole .app.
struct Swap { fs::path from, to; };

inline std::vector<Swap> swapsFor(const fs::path& unpacked) {
    std::vector<Swap> out;
    std::error_code ec;
#ifdef __APPLE__
    const fs::path bundle = platform::exeDir().parent_path().parent_path();   // .../Frame Studio.app/Contents/MacOS
    out.push_back({unpacked / L"Frame Studio.app", bundle});
#else
    for (const auto& e : fs::directory_iterator(unpacked, ec)) out.push_back({e.path(), platform::exeDir() / e.path().filename()});
#endif
    return out;
}

// The program to start after an update.
inline fs::path updatedProgram() {
#if defined(__APPLE__)
    return platform::exeDir().parent_path().parent_path();
#elif defined(_WIN32)
    return platform::exeDir() / L"Frame Studio.exe";
#else
    return platform::exeDir() / L"Frame Studio";
#endif
}

// At start: delete what the last update set aside (listed in update/aside.txt).
inline void cleanUp() {
    const fs::path list = workDir() / L"aside.txt";
    std::error_code ec;
    if (FILE* f = platform::openFile(list, "rb")) {
        char line[4096];
        while (std::fgets(line, sizeof line, f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (!s.empty()) fs::remove_all(fs::path(platform::widen(s)), ec);
        }
        std::fclose(f);
    }
    fs::remove_all(workDir(), ec);
}

enum class State { Idle, Checking, Current, Available, Installing, Ready, Failed };

class Updater {
public:
    // A local build can't tell what it is, so it has nothing to compare.
    static bool canUpdate() { return kBuild[0] != 0; }

    // Asks GitHub for latest.json, in the background. Nothing happens while a check or install is running.
    void check() {
        if (!canUpdate() || !begin(State::Checking, "Checking for updates…")) return;
        std::shared_ptr<Shared> s = s_;
        std::thread([s] {
            std::string out;
            const std::string url = releaseUrl() + "latest.json";
            const int rc = platform::run(platform::systemTool("curl") + "-fsSL --max-time 30 \"" + url + "\"", {}, out);
            std::lock_guard<std::mutex> l(s->m);
            const std::string build = jsonString(out, "build");
            if (rc != 0 || build.empty()) { s->state = State::Failed; s->text = "Couldn't reach GitHub to check for updates."; return; }
            s->latest = build;
            if (build == kBuild) { s->state = State::Current; s->text = "Frame Studio is up to date."; return; }
            if (out.find("\"" + assetName() + "\"") == std::string::npos) {   // that build has none for this computer (yet)
                s->state = State::Current; s->text = "Frame Studio is up to date.";
                return;
            }
            s->state = State::Available;
            s->text = "A new version of Frame Studio is ready (build " + build.substr(0, 7) + ").";
        }).detach();
    }

    // Downloads the update and puts it in place of this copy, in the background. When it's done: Ready.
    void install() {
        {
            std::lock_guard<std::mutex> l(s_->m);
            if (s_->state != State::Available && s_->state != State::Failed) return;
        }
        if (!begin(State::Installing, "Downloading the update…")) return;
        std::shared_ptr<Shared> s = s_;
        std::thread([s] {
            const std::string err = download(*s);
            std::lock_guard<std::mutex> l(s->m);
            if (!err.empty()) { s->state = State::Failed; s->text = err; return; }
            s->state = State::Ready;
            s->text = "The update is installed. Restart Frame Studio to use it.";
        }).detach();
    }

    State state() const { std::lock_guard<std::mutex> l(s_->m); return s_->state; }
    std::string text() const { std::lock_guard<std::mutex> l(s_->m); return s_->text; }

private:
    struct Shared {
        std::mutex m;
        State state = State::Idle;
        std::string text, latest;
    };
    std::shared_ptr<Shared> s_ = std::make_shared<Shared>();

    bool begin(State st, const char* text) {
        std::lock_guard<std::mutex> l(s_->m);
        if (s_->state == State::Checking || s_->state == State::Installing || s_->state == State::Ready) return false;
        s_->state = st;
        s_->text = text;
        return true;
    }

    static void say(Shared& s, const std::string& t) { std::lock_guard<std::mutex> l(s.m); s.text = t; }

    // The download, unpacked and swapped in. Empty when it worked, else what went wrong (nothing changed then).
    static std::string download(Shared& s) {
        std::error_code ec;
        const fs::path dir = workDir();
        fs::remove_all(dir, ec);
        fs::create_directories(dir / L"unpacked", ec);
        const fs::path file = dir / platform::widen(assetName());
        std::string out;
        const std::string url = releaseUrl() + assetName();
        if (platform::run(platform::systemTool("curl") + "-fsSL --retry 2 --max-time 1800 -o \"" + platform::narrow(file.wstring()) + "\" \"" + url + "\"", {}, out) != 0)
            return "The download didn't finish. Check your connection and try again.";
        say(s, "Unpacking the update…");
        out.clear();
        if (platform::run(platform::systemTool("tar") + "-xf \"" + platform::narrow(file.wstring()) + "\" -C \"" + platform::narrow((dir / L"unpacked").wstring()) + "\"", {}, out) != 0)
            return "The download couldn't be unpacked.";
        const fs::path unpacked = dir / L"unpacked" / platform::widen(assetFolder());
        const std::vector<Swap> swaps = swapsFor(unpacked);
        if (swaps.empty() || !fs::exists(swaps.front().from, ec)) return "The download isn't a Frame Studio build.";

        say(s, "Installing the update…");
        std::vector<Swap> done;                  // what was put in place, to undo on a failure
        std::vector<fs::path> aside;
        auto undo = [&] {
            for (auto it = done.rbegin(); it != done.rend(); ++it) {
                std::error_code e;
                fs::remove_all(it->to, e);
                if (!it->from.empty()) fs::rename(it->from, it->to, e);
            }
        };
        for (const Swap& sw : swaps) {
            fs::path old;
            if (fs::exists(sw.to, ec)) {
                old = sw.to;
                old += L".old";
                fs::remove_all(old, ec);
                ec.clear();
                fs::rename(sw.to, old, ec);
                if (ec) { undo(); return "Frame Studio's folder can't be changed (" + ec.message() + "). Download the update from GitHub instead."; }
                aside.push_back(old);
            }
            done.push_back({old, sw.to});
            ec.clear();
            fs::rename(sw.from, sw.to, ec);            // (another drive than the download: copied instead)
            if (ec) { ec.clear(); fs::copy(sw.from, sw.to, fs::copy_options::recursive, ec); }
            if (ec) { undo(); return "The update couldn't be put in place (" + ec.message() + "). Nothing was changed."; }
        }
#ifndef _WIN32
        fs::permissions(updatedProgram(), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
#endif
        if (FILE* f = platform::openFile(dir / L"aside.txt", "wb")) {
            for (const fs::path& p : aside) std::fprintf(f, "%s\n", platform::narrow(p.wstring()).c_str());
            std::fclose(f);
        }
        fs::remove_all(dir / L"unpacked", ec);
        fs::remove(file, ec);
        return {};
    }
};

}  // namespace updater
