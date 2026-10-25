// The shared slots between Frame Home and the compositor layer (src/env_share.h, layer/frame_home_layer.cpp), hammered:
// a writer publishes frames as fast as it can and resizes every few hundred (a new image generation, the layer
// importing it a while later), while the layer's own liveSlot()/fillUbo() pick and fill frames. Every uniform block the
// layer fills must be one whole frame of the generation whose images it draws: never half of one pose and half of the
// next, never the new generation's pose over the old images.
//   env_share_test [seconds] [thread|process] [legacy]
//     thread:  the writer a thread (run it under -fsanitize=thread)
//     process: the writer another process, over a memfd as the real pair share it
//     legacy:  also run the layer's old way (shared slot read as it is) and show the test catches what it got wrong
// Also: the State layouts of a mismatched pair (an older Frame Home with this layer, and the other way) fail safe.
//
// Build (from the repo): g++ -O1 -g -std=c++17 -fsanitize=thread -Ithird_party/vulkan-headers/include -Isrc
//   tests_loco/env_share_test.cpp -o env_share_test -lpthread
#include "../layer/frame_home_layer.cpp"

#include <sys/wait.h>

#include <cstdio>
#include <random>

namespace {

int g_fails = 0;
void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

// What the writer puts in a slot for frame f of generation g: every word tells which frame it belongs to.
float word(uint64_t f, int i) { return static_cast<float>((f % 100000) * 64 + static_cast<uint64_t>(i)); }
envshare::Slot slotFor(uint64_t f, uint32_t g) {
    envshare::Slot s;
    s.frame = f;
    s.generation = g;
    for (int e = 0; e < 2; ++e) for (int i = 0; i < 16; ++i) s.viewProj[e][i] = word(f, e * 16 + i);
    for (int e = 0; e < 2; ++e) { for (int c = 0; c < 3; ++c) s.eyePos[e][c] = static_cast<float>(g); s.eyePos[e][3] = word(f, 32 + e); }
    return s;
}
envshare::Motion motionFor(uint64_t f, uint32_t g) {
    envshare::Motion m;
    m.targetNs = 0;                          // (no carrying on: the block then holds the slot's pose exactly)
    m.yawRate = static_cast<float>(g);
    m.depth = 0;
    m.velocity[0] = word(f, 40);
    return m;
}

// Shared between the writer and the reader beside the State: the generation the writer has made images for (the
// "Hello" the layer imports when it gets round to it), and stop.
struct Control {
    std::atomic<uint32_t> helloGen{1};
    std::atomic<uint32_t> stop{0};
    std::atomic<uint64_t> published{0}, resizes{0};
};

// Frame Home: publishes into the slots in turn (EnvExport::publish), a resize every `every` frames. `legacy`: as a
// v6 Frame Home did (plain stores, no sequence counts).
void writer(envshare::State* st, Control* c, bool legacy, uint32_t every) {
    uint64_t frame = 0;
    uint32_t gen = 1, next = 0;
    while (!c->stop.load(std::memory_order_relaxed)) {
        if (++frame % every == 0) {   // createImages(): a new generation; the Hello goes out (the layer imports later)
            ++gen;
            c->helloGen.store(gen);
            c->resizes.fetch_add(1, std::memory_order_relaxed);
        }
        const uint32_t k = next;
        next = (next + 1) % envshare::kSlots;
        const envshare::Slot s = slotFor(frame, gen);
        const envshare::Motion m = motionFor(frame, gen);
        if (legacy) {
            envshare::storeWords(&st->slot[k], &s, sizeof s);         // (word by word, no count: as torn as memcpy)
            envshare::storeWords(&st->motion[k], &m, sizeof m);
        } else {
            envshare::writeSlot(st, k, s, m);
        }
        st->published.store(frame);
        st->latest.store(k, std::memory_order_release);
        st->publishNs.store(envshare::nowNs());
        c->published.store(frame, std::memory_order_relaxed);
    }
}

struct Result { uint64_t blocks = 0, torn = 0, wrongGen = 0, imports = 0, shownStale = 0; };

// The layer: the real liveSlot() + fillUbo() (frame_home_layer.cpp), importing each new generation a random while
// after its Hello (the compositor thread gets to it at its next submit). `legacy`: the layer's old reading (the shared
// slot as it is at fill time, its generation unchecked) instead, to show what this test catches.
Result reader(envshare::State* st, Control* c, double seconds, bool seqlocked, bool legacyRead, uint32_t seed) {
    Result r;
    static float ubo[envshare::kSlots][kUboRing * kUboStride / 4];
    Gen fake;
    {
        std::lock_guard<std::mutex> ol(g_oursMutex);
        g_ours = Ours();
        for (uint32_t i = 0; i < envshare::kSlots; ++i) g_ours.ubo[i].mapped = ubo[i];
        g_ours.cur = &fake;
        g_ours.state = st;
        g_ours.stateSize = sizeof(envshare::State);
        g_ours.hasMotion = true;
        g_ours.seqlocked = seqlocked;
        g_ours.generation = 1;
        g_imported = true;
        g_connected = true;
        g_off = false;
    }
    std::mt19937 rng(seed);
    uint64_t importAt = 0;
    const uint64_t end = envshare::nowNs() + static_cast<uint64_t>(seconds * 1e9);
    uint32_t ring = 0;
    while (envshare::nowNs() < end) {
        std::lock_guard<std::mutex> ol(g_oursMutex);
        const uint32_t hello = c->helloGen.load();
        if (hello != g_ours.generation) {   // importPending(), a little later than the Hello came
            const uint64_t now = envshare::nowNs();
            if (!importAt) importAt = now + (rng() % 3000) * 1000ull;   // 0..3 ms
            if (now >= importAt) {
                importAt = 0;
                g_ours.generation = hello;
                g_ours.lastGood = -1;
                for (auto& s : g_ours.shown) s = envshare::Slot();
                for (auto& m : g_ours.shownMotion) m = envshare::Motion();
                ++r.imports;
            }
        }
        g_latchNs = envshare::nowNs();
        const int k = liveSlot();
        if (k < 0) continue;
        ring = (ring + 1) % kUboRing;
        if (legacyRead) {   // the old fillUbo: whatever the shared slot holds now
            g_ours.shown[k] = envshare::Slot();
            std::memcpy(&g_ours.shown[k], &st->slot[k], sizeof(envshare::Slot));   // (racy on purpose: legacy mode only)
            g_ours.shownMotion[k] = envshare::Motion();
        }
        fillUbo(k, ring);
        const float* d = ubo[k] + ring * kUboStride / 4;
        ++r.blocks;
        // One frame throughout: viewProj and eyePos's w all of the frame word 0 says.
        const uint64_t f = static_cast<uint64_t>(d[0]) / 64;
        bool whole = true;
        for (int i = 0; i < 32; ++i) whole &= d[i] == word(f, i);
        whole &= d[32 + 3] == word(f, 32) && d[36 + 3] == word(f, 33);
        if (!whole) ++r.torn;
        // Of the generation whose images are drawn (eyePos.xyz carry the generation the writer rendered it for).
        if (d[32] != static_cast<float>(g_ours.generation) || d[36] != static_cast<float>(g_ours.generation)) ++r.wrongGen;
    }
    {
        std::lock_guard<std::mutex> ol(g_oursMutex);
        g_ours.cur = nullptr;
        g_ours.state = nullptr;
        g_imported = false;
        for (auto& u : g_ours.ubo) u.mapped = nullptr;
    }
    return r;
}

void report(const char* what, const Result& r, const Control& c) {
    std::printf("  %s: %llu frames published, %llu resizes, %llu imports, %llu blocks filled: %llu torn, %llu of the wrong generation\n", what,
                static_cast<unsigned long long>(c.published.load()), static_cast<unsigned long long>(c.resizes.load()),
                static_cast<unsigned long long>(r.imports), static_cast<unsigned long long>(r.blocks), static_cast<unsigned long long>(r.torn),
                static_cast<unsigned long long>(r.wrongGen));
}

// One run: the writer as a thread or a process.
Result run(double seconds, bool process, bool legacyWriter, bool seqlocked, bool legacyRead, Control*& ctl) {
    void* mem = nullptr;
    const size_t size = sizeof(envshare::State) + sizeof(Control);
    int fd = -1;
    if (process) {
        fd = memfd_create("env_share_test", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) { std::perror("memfd"); std::exit(2); }
        mem = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    } else {
        mem = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    }
    auto* st = new (mem) envshare::State();
    st->show = 1;
    ctl = new (static_cast<char*>(mem) + sizeof(envshare::State)) Control();
    Result r;
    if (process) {
        const pid_t pid = fork();
        if (pid == 0) { writer(st, ctl, legacyWriter, 300); _exit(0); }
        r = reader(st, ctl, seconds, seqlocked, legacyRead, 7);
        ctl->stop = 1;
        int status = 0;
        waitpid(pid, &status, 0);
    } else {
        std::thread w(writer, st, ctl, legacyWriter, 300);
        r = reader(st, ctl, seconds, seqlocked, legacyRead, 7);
        ctl->stop = 1;
        w.join();
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 3.0;
    const bool process = argc > 2 && !std::strcmp(argv[2], "process");
    const bool legacy = process && argc > 3 && !std::strcmp(argv[3], "legacy");   // (racy by design: processes only)
    std::printf("env_share_test: %.1f s per run, writer as a %s\n", seconds, process ? "process" : "thread");

    // Mismatched pairs: what the layer maps of each State a Frame Home might send.
    {
        const auto v5 = envshare::stateLayout(5, envshare::kStateV5Size), v6 = envshare::stateLayout(6, envshare::kStateV6Size),
                   v7 = envshare::stateLayout(6, sizeof(envshare::State)), shortV6 = envshare::stateLayout(6, envshare::kStateV6Size - 8),
                   shortV5 = envshare::stateLayout(5, envshare::kStateV5Size - 1), v5big = envshare::stateLayout(5, sizeof(envshare::State));
        expect(v5.size == envshare::kStateV5Size && !v5.motion, "an older (v5) Frame Home's State: mapped to its end, no Motion");
        expect(v6.size == envshare::kStateV6Size && v6.motion, "a v6 Frame Home's State: mapped to its end (no sequence counts)");
        expect(v7.size == sizeof(envshare::State) && v7.motion, "this Frame Home's State: all of it");
        expect(!shortV6.size && !shortV5.size, "a State shorter than its Hello says: not mapped at all");
        expect(v5big.size == envshare::kStateV5Size, "a v5 Hello with a bigger State: only v5's part read");
        envshare::State s;
        expect(envshare::seqlocked(&s, sizeof s), "this Frame Home's State: read under its sequence counts");
        s.version = 6;
        expect(!envshare::seqlocked(&s, sizeof s), "a State that says v6: read the old way (two copies compared)");
        s.version = envshare::kStateVersion;
        expect(!envshare::seqlocked(&s, envshare::kStateV6Size), "a v6-sized mapping: no sequence counts read past its end");
        // The Hello itself didn't change: an older layer (which takes v5 and v6 Hellos only) still pairs with this one.
        envshare::Hello h;
        expect(h.version == 6, "the Hello is still v6 (an older layer accepts it)");
        expect(offsetof(envshare::State, motion) == envshare::kStateV5Size && offsetof(envshare::State, seq) == envshare::kStateV6Size,
               "the v7 State only adds at its end (an older layer reads the same offsets)");
    }

    // This layer with this Frame Home.
    Control* c = nullptr;
    Result r = run(seconds, process, false, true, false, c);
    report("sequence counts + the layer's own copies", r, *c);
    expect(r.blocks > 1000 && c->resizes.load() > 10 && r.imports > 10, "...hammered (many frames, resizes and imports)");
    expect(r.torn == 0, "...no uniform block mixes two frames");
    expect(r.wrongGen == 0, "...no block holds another generation's pose than the images drawn");

    if (process) {   // (a plain-store writer is a data race by design: not under TSan's eye in thread mode)
        r = run(seconds, process, true, false, false, c);
        report("an older (v6) Frame Home: no counts, copies compared", r, *c);
        expect(r.wrongGen == 0, "...never another generation's pose (torn copies are rarer, not ruled out: reported)");
    }
    if (legacy) {
        r = run(seconds, process, true, false, true, c);
        report("the old layer (the shared slot read as it is at fill time)", r, *c);
        expect(r.torn + r.wrongGen > 0, "...the test sees what the old way got wrong");
    }
    std::printf("%d failed\n", g_fails);
    return g_fails ? 1 : 0;
}
