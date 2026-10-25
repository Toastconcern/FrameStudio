// Frame Studio's own updates. Every push to main replaces the GitHub release "latest", and with it latest.json:
//   { "build": "<commit>", "home": "<Frame Home's hash>", "files": ["FrameStudio-windows-x64.zip", ...] }
// A build from another commit than this one (FRAME_BUILD, set by CI) is offered. Installing downloads this computer's
// file, unpacks it, and puts each thing in it where the old one was (the old one set aside: the running copy keeps
// going, and a failure puts everything back). Frame Studio then restarts into it; the next start deletes
// what was set aside. A local build (no FRAME_BUILD) never updates itself.
//
// One update at a time: two Studios open share update/, so whichever installs holds update.lock (beside it) for the
// whole download and swap, and the other is told to wait; a start doesn't tidy update/ while another holds it. And a
// swap that stops halfway (a crash, a power cut) is finished or undone on the next start: before anything is moved,
// update/journal.txt lists every move, the program itself last. Started from the new program, everything else was
// already in place: the swap is finished. Started from the old one, it never got that far: every move is put back.
// A file's path is never empty along the way (or a crash there would leave no program to start and do that): the
// new one is staged beside it as "<name>.new", the old one gets a second name ("<name>.old", a hard link), and the
// new one then replaces it in one move. A folder is renamed aside and the new one moved in; on macOS the .app is
// swapped with the new one in one move.
#pragma once

#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#ifdef __APPLE__
#include <stdio.h>   // renamex_np
#endif
#endif
#include <algorithm>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// update.lock, held while one Studio updates (or tidies up after one): never waited for, so a second Studio
// doesn't hang behind a download, it's just told.
class Lock {
public:
    bool take() {
        std::error_code ec;
        const fs::path p = platform::userDataDir() / L"FrameStudio" / L"update.lock";
        fs::create_directories(p.parent_path(), ec);
#ifdef _WIN32
        h_ = CreateFileW(p.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return false;
        if (!LockFileEx(h_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &ov_)) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; return false; }
#else
        fd_ = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) return false;
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) { ::close(fd_); fd_ = -1; return false; }
#endif
        return true;
    }
    Lock() = default;
    ~Lock() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) { UnlockFileEx(h_, 0, MAXDWORD, MAXDWORD, &ov_); CloseHandle(h_); }
#else
        if (fd_ >= 0) { ::flock(fd_, LOCK_UN); ::close(fd_); }
#endif
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
    OVERLAPPED ov_ = {};
#else
    int fd_ = -1;
#endif
};

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

// A file's SHA-256, as 64 lowercase hex digits ("" when it can't be read). What latest.json says each download's is
// ("sha256-<file>"): an update is only unpacked when its download matches, so a cut-off, damaged or swapped download
// is never installed.
inline std::string sha256File(const fs::path& path) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto block = [&](const unsigned char* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    FILE* file = platform::openFile(path, "rb");
    if (!file) return {};
    std::vector<unsigned char> buf(1 << 16);
    uint64_t total = 0;
    size_t have = 0;                                   // bytes in buf not yet hashed (less than a block)
    for (;;) {
        const size_t n = std::fread(buf.data() + have, 1, buf.size() - have, file);
        if (n == 0) break;
        total += n;
        have += n;
        size_t at = 0;
        for (; at + 64 <= have; at += 64) block(buf.data() + at);
        std::memmove(buf.data(), buf.data() + at, have - at);
        have -= at;
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) return {};
    unsigned char tail[128] = {};
    std::memcpy(tail, buf.data(), have);
    tail[have] = 0x80;
    const size_t len = have + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = total * 8;
    for (int i = 0; i < 8; ++i) tail[len - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));
    for (size_t at = 0; at < len; at += 64) block(tail + at);
    std::string hex;
    char b[9];
    for (uint32_t v : h) { std::snprintf(b, sizeof b, "%08x", v); hex += b; }
    return hex;
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
    // The program last (see the journal, above): while it's still the old one, a start runs the old one.
    const fs::path program = updatedProgram();
    std::stable_partition(out.begin(), out.end(), [&](const Swap& sw) { return sw.to != program; });
    return out;
}

// update/journal.txt: what a swap moves, written (whole, then renamed into place) before the first move.
//   frame-studio-update 1
//   build <the new build>
//   <1 if `to` was there before, else 0> TAB <to> TAB <where the old one goes> TAB <the new one>
struct JournalEntry { bool had = false; fs::path to, old, from; };
inline fs::path journalPath() { return workDir() / L"journal.txt"; }
inline bool writeJournal(const std::string& build, const std::vector<JournalEntry>& entries) {
    const fs::path tmp = workDir() / L"journal.txt.part";
    FILE* f = platform::openFile(tmp, "wb");
    if (!f) return false;
    std::fprintf(f, "frame-studio-update 1\nbuild %s\n", build.c_str());
    for (const JournalEntry& e : entries)
        std::fprintf(f, "%d\t%s\t%s\t%s\n", e.had ? 1 : 0, platform::narrow(e.to.wstring()).c_str(), platform::narrow(e.old.wstring()).c_str(),
                     platform::narrow(e.from.wstring()).c_str());
    const bool ok = std::fflush(f) == 0 && !std::ferror(f);
    std::fclose(f);
    std::error_code ec;
    if (ok) fs::rename(tmp, journalPath(), ec);
    return ok && !ec;
}
inline bool readJournal(std::string& build, std::vector<JournalEntry>& entries) {
    FILE* f = platform::openFile(journalPath(), "rb");
    if (!f) return false;
    std::vector<std::string> lines;
    char line[8192];
    while (std::fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        lines.push_back(s);
    }
    std::fclose(f);
    if (lines.size() < 2 || lines[0] != "frame-studio-update 1" || lines[1].rfind("build ", 0) != 0) return false;
    build = lines[1].substr(6);
    for (size_t i = 2; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        const size_t a = l.find('\t'), b = a == std::string::npos ? a : l.find('\t', a + 1), c = b == std::string::npos ? b : l.find('\t', b + 1);
        if (c == std::string::npos || a != 1 || (l[0] != '0' && l[0] != '1')) return false;
        JournalEntry e;
        e.had = l[0] == '1';
        e.to = fs::path(platform::widen(l.substr(a + 1, b - a - 1)));
        e.old = fs::path(platform::widen(l.substr(b + 1, c - b - 1)));
        e.from = fs::path(platform::widen(l.substr(c + 1)));
        if (e.to.empty() || (e.had && e.old.empty())) return false;
        entries.push_back(e);
    }
    return true;
}

// A swap's stand-in for a crash, for the updater test: swapIn() stops dead (no undo, the journal left) after this
// many more moves. -1: never.
inline int& stopAfterMoves() { static int n = -1; return n; }
inline bool stopNow() { int& n = stopAfterMoves(); return n >= 0 && n-- == 0; }
inline constexpr const char* kStopped = "(stopped by the test)";

inline fs::path withSuffix(fs::path p, const wchar_t* suffix) { p += suffix; return p; }

// Puts one journal entry back the way it was before the swap, whatever step it stopped at.
inline void putBack(const JournalEntry& e) {
    std::error_code ec;
    fs::remove_all(withSuffix(e.to, L".new"), ec);                // the new one, staged beside it
    if (!e.had) { fs::remove_all(e.to, ec); return; }             // added by the update (it wasn't there before)
    fs::remove_all(withSuffix(e.old, L".part"), ec);              // (a copy of the old one, cut off)
    if (!fs::exists(e.old, ec)) return;                           // never set aside: it's still the old one
    if (fs::exists(e.to, ec) && fs::equivalent(e.to, e.old, ec)) { fs::remove(e.old, ec); return; }   // only linked
    ec.clear();
    if (!fs::is_directory(e.old, ec)) { fs::rename(e.old, e.to, ec); if (!ec) return; }   // one move over the new one
    ec.clear();
    fs::remove_all(e.to, ec);
    ec.clear();
    fs::rename(e.old, e.to, ec);
}

#ifndef _WIN32
// Swaps two paths in one move (both stay there). False: this file system (or system) can't.
inline bool swapPaths(const fs::path& a, const fs::path& b) {
#ifdef __APPLE__
    return ::renamex_np(a.c_str(), b.c_str(), RENAME_SWAP) == 0;
#else
    (void)a; (void)b;
    return false;
#endif
}
#endif

// Adds to update/aside.txt (what the next start deletes): an earlier update's leftovers stay listed too.
inline void noteAside(const std::vector<fs::path>& aside) {
    if (aside.empty()) return;
    if (FILE* f = platform::openFile(workDir() / L"aside.txt", "ab")) {
        for (const fs::path& p : aside) std::fprintf(f, "%s\n", platform::narrow(p.wstring()).c_str());
        std::fclose(f);
    }
}

// A swap a crash stopped halfway (the journal is there). Started from the new program (this build is the
// journal's): everything else was moved before it, so the swap is finished, and what was set aside goes on the list.
// From the old one: every move is put back, last first (a thing the update added is removed, one that never moved
// is left alone). (`kBuild` is passed in: the self-test plays both sides.)
inline void recover(const std::string& thisBuild = kBuild) {
    std::string build;
    std::vector<JournalEntry> entries;
    std::error_code ec;
    if (!fs::exists(journalPath(), ec)) return;
    if (!readJournal(build, entries)) {   // (cut off while written, or not ours: nothing was moved by it)
        fs::remove(journalPath(), ec);
        return;
    }
    if (!build.empty() && build == thisBuild) {
        std::vector<fs::path> aside;
        for (const JournalEntry& e : entries) if (e.had && fs::exists(e.old, ec)) aside.push_back(e.old);
        noteAside(aside);
    } else {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) putBack(*it);
    }
    fs::remove(journalPath(), ec);
    fs::remove_all(workDir() / L"unpacked", ec);
}

// At start: finish or undo a swap a crash interrupted, then delete what the last update set aside (listed in
// update/aside.txt). Not while another Studio is updating (it holds update.lock): update/ is in use.
inline void cleanUp() {
    Lock lock;
    if (!lock.take()) return;
    recover();
    const fs::path list = workDir() / L"aside.txt";
    std::error_code ec;
    if (FILE* f = platform::openFile(list, "rb")) {
        // What can't go yet stays listed for the next start: the copy that restarted into this one may still be
        // running from its old program (Windows won't delete a running .exe), and forgetting it would leave it there.
        std::vector<std::string> left;
        char line[4096];
        while (std::fgets(line, sizeof line, f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty()) continue;
            const fs::path p(platform::widen(s));
            ec.clear();
            fs::remove_all(p, ec);
            if (fs::exists(p, ec)) left.push_back(s);
        }
        std::fclose(f);
        if (left.empty()) { fs::remove_all(workDir(), ec); return; }
        if (FILE* o = platform::openFile(list, "wb")) {
            for (const std::string& s : left) std::fprintf(o, "%s\n", s.c_str());
            std::fclose(o);
        }
        return;
    }
    // No update just made: only what's been lying there a while (another Studio open may be downloading one now).
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(1);
    for (const auto& e : fs::directory_iterator(workDir(), ec))
        if (e.last_write_time(ec) < old) fs::remove_all(e.path(), ec);
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
        Lock lock;   // (held to the end: another Studio's start won't tidy update/ from under this one)
        if (!lock.take()) return "Another Frame Studio window is installing the update now. Restart Frame Studio once it's done.";
        recover();   // (a swap that stopped halfway and wasn't seen to since: put back first)
        const fs::path dir = workDir();
        // A fresh download, keeping the list of what earlier updates set aside (not all of it can go while a Studio
        // started from an older program still runs).
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().filename() != L"aside.txt") { std::error_code e2; fs::remove_all(e.path(), e2); }
        ec.clear();
        fs::create_directories(dir / L"unpacked", ec);
        const fs::path file = dir / platform::widen(assetName());
        std::string out;
        // What the download must be: latest.json read again now (the build may have moved on since the check), its
        // checksum for this computer's file. Without one there's nothing to check the download against: not installed.
        if (platform::run(platform::systemTool("curl") + "-fsSL --max-time 30 \"" + releaseUrl() + "latest.json\"", {}, out) != 0)
            return "Couldn't reach GitHub for the update. Check your connection and try again.";
        const std::string build = jsonString(out, "build");   // (what's downloaded: the journal names it)
        std::string want = jsonString(out, "sha256-" + assetName());
        for (char& c : want) c = static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
        if (want.size() != 64 || want.find_first_not_of("0123456789abcdef") != std::string::npos)
            return "This update has no checksum to check the download against, so it wasn't installed. Download it from GitHub instead.";
        out.clear();
        const std::string url = releaseUrl() + assetName();
        if (platform::run(platform::systemTool("curl") + "-fsSL --retry 2 --max-time 1800 -o \"" + platform::narrow(file.wstring()) + "\" \"" + url + "\"", {}, out) != 0)
            return "The download didn't finish. Check your connection and try again.";
        say(s, "Checking the download…");
        if (sha256File(file) != want) {
            fs::remove_all(dir, ec);
            return "The download was damaged or incomplete (its checksum doesn't match), so nothing was changed. Try again.";
        }
        say(s, "Unpacking the update…");
        out.clear();
        if (platform::run(platform::systemTool("tar") + "-xf \"" + platform::narrow(file.wstring()) + "\" -C \"" + platform::narrow((dir / L"unpacked").wstring()) + "\"", {}, out) != 0)
            return "The download couldn't be unpacked.";
        const fs::path unpacked = dir / L"unpacked" / platform::widen(assetFolder());
        const std::vector<Swap> swaps = swapsFor(unpacked);
        if (swaps.empty() || !fs::exists(swaps.front().from, ec)) return "The download isn't a Frame Studio build.";

        say(s, "Installing the update…");
        const std::string err = swapIn(swaps, build);
        if (!err.empty()) return err;
        fs::remove_all(dir / L"unpacked", ec);
        fs::remove(file, ec);
        return {};
    }

public:
    // Puts each `from` where its `to` is, the old one set aside: journaled (recover() finishes or undoes it if this
    // stops halfway), undone on a failure. Empty when it worked, else what went wrong (nothing changed then).
    static std::string swapIn(const std::vector<Swap>& swaps, const std::string& build) {
        std::error_code ec;
        // Where each old one goes ("<name>.old", or .old1 ... while an earlier one can't be deleted yet), all in the
        // journal before anything moves.
        std::vector<JournalEntry> plan;
        for (const Swap& sw : swaps) {
            JournalEntry e;
            e.to = sw.to;
            e.from = sw.from;
            e.had = fs::exists(sw.to, ec);
            for (int n = 0; e.had && n < 10; ++n) {
                fs::path old = sw.to;
                old += n ? L".old" + std::to_wstring(n) : std::wstring(L".old");
                std::error_code e2;
                fs::remove_all(old, e2);
                if (!fs::exists(old, e2)) { e.old = old; break; }
            }
            if (e.had && e.old.empty()) return "Frame Studio's folder can't be changed (an older copy is still in use). Close every Frame Studio window and try again.";
            std::error_code e2;
            fs::remove_all(withSuffix(sw.to, L".new"), e2);
            if (e.had) fs::remove_all(withSuffix(e.old, L".part"), e2);
            plan.push_back(e);
        }
        if (!writeJournal(build, plan)) return "The update couldn't be started (its folder can't be written). Nothing was changed.";
        std::vector<fs::path> aside;
        // Stopped at plan[i]: it and everything before it put back, last first.
        auto undo = [&](size_t i, const std::string& what) {
            for (size_t j = i + 1; j-- > 0;) putBack(plan[j]);
            std::error_code e;
            fs::remove(journalPath(), e);
            return what;
        };
        const auto cantChange = [](const std::error_code& e) { return "Frame Studio's folder can't be changed (" + e.message() + "). Download the update from GitHub instead."; };
        const auto cantPut = [](const std::error_code& e) { return "The update couldn't be put in place (" + e.message() + "). Nothing was changed."; };
        for (size_t i = 0; i < plan.size(); ++i) {
            const JournalEntry& e = plan[i];
            if (stopNow()) return kStopped;
            ec.clear();
            if (fs::is_directory(e.from, ec)) {
#ifndef _WIN32
                // The .app (macOS): swapped with the new one in one move, then the old one (now where the new was)
                // set aside. (Stopped between: the new one is in place; the old one goes with update/unpacked.)
                if (e.had && e.to == updatedProgram() && swapPaths(e.from, e.to)) {
                    if (stopNow()) return kStopped;
                    ec.clear();
                    fs::rename(e.from, e.old, ec);
                    if (!ec) aside.push_back(e.old);
                    continue;
                }
#endif
                if (e.had) {
                    ec.clear();
                    fs::rename(e.to, e.old, ec);
                    if (ec) return undo(i, cantChange(ec));
                    aside.push_back(e.old);
                    if (stopNow()) return kStopped;
                }
                ec.clear();
                fs::rename(e.from, e.to, ec);            // (another drive than the download: copied instead)
                if (ec) { ec.clear(); fs::copy(e.from, e.to, fs::copy_options::recursive, ec); }
                if (ec) return undo(i, cantPut(ec));
                continue;
            }
            // A file: the new one staged beside it first (moved, or copied from another drive), so the last step
            // is one move on the same drive.
            const fs::path staged = withSuffix(e.to, L".new");
            ec.clear();
            fs::rename(e.from, staged, ec);
            if (ec) { ec.clear(); fs::copy_file(e.from, staged, fs::copy_options::overwrite_existing, ec); }
            if (ec) return undo(i, cantPut(ec));
            if (stopNow()) return kStopped;
            if (e.had) {
                // The old one keeps its path and gets a second name, the one it's set aside under (Windows lets a
                // running program be replaced like this: the file itself stays, under its other name).
                ec.clear();
                fs::create_hard_link(e.to, e.old, ec);
                if (ec) {   // (a drive without hard links: FAT, exFAT)
#ifdef _WIN32
                    // In one call: the old one renamed aside, the new one in its place.
                    if (!ReplaceFileW(e.to.wstring().c_str(), staged.wstring().c_str(), e.old.wstring().c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
                        return undo(i, cantChange(std::error_code(static_cast<int>(GetLastError()), std::system_category())));
                    aside.push_back(e.old);
                    continue;
#else
                    // A copy instead (whole, then renamed: a copy cut off is never taken for the old one).
                    const fs::path part = withSuffix(e.old, L".part");
                    ec.clear();
                    fs::copy_file(e.to, part, fs::copy_options::overwrite_existing, ec);
                    if (!ec) fs::rename(part, e.old, ec);
                    if (ec) return undo(i, cantChange(ec));
#endif
                }
                aside.push_back(e.old);
                if (stopNow()) return kStopped;
            }
            ec.clear();
            fs::rename(staged, e.to, ec);                // (over the old one: its path is never empty)
            if (ec) return undo(i, cantChange(ec));
        }
        if (stopNow()) return kStopped;          // (all moved, the journal still there: the new program finishes it)
#ifndef _WIN32
        fs::permissions(updatedProgram(), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
#endif
        noteAside(aside);
        fs::remove(journalPath(), ec);           // (done: from here a start has nothing to finish or undo)
        return {};
    }
};

}  // namespace updater
