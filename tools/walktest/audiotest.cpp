#include "core/audio.h"
#include "hsl_scene.h"
#include <fstream>
#include <iterator>
std::atomic<bool> g_audioMuted{false};
static void report(const char* name, const std::vector<uint8_t>& b) {
    std::vector<short> pcm; int chs = 0, sr = 0; const char* what = "?";
    const bool ok = AudioPlayer::decode(b.data(), b.size(), pcm, chs, sr, &what);
    double e = 0; for (short v : pcm) e += double(v) * v;
    std::printf("%-22s %-10s ok=%d ch=%d sr=%d %.2fs rms=%.0f\n", name, ok ? what : "-", ok, chs, sr, ok ? pcm.size() / double(chs) / sr : 0, pcm.empty() ? 0 : std::sqrt(e / pcm.size()));
}
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.size() > 4 && a.substr(a.size() - 4) == ".apk") {
            HslScene s; std::string err;
            if (!loadHslScene(a, s, err)) { std::printf("load fail\n"); continue; }
            report(("home:" + s.ambientSource).c_str(), s.ambientOgg);
            continue;
        }
        std::ifstream f(a, std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        report(a.substr(a.rfind('/') + 1).c_str(), b);
    }
}
