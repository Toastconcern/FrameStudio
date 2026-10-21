// bundletest <workdir> a.fenv b.fenv [c.fenv...] [--big]: environment bundles (fenv_bundle.h) against real
// environments; see CMakeLists.txt. Prints each check, exits 1 when any failed.
#include "fenv_bundle.h"

#include <cstdio>

namespace fs = std::filesystem;

namespace {

int g_failed = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failed;
}

bool sameBytes(const fs::path& a, const fs::path& b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> ba(1 << 20), bb(1 << 20);
    for (;;) {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        if (fa.gcount() != fb.gcount() || std::memcmp(ba.data(), bb.data(), static_cast<size_t>(fa.gcount())) != 0) return false;
        if (!fa) return !fb;
    }
}

std::vector<std::string> filesIn(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) names.push_back(e.path().filename().u8string());
    std::sort(names.begin(), names.end());
    return names;
}

// Made-up environment bytes, the same for the same offset (so any byte read from the wrong place shows).
struct PatternSource {
    uint64_t size = 0;
    static size_t read(void* opaque, mz_uint64 offset, void* buffer, size_t n) {
        const uint64_t size = static_cast<PatternSource*>(opaque)->size;
        n = offset >= size ? 0 : static_cast<size_t>(std::min<uint64_t>(n, size - offset));   // (0 at the end: the writer's sign to stop)
        auto* out = static_cast<unsigned char*>(buffer);
        for (size_t i = 0; i < n; ++i) {
            uint64_t x = (offset + i) / 8 * 0x9E3779B97F4A7C15ull;
            x ^= x >> 29;
            out[i] = static_cast<unsigned char>(x >> (8 * ((offset + i) % 8)));
        }
        return n;
    }
};

// A bundle bigger than 4 GB: two copies of a 2.3 GB environment, so the second starts past the 32-bit offsets.
void bigBundle(const fs::path& work) {
    const fs::path bigFenv = work / "big.fenv", bundlePath = work / "big.fbundle", outDir = work / "big-out";
    std::error_code ec;
    const uint64_t sceneSize = (uint64_t(23) << 30) / 10;
    {
        mz_zip_archive z = {};
        PatternSource pattern;
        pattern.size = sceneSize;
        FenvManifest m;
        m.name = "Big";
        m.scene = "scene.bin";
        const std::string json = fenv_detail::manifestJson(m);
        bool ok = mz_zip_writer_init_file_v2(&z, bigFenv.c_str(), 0, 0) &&
                  mz_zip_writer_add_mem(&z, "fenv.json", json.data(), json.size(), MZ_DEFAULT_COMPRESSION) &&
                  mz_zip_writer_add_read_buf_callback(&z, "scene.bin", &PatternSource::read, &pattern, sceneSize, nullptr, nullptr, 0, MZ_NO_COMPRESSION,
                                                      nullptr, 0, nullptr, 0) &&
                  mz_zip_writer_finalize_archive(&z);
        const std::string why = mz_zip_get_error_string(mz_zip_get_last_error(&z));
        mz_zip_writer_end(&z);
        check(ok, "big: made a 2.3 GB environment" + (ok ? std::string() : " (" + why + ")"));
        if (!ok) return;
    }
    std::string err;
    uint64_t lastDone = 0, lastTotal = 0;
    bool ok = fenvBundleWrite(bundlePath.u8string(), {bigFenv.u8string(), bigFenv.u8string()}, "Big", err,
                              [&](uint64_t d, uint64_t t) { lastDone = d; lastTotal = t; });
    check(ok, "big: bundled " + err);
    const uint64_t bundleSize = fs::file_size(bundlePath, ec);
    check(bundleSize > (uint64_t(4) << 30), "big: bundle is " + std::to_string(bundleSize >> 20) + " MB (> 4 GB)");
    check(lastTotal == 2 * fs::file_size(bigFenv, ec) && lastDone == lastTotal, "big: progress reached the total");
    {   // a zip64 end of central directory record is there (signature PK\6\6)
        std::ifstream f(bundlePath, std::ios::binary);
        f.seekg(-4096, std::ios::end);
        std::string tail(4096, '\0');
        f.read(&tail[0], 4096);
        check(tail.find(std::string("PK\x06\x06", 4)) != std::string::npos, "big: zip64 end record present");
    }
    FenvBundleManifest bm;
    check(fenvBundleReadManifest(bundlePath.u8string(), bm) && bm.environments.size() == 2 && bm.environments[1].file == "environments/big 2.fenv",
          "big: manifest lists big + big 2");
    FenvManifest second;
    check(fenvBundleReadEnvironment(bundlePath.u8string(), "environments/big 2.fenv", second) && second.name == "Big",
          "big: the member past 4 GB reads in place");
    ok = fenvBundleValidate(bundlePath.u8string(), err);
    check(ok, "big: validates with checksums " + (ok ? std::string() : err));
    std::vector<std::string> unpacked;
    ok = fenvBundleExtract(bundlePath.u8string(), outDir.u8string(), unpacked, err, "environments/big 2.fenv");
    check(ok && unpacked.size() == 1 && sameBytes(unpacked[0], bigFenv), "big: the member past 4 GB unpacks byte for byte");
    fs::remove(bigFenv, ec);
    fs::remove(bundlePath, ec);
    fs::remove_all(outDir, ec);
}

// A copy of an environment with a thumbnail added (the converted test homes have none).
bool withThumbnail(const std::string& from, const fs::path& to, std::vector<uint8_t>& thumb) {
    FenvManifest m;
    if (!fenvReadManifest(from, m)) return false;
    m.thumbnail = "thumb.png";
    thumb.resize(3000);
    for (size_t i = 0; i < thumb.size(); ++i) thumb[i] = static_cast<uint8_t>(i * 7 + 1);
    mz_zip_archive in = {}, out = {};
    if (!mz_zip_reader_init_file(&in, from.c_str(), 0)) return false;
    const std::string json = fenv_detail::manifestJson(m);
    bool ok = mz_zip_writer_init_file_v2(&out, to.c_str(), 0, 0) &&
              mz_zip_writer_add_mem(&out, "fenv.json", json.data(), json.size(), MZ_DEFAULT_COMPRESSION) &&
              mz_zip_writer_add_mem(&out, "thumb.png", thumb.data(), thumb.size(), MZ_NO_COMPRESSION);
    for (mz_uint i = 0; ok && i < mz_zip_reader_get_num_files(&in); ++i) {
        char name[512];
        mz_zip_reader_get_filename(&in, i, name, sizeof name);
        if (std::strcmp(name, "fenv.json") != 0) ok = mz_zip_writer_add_from_zip_reader(&out, &in, i);
    }
    ok = ok && mz_zip_writer_finalize_archive(&out);
    mz_zip_writer_end(&out);
    mz_zip_reader_end(&in);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: bundletest <workdir> a.fenv b.fenv [c.fenv...] [--big]\n"); return 2; }
    const fs::path work = fs::u8path(argv[1]);
    std::vector<std::string> originals;
    bool big = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--big")) big = true;
        else originals.push_back(argv[i]);
    }
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    std::string err;

    // Bundled: every original, one with a thumbnail, then the first again (a second file of the same name).
    std::vector<uint8_t> thumb;
    const fs::path thumbFenv = work / "with-thumbnail.fenv";
    check(withThumbnail(originals.front(), thumbFenv, thumb), "made " + thumbFenv.filename().u8string());
    std::vector<std::string> inputs = originals;
    inputs.push_back(thumbFenv.u8string());
    inputs.push_back(originals.front());
    const std::string bundlePath = (work / "test.fbundle").u8string();
    uint64_t total = 0, lastDone = 0, lastTotal = 0, calls = 0;
    for (const auto& f : inputs) total += fs::file_size(fs::u8path(f), ec);
    bool ok = fenvBundleWrite(bundlePath, inputs, "Test bundle", err, [&](uint64_t d, uint64_t t) { lastDone = d; lastTotal = t; ++calls; });
    check(ok, "bundle written " + err);
    check(lastDone == total && lastTotal == total, "progress reached " + std::to_string(total) + " bytes in " + std::to_string(calls) + " calls");
    check(!fs::exists(bundlePath + ".part", ec), "no .part left");
    check(fs::file_size(fs::u8path(bundlePath), ec) < total + 4096, "members stored (bundle " + std::to_string(fs::file_size(fs::u8path(bundlePath), ec)) +
                                                                       " bytes for " + std::to_string(total) + ")");

    FenvBundleManifest bm;
    ok = fenvBundleReadManifest(bundlePath, bm, &err);
    check(ok && bm.name == "Test bundle" && bm.environments.size() == inputs.size(), "manifest: name and " + std::to_string(bm.environments.size()) + " environments");
    const std::string firstStem = fs::u8path(originals.front()).stem().u8string();
    for (size_t i = 0; ok && i < inputs.size(); ++i) {
        const FenvBundleEntry& e = bm.environments[i];
        const std::string want = "environments/" + (i + 1 == inputs.size() ? firstStem + " 2" : fs::u8path(inputs[i]).stem().u8string()) + ".fenv";
        FenvManifest original, inBundle;
        std::vector<uint8_t> originalThumb, bundleThumb;
        fenvReadManifest(inputs[i], original, &originalThumb);
        const bool read = fenvBundleReadEnvironment(bundlePath, e.file, inBundle, &bundleThumb, &err);
        check(e.file == want && e.size == fs::file_size(fs::u8path(inputs[i]), ec) && e.name == original.name,
              "entry " + e.file + " \"" + e.name + "\" " + std::to_string(e.size) + " bytes");
        check(read && inBundle.name == original.name && inBundle.scene == original.scene && bundleThumb == originalThumb &&
                  (inputs[i] != thumbFenv.u8string() || bundleThumb == thumb),
              "  its fenv.json + thumbnail (" + std::to_string(bundleThumb.size()) + " bytes) read in place");
    }
    ok = fenvBundleValidate(bundlePath, err);
    check(ok, "validates with checksums " + (ok ? std::string() : err));

    // Unpacked: byte for byte the originals.
    const fs::path out = work / "out";
    std::vector<std::string> unpacked;
    ok = fenvBundleExtract(bundlePath, out.u8string(), unpacked, err);
    check(ok && unpacked.size() == inputs.size(), "unpacked " + std::to_string(unpacked.size()) + " " + err);
    for (size_t i = 0; ok && i < inputs.size(); ++i)
        check(sameBytes(unpacked[i], inputs[i]), "  " + fs::u8path(unpacked[i]).filename().u8string() + " == " + inputs[i]);
    const std::vector<std::string> before = filesIn(out);
    std::vector<std::string> again;
    ok = fenvBundleExtract(bundlePath, out.u8string(), again, err);
    check(ok && again == unpacked && filesIn(out) == before, "unpacked again: the same files, nothing new");

    // Beside a different file of the same name: that file stays, this one becomes "<name> 2".
    const fs::path clash = work / "clash";
    fs::create_directories(clash, ec);
    std::ofstream(clash / (firstStem + ".fenv")) << "someone else's";
    ok = fenvBundleExtract(bundlePath, clash.u8string(), unpacked, err, "environments/" + firstStem + ".fenv");
    check(ok && unpacked.size() == 1 && fs::u8path(unpacked[0]).filename().u8string() == firstStem + " 2.fenv" && sameBytes(unpacked[0], originals.front()) &&
              fs::file_size(clash / (firstStem + ".fenv"), ec) == std::string("someone else's").size(),
          "one member beside a different same-named file -> \"" + firstStem + " 2.fenv\"");

    // Refused: what isn't an environment (and nothing left behind).
    std::ofstream(work / "fake.fenv") << "not a zip";
    std::ofstream(work / "notes.txt") << "text";
    const std::string refused = (work / "refused.fbundle").u8string();
    const std::vector<std::pair<std::string, std::string>> notEnvironments = {
        {"a broken .fenv", (work / "fake.fenv").u8string()}, {"a non-.fenv", (work / "notes.txt").u8string()}, {"a missing file", (work / "missing.fenv").u8string()}};
    for (const auto& [what, file] : notEnvironments) {
        ok = fenvBundleWrite(refused, {originals.front(), file}, "", err);
        check(!ok && !fs::exists(refused, ec) && !fs::exists(refused + ".part", ec), "refuses " + what + " (" + err + ")");
    }
    check(!fenvBundleReadManifest(originals.front(), bm), "a .fenv isn't a bundle");
    {   // a damaged bundle: one byte flipped inside a member fails validation and unpacking, leaving no file
        const std::string damaged = (work / "damaged.fbundle").u8string();
        fs::copy_file(fs::u8path(bundlePath), fs::u8path(damaged), ec);
        std::fstream f(fs::u8path(damaged), std::ios::binary | std::ios::in | std::ios::out);
        const auto at = static_cast<std::streamoff>(fs::file_size(fs::u8path(damaged), ec) / 2);
        f.seekg(at);
        char c = 0;
        f.get(c);
        f.seekp(at);
        f.put(static_cast<char>(c ^ 0x5a));
        f.close();
        ok = fenvBundleValidate(damaged, err);
        check(!ok, "a flipped byte fails validation (" + err + ")");
        const fs::path damagedOut = work / "damaged-out";
        ok = fenvBundleExtract(damaged, damagedOut.u8string(), unpacked, err);
        bool partLeft = false;
        for (const auto& n : filesIn(damagedOut)) partLeft = partLeft || n.find(".part") != std::string::npos;
        check(!ok && !partLeft, "  and unpacking stops without leaving a .part (" + err + ")");
    }
    if (big) bigBundle(work);
    std::printf("%s: %d failed\n", g_failed ? "FAILED" : "passed", g_failed);
    return g_failed ? 1 : 0;
}
