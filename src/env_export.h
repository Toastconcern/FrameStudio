#pragma once
// Frame Home's side of the system environment (env_share.h): the shared state, the exported frame images and the
// connection to the compositor layer. frame-home --system renders into slot(), then publish() once the GPU is done.
#include "env_share.h"
#include "hsl_renderer.h"

#ifdef _WIN32
// A PC has no compositor layer: Frame Home is SteamVR's scene app there, so system mode never starts (init() fails).
class EnvExport {
public:
    bool init() { return false; }
    envshare::State* state() { return &state_; }
    void release(HslRenderer&) {}
    bool connected() const { return false; }
    bool createImages(HslRenderer&) { return false; }
    bool connect() { return false; }
    void poll() {}
    int beginFrame() { return 0; }
    void publish(int, const float[2][16], const float[2][3]) {}
    uint32_t refusedFlags() const { return 0; }
    double sinceSampled() const { return 1e9; }
private:
    envshare::State state_;
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
