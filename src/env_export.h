#pragma once
// Frame Home's side of the system environment (env_share.h): the shared state, the exported frame images and the
// connection to the compositor layer. frame-home --system renders into slot(), then publish() once the GPU is done.
#include "env_share.h"
#include "hsl_renderer.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>

// Windows system mode: Frame Home renders the home into the compositor's own Direct3D 11 textures (see env_share.h).
// Frame Home owns the shared State (a file mapping) and hands the layer its handle; the layer owns the textures and
// hands Frame Home theirs. createImages() does that hand-over over the layer's named pipe and imports the textures
// into the renderer; after that it mirrors the Linux EnvExport.
class EnvExport {
public:
    ~EnvExport() {
        closeTextures();
        if (pipe_ != INVALID_HANDLE_VALUE) ::CloseHandle(pipe_);
        if (state_) ::UnmapViewOfFile(state_);
        if (stateMap_) ::CloseHandle(stateMap_);
    }

    bool init() {
        stateMap_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(envshare::State), nullptr);
        if (!stateMap_) return false;
        void* p = ::MapViewOfFile(stateMap_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(envshare::State));
        if (!p) return false;
        state_ = new (p) envshare::State();
        state_->show = 0;
        return true;
    }
    envshare::State* state() { return state_; }

    // (Re)negotiates the compositor's textures at the renderer's current size and imports them. Called at startup
    // (where it waits a little for the layer to come up) and on every resize. True once the home can be drawn into
    // the compositor's textures.
    bool createImages(HslRenderer& r) {
        r_ = &r;
        const bool first = !everImported_;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(first ? 3 : 0);
        do {
            if (negotiate(r)) return true;
            if (first) ::Sleep(200);
        } while (first && std::chrono::steady_clock::now() < deadline);
        return false;
    }
    bool connected() const { return pipe_ != INVALID_HANDLE_VALUE && imported_; }

    // Keeps the hand-over alive: reconnects and re-imports after the compositor restarts (as the Linux connect()
    // re-sends its Hello). Cheap when already connected.
    bool connect() {
        if (connected()) return true;
        if (!r_) return false;
        return negotiate(*r_);
    }

    // Is the layer still there? (the pipe breaks when the compositor goes away)
    void poll() {
        if (pipe_ == INVALID_HANDLE_VALUE) return;
        DWORD avail = 0;
        if (!::PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) {
            const DWORD e = ::GetLastError();
            if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED || e == ERROR_INVALID_HANDLE) {
                std::fprintf(stderr, "[frame-home] the compositor went away: reconnecting\n");
                dropPipe();
            }
        }
    }

    int beginFrame() { const int k = static_cast<int>(next_); next_ = (next_ + 1) % envshare::kSlots; return k; }

    void publish(int k, const float viewProj[2][16], const float eyePos[2][3]) {
        envshare::Slot& s = state_->slot[k];
        s.frame = ++frame_;
        s.generation = generation_;
        std::memcpy(s.viewProj, viewProj, sizeof s.viewProj);
        for (int e = 0; e < 2; ++e) { for (int c = 0; c < 3; ++c) s.eyePos[e][c] = eyePos[e][c]; s.eyePos[e][3] = 1; }
        state_->published.store(frame_);
        state_->latest.store(static_cast<uint32_t>(k), std::memory_order_release);
        state_->publishNs.store(envshare::nowNs());
    }

    uint32_t refusedFlags() const { return 0; }   // (Windows frames carry no Vulkan create flags for the layer to refuse)

    double sinceSampled() const {
        const uint64_t s = state_->sampledNs.load();
        return s ? (envshare::nowNs() - s) / 1e9 : 1e9;
    }

    void release(HslRenderer&) { dropPipe(); }

private:
    void closeTextures() { for (HANDLE& h : texture_) if (h) { ::CloseHandle(h); h = nullptr; } }
    void dropPipe() {
        if (pipe_ != INVALID_HANDLE_VALUE) { ::CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; }
        imported_ = false;
    }

    // Opens the pipe if need be, asks the layer for textures at this size, imports them. Throttled to once a second
    // while it can't connect. On success the renderer holds the imported images and connected() is true.
    bool negotiate(HslRenderer& r) {
        const auto now = std::chrono::steady_clock::now();
        if (pipe_ == INVALID_HANDLE_VALUE) {
            if (now - lastTry_ < std::chrono::seconds(1)) return false;
            lastTry_ = now;
            pipe_ = ::CreateFileA(envshare::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe_ == INVALID_HANDLE_VALUE) {
                static bool said = false;
                if (!said) { said = true; std::fprintf(stderr, "[frame-home] waiting for the compositor layer (no pipe yet, error %lu)\n", ::GetLastError()); }
                return false;
            }
            DWORD mode = PIPE_READMODE_MESSAGE;
            ::SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr);
        }
        // The State mapping, duplicated into the layer's (pipe server's) process so it can map it.
        DWORD serverPid = 0;
        HANDLE serverProc = nullptr, dupState = nullptr;
        if (!::GetNamedPipeServerProcessId(pipe_, &serverPid) ||
            !(serverProc = ::OpenProcess(PROCESS_DUP_HANDLE, FALSE, serverPid)) ||
            !::DuplicateHandle(::GetCurrentProcess(), stateMap_, serverProc, &dupState, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            if (serverProc) ::CloseHandle(serverProc);
            dropPipe();
            return false;
        }
        ::CloseHandle(serverProc);

        envshare::WinRequest req;
        req.width = r.width();
        req.height = r.height();
        req.format = 87;   // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        req.pid = ::GetCurrentProcessId();
        req.generation = generation_ + 1;
        req.state = reinterpret_cast<uint64_t>(dupState);
        req.stateSize = static_cast<uint32_t>(sizeof(envshare::State));
        DWORD wrote = 0;
        envshare::WinReply reply = {};
        DWORD got = 0;
        if (!::WriteFile(pipe_, &req, sizeof req, &wrote, nullptr) || wrote != sizeof req ||
            !::ReadFile(pipe_, &reply, sizeof reply, &got, nullptr) || got != sizeof reply ||
            reply.magic != envshare::kMagic || !reply.ok || reply.generation != req.generation) {
            std::fprintf(stderr, "[frame-home] the layer didn't hand over textures (wrote %lu read %lu ok %u gen %u/%u)\n",
                         wrote, got, reply.ok, reply.generation, req.generation);
            dropPipe();
            return false;   // (the layer now owns dupState: it closes it, or drops it when the pipe breaks)
        }
        // The compositor's adapter: Frame Home must be on it, or the import silently reads another GPU's memory.
        if (const uint8_t* luid = r.deviceLUID(); std::memcmp(luid, reply.deviceLUID, 8) != 0) {
            static bool said = false;
            if (!said) { said = true; std::fprintf(stderr, "[frame-home] the compositor draws on a different GPU than Frame Home: the home can't be shared with it\n"); }
            dropPipe();
            return false;
        }
        closeTextures();
        for (uint32_t i = 0; i < envshare::kSlots; ++i) texture_[i] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(reply.texture[i]));
        if (!r.importWin32Images(envshare::kSlots, reply.texture, req.width, req.height)) { dropPipe(); return false; }
        generation_ = req.generation;
        imported_ = everImported_ = true;
        std::fprintf(stderr, "[frame-home] the compositor's textures are ours now: Frame Home is the environment (%ux%u per eye)\n", req.width, req.height);
        return true;
    }

    envshare::State* state_ = nullptr;
    HANDLE stateMap_ = nullptr;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE texture_[envshare::kSlots] = {};
    HslRenderer* r_ = nullptr;
    uint32_t next_ = 0, generation_ = 0;
    uint64_t frame_ = 0;
    bool imported_ = false, everImported_ = false;
    std::chrono::steady_clock::time_point lastTry_;
};
#else
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

class EnvExport {
public:
    ~EnvExport() { disconnect(); if (state_) ::munmap(state_, sizeof(envshare::State)); if (stateFd_ >= 0) ::close(stateFd_); }

    bool init() {
        stateFd_ = memfd_create("framehome-env", MFD_CLOEXEC);
        if (stateFd_ < 0 || ftruncate(stateFd_, sizeof(envshare::State)) != 0) return false;
        void* p = ::mmap(nullptr, sizeof(envshare::State), PROT_READ | PROT_WRITE, MAP_SHARED, stateFd_, 0);
        if (p == MAP_FAILED) return false;
        state_ = new (p) envshare::State();
        state_->show = 0;
        return true;
    }
    envshare::State* state() { return state_; }
    // The images go before the renderer's device does (after VR_Shutdown; exit and restart alike).
    void release(HslRenderer& r) { disconnect(); r.destroyExportImages(images_); }
    bool connected() const { return sock_ >= 0 && sent_; }

    // (Re)creates the images at the renderer's size; they go to the layer on the next connect().
    bool createImages(HslRenderer& r) {
        r.exportSlot = -1;
        if (!r.createExportImages(envshare::kSlots, images_, memoryType_, uuid_)) return false;
        usage_ = r.exportUsage();
        flags_ = r.exportCreateFlags();
        w_ = r.width();
        h_ = r.height();
        ++generation_;      // the layer keeps showing the last frame of the old images until it has these
        if (sent_) sent_ = false, lastTry_ = {};
        return true;
    }

    // Connects to the layer (once a second at most) and sends the images; false while there's no layer.
    bool connect() {
        if (connected()) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastTry_ < std::chrono::seconds(1) || images_.empty()) return false;
        lastTry_ = now;
        if (sock_ < 0) {
            const char* rt = std::getenv("XDG_RUNTIME_DIR");
            const std::string path = std::string(rt && *rt ? rt : "/run/user/1000") + "/" + envshare::kSocketName;
            sock_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_un a = {};
            a.sun_family = AF_UNIX;
            std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
            if (sock_ < 0 || ::connect(sock_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { disconnect(); return false; }
        }
        envshare::Hello h;
        h.width = w_;
        h.height = h_;
        h.layers = 2;
        h.format = 43;   // VK_FORMAT_R8G8B8A8_SRGB
        h.memoryType = memoryType_;
        h.generation = generation_;
        h.usage = usage_;
        h.flags = flags_;
        std::memcpy(h.deviceUUID, uuid_, 16);
        int fds[envshare::kSlots + 1];
        for (uint32_t i = 0; i < envshare::kSlots; ++i) { h.memorySize[i] = images_[i].size; fds[i] = images_[i].fd; }
        fds[envshare::kSlots] = stateFd_;
        iovec iov = {&h, sizeof h};
        char ctrl[CMSG_SPACE(sizeof fds)] = {};
        msghdr m = {};
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = ctrl;
        m.msg_controllen = sizeof ctrl;
        cmsghdr* cm = CMSG_FIRSTHDR(&m);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof fds);
        std::memcpy(CMSG_DATA(cm), fds, sizeof fds);
        if (::sendmsg(sock_, &m, MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof h)) { disconnect(); return false; }
        sent_ = true;
        std::fprintf(stderr, "[frame-home] connected to the compositor: Frame Home is the environment (%ux%u per eye)\n", w_, h_);
        return true;
    }

    // Is the layer still there? (it closes the socket when the compositor goes away)
    void poll() {
        if (sock_ < 0) return;
        char b;
        const ssize_t n = ::recv(sock_, &b, 1, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            std::fprintf(stderr, "[frame-home] the compositor went away: reconnecting\n");
            disconnect();
        }
    }

    // The slot to render the next frame into.
    int beginFrame() { const int k = static_cast<int>(next_); next_ = (next_ + 1) % envshare::kSlots; return k; }

    // The frame in slot k is done on the GPU: hand it to the layer with the eye poses it was rendered from.
    void publish(int k, const float viewProj[2][16], const float eyePos[2][3]) {
        envshare::Slot& s = state_->slot[k];
        s.frame = ++frame_;
        s.generation = generation_;
        std::memcpy(s.viewProj, viewProj, sizeof s.viewProj);
        for (int e = 0; e < 2; ++e) { for (int k = 0; k < 3; ++k) s.eyePos[e][k] = eyePos[e][k]; s.eyePos[e][3] = 1; }
        state_->published.store(frame_);
        state_->latest.store(static_cast<uint32_t>(k), std::memory_order_release);
        state_->publishNs.store(envshare::nowNs());
    }

    // The layer couldn't import the images with these create flags (0: none refused).
    uint32_t refusedFlags() const { return state_->refusedFlags.load() & flags_; }

    // Seconds since the layer last drew the home (large while a game runs, or before the layer is there).
    double sinceSampled() const {
        const uint64_t s = state_->sampledNs.load();
        return s ? (envshare::nowNs() - s) / 1e9 : 1e9;
    }

private:
    void disconnect() { if (sock_ >= 0) ::close(sock_); sock_ = -1; sent_ = false; }
    envshare::State* state_ = nullptr;
    int stateFd_ = -1, sock_ = -1;
    bool sent_ = false;
    std::vector<HslRenderer::ExportImage> images_;
    uint32_t memoryType_ = 0, w_ = 0, h_ = 0, next_ = 0, usage_ = 0, flags_ = 0;
    uint8_t uuid_[16] = {};
    uint64_t frame_ = 0;
    uint32_t generation_ = 0;
    std::chrono::steady_clock::time_point lastTry_;
};
#endif  // _WIN32
