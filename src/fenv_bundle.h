#pragma once
// .fbundle — several Frame environments in one file, to share or install together. Each stays its own environment:
// a bundle is only the carrier. It is a ZIP holding bundle.json and the .fenv files as they are:
//
//   { "format": "fbundle", "version": 1, "name": "Meta homes",
//     "environments": [ { "file": "environments/haven.fenv", "name": "Haven + Calming", "size": 283115520 } ] }
//
// The .fenv members are STORED, not recompressed (their content is compressed already), and copied file to zip and
// zip to file in pieces, so a bundle of any size never passes through memory. A bundle of 4 GB or more is written as
// zip64 from the start (miniz only switches before a file that starts past 4 GB, not for one that ends past it, and
// then can't finish the archive). A member being stored means its bytes sit in the bundle as one run, so a member's
// own fenv.json and thumbnail are read in place, without unpacking anything.
//
// The Steam Frame's switcher unpacks a bundle dropped into ~/frame-home/envs into its environments (switcher.cpp
// unpackBundles); Frame Studio and fenv-tool write, list and unpack them.
#include "fenv.h"

#include <ctime>
#include <fstream>

struct FenvBundleEntry {
    std::string file;                   // member path in the bundle: "environments/<name>.fenv"
    std::string name;                   // the environment's display name (its fenv.json "name")
    uint64_t size = 0;                  // bytes
};

struct FenvBundleManifest {
    std::string name;
    std::vector<FenvBundleEntry> environments;
};

// progress(bytesDone, bytesTotal), called as the environments' bytes are copied.
using FenvBundleProgress = std::function<void(uint64_t, uint64_t)>;

inline bool isFenvBundlePath(const std::string& p) {
    return p.size() > 8 && (p.compare(p.size() - 8, 8, ".fbundle") == 0 || p.compare(p.size() - 8, 8, ".FBUNDLE") == 0);
}

namespace fenv_bundle_detail {

constexpr const char* kManifestName = "bundle.json";

inline std::string manifestJson(const FenvBundleManifest& m) {
    std::string j = "{\n  \"format\": \"fbundle\",\n  \"version\": 1,\n  \"name\": " + fenv_detail::quote(m.name) + ",\n  \"environments\": [";
    for (size_t i = 0; i < m.environments.size(); ++i) {
        const FenvBundleEntry& e = m.environments[i];
        j += std::string(i ? "," : "") + "\n    { \"file\": " + fenv_detail::quote(e.file) + ", \"name\": " + fenv_detail::quote(e.name) +
             ", \"size\": " + std::to_string(e.size) + " }";
    }
    return j + "\n  ]\n}\n";
}

inline bool parseManifest(const std::string& json, FenvBundleManifest& m) {
    try {
        const tinyjson::Value doc = tinyjson::parse(json);
        if (!doc.isObject() || doc["format"].asString() != "fbundle") return false;
        m = FenvBundleManifest{};
        m.name = doc["name"].asString();
        for (size_t i = 0; i < doc["environments"].size(); ++i) {
            const auto& v = doc["environments"][i];
            FenvBundleEntry e;
            e.file = v["file"].asString();
            e.name = v["name"].asString();
            e.size = static_cast<uint64_t>(std::max<int64_t>(0, v["size"].asInt()));
            if (fenv_detail::safeMember(e.file) && isFenvPath(e.file)) m.environments.push_back(e);
        }
        return !m.environments.empty();
    } catch (...) {
        return false;
    }
}

// An opened bundle: the ZIP and its manifest; closes the ZIP when it goes.
struct OpenBundle {
    mz_zip_archive zip = {};
    bool open = false;
    FenvBundleManifest manifest;

    bool load(const std::string& path, std::string& error) {
        if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) { error = "can't open " + path; return false; }
        open = true;
        const std::vector<uint8_t> j = fenv_detail::readMember(zip, kManifestName, uint64_t(1) << 20);
        if (j.empty() || !parseManifest(std::string(j.begin(), j.end()), manifest)) { error = "not a Frame environment bundle (.fbundle): " + path; return false; }
        return true;
    }
    // The member's index and stat, refusing anything that isn't stored whole (a damaged or foreign bundle).
    bool member(const FenvBundleEntry& e, mz_uint& index, mz_zip_archive_file_stat& st, std::string& error) {
        const int i = mz_zip_reader_locate_file(&zip, e.file.c_str(), nullptr, 0);
        if (i < 0 || !mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(i), &st)) { error = "the bundle has no " + e.file; return false; }
        if (st.m_method != 0 || st.m_comp_size != st.m_uncomp_size || st.m_is_encrypted) { error = e.file + " isn't stored as-is in the bundle"; return false; }
        index = static_cast<mz_uint>(i);
        return true;
    }
    ~OpenBundle() { if (open) mz_zip_reader_end(&zip); }
};

// A file read at a fixed offset: one stored member of the bundle, read as a ZIP of its own.
struct FileSlice {
    std::ifstream file;
    uint64_t start = 0;
    static size_t read(void* opaque, mz_uint64 offset, void* buffer, size_t n) {
        FileSlice& s = *static_cast<FileSlice*>(opaque);
        s.file.clear();
        s.file.seekg(static_cast<std::streamoff>(s.start + offset));
        s.file.read(static_cast<char*>(buffer), static_cast<std::streamsize>(n));
        return static_cast<size_t>(s.file.gcount());
    }
};

// Where a stored member's bytes begin: after its local header (30 bytes, then its name and extra field, whose
// lengths the local header holds; they may differ from the central directory's).
inline bool memberDataOffset(const std::string& bundlePath, const mz_zip_archive_file_stat& st, uint64_t& offset) {
    std::ifstream f(std::filesystem::u8path(bundlePath), std::ios::binary);
    unsigned char header[30];
    f.seekg(static_cast<std::streamoff>(st.m_local_header_ofs));
    if (!f.read(reinterpret_cast<char*>(header), sizeof header)) return false;
    if (header[0] != 'P' || header[1] != 'K' || header[2] != 3 || header[3] != 4) return false;
    const unsigned nameLength = header[26] | (header[27] << 8), extraLength = header[28] | (header[29] << 8);
    offset = st.m_local_header_ofs + 30 + nameLength + extraLength;
    return true;
}

// Reads a .fenv's own file from disk, piece by piece, as the ZIP writer asks for it; counts the bytes for progress.
struct SourceReader {
    std::ifstream file;
    uint64_t* done = nullptr;
    uint64_t total = 0;
    const FenvBundleProgress* progress = nullptr;
    static size_t read(void* opaque, mz_uint64 offset, void* buffer, size_t n) {
        SourceReader& r = *static_cast<SourceReader*>(opaque);
        r.file.clear();                         // (after the last piece: reads 0, the writer's sign that it's all there)
        r.file.seekg(static_cast<std::streamoff>(offset));
        r.file.read(static_cast<char*>(buffer), static_cast<std::streamsize>(n));
        const size_t got = static_cast<size_t>(r.file.gcount());
        *r.done += got;
        if (*r.progress) (*r.progress)(*r.done, r.total);
        return got;
    }
};

// Where a member's bytes go as they come out of the bundle: into `out` (when open), or compared against a file already
// there (`compare` open: equal means that environment is installed already); checksummed either way but the last.
struct MemberSink {
    std::ofstream out;
    std::ifstream compare;
    bool same = true;
    mz_uint32 crc = MZ_CRC32_INIT;
    uint64_t* done = nullptr;           // with progress: bytes so far, of `total`
    uint64_t total = 0;
    const FenvBundleProgress* progress = nullptr;
    static size_t write(void* opaque, mz_uint64, const void* buffer, size_t n) {
        MemberSink& s = *static_cast<MemberSink*>(opaque);
        if (s.compare.is_open()) {
            char theirs[65536];
            for (size_t at = 0; at < n && s.same;) {
                const size_t piece = std::min(n - at, sizeof theirs);
                s.same = static_cast<bool>(s.compare.read(theirs, static_cast<std::streamsize>(piece))) &&
                         std::memcmp(theirs, static_cast<const char*>(buffer) + at, piece) == 0;
                at += piece;
            }
            return s.same ? n : 0;              // (a difference stops the extraction: the answer is known)
        }
        if (s.out.is_open() && !s.out.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(n))) return 0;
        s.crc = static_cast<mz_uint32>(mz_crc32(s.crc, static_cast<const unsigned char*>(buffer), n));
        if (s.done) {
            *s.done += n;
            if (s.progress && *s.progress) (*s.progress)(*s.done, s.total);
        }
        return n;
    }
};

// "haven.fenv", "haven 2.fenv", "haven 3.fenv"...: the first name `taken` doesn't refuse.
inline std::string freeName(const std::string& stem, const std::function<bool(const std::string&)>& taken) {
    std::string name = stem + ".fenv";
    for (int n = 2; taken(name); ++n) name = stem + " " + std::to_string(n) + ".fenv";
    return name;
}

inline std::string lowercase(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

}  // namespace fenv_bundle_detail

// Packs the .fenv files into one bundle at outPath (written as outPath + ".part", renamed when whole). Every file
// must be a readable Frame environment; two of the same file name become "name" and "name 2". `name` is the bundle's
// own name (empty: outPath's file name).
inline bool fenvBundleWrite(const std::string& outPath, const std::vector<std::string>& fenvPaths, const std::string& name, std::string& error,
                            const FenvBundleProgress& progress = {}) {
    namespace fs = std::filesystem;
    using namespace fenv_bundle_detail;
    if (fenvPaths.empty()) { error = "no environments to bundle"; return false; }
    FenvBundleManifest manifest;
    manifest.name = name.empty() ? fs::u8path(outPath).stem().u8string() : name;
    std::vector<std::string> takenLowercase;   // member names compared without case: they must unpack apart on Windows too
    uint64_t total = 0;
    for (const std::string& path : fenvPaths) {
        std::error_code ec;
        FenvManifest m;
        if (!isFenvPath(path)) { error = "not a Frame environment (.fenv): " + path; return false; }
        const uint64_t size = fs::file_size(fs::u8path(path), ec);
        if (ec || !fenvReadManifest(path, m)) { error = "can't read the environment " + path; return false; }
        FenvBundleEntry e;
        const std::string member = freeName(fs::u8path(path).stem().u8string(), [&](const std::string& n) {
            return std::find(takenLowercase.begin(), takenLowercase.end(), lowercase(n)) != takenLowercase.end();
        });
        takenLowercase.push_back(lowercase(member));
        e.file = "environments/" + member;
        e.name = m.name.empty() ? fs::u8path(path).stem().u8string() : m.name;
        e.size = size;
        total += size;
        manifest.environments.push_back(e);
    }
    const std::string tmp = outPath + ".part", json = manifestJson(manifest);
    const uint64_t headroom = json.size() + (uint64_t(1) << 20);           // names, headers, the central directory
    const mz_uint zip64 = total + headroom >= 0xFFFFFFFFull ? MZ_ZIP_FLAG_WRITE_ZIP64 : 0;
    mz_zip_archive zip = {};
    if (!mz_zip_writer_init_file_v2(&zip, tmp.c_str(), 0, zip64)) { error = "can't write " + outPath; return false; }
    bool ok = mz_zip_writer_add_mem(&zip, kManifestName, json.data(), json.size(), MZ_DEFAULT_COMPRESSION);
    if (!ok) error = "couldn't write " + outPath;
    uint64_t done = 0;
    const MZ_TIME_T now = std::time(nullptr);
    for (size_t i = 0; ok && i < fenvPaths.size(); ++i) {
        const FenvBundleEntry& e = manifest.environments[i];
        SourceReader reader;
        reader.file.open(fs::u8path(fenvPaths[i]), std::ios::binary);
        reader.done = &done;
        reader.total = total;
        reader.progress = &progress;
        const uint64_t before = done;
        ok = reader.file.is_open() &&
             mz_zip_writer_add_read_buf_callback(&zip, e.file.c_str(), &SourceReader::read, &reader, e.size, &now, nullptr, 0, MZ_NO_COMPRESSION,
                                                 nullptr, 0, nullptr, 0) &&
             done - before == e.size;          // (a file that changed size while it was copied)
        if (!ok) error = "couldn't add " + fenvPaths[i] + " to the bundle";
    }
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    if (!ok && error.empty()) error = "couldn't finish " + outPath;
    if (!ok) error += std::string(" (") + mz_zip_get_error_string(mz_zip_get_last_error(&zip)) + ")";
    mz_zip_writer_end(&zip);
    std::error_code ec;
    if (!ok) { fs::remove(fs::u8path(tmp), ec); return false; }
    fs::rename(fs::u8path(tmp), fs::u8path(outPath), ec);
    if (ec) { fs::remove(fs::u8path(outPath), ec); fs::rename(fs::u8path(tmp), fs::u8path(outPath), ec); }
    if (ec) { fs::remove(fs::u8path(tmp), ec); error = "couldn't replace " + outPath + ": " + ec.message(); return false; }
    return true;
}

// Reads bundle.json alone.
inline bool fenvBundleReadManifest(const std::string& path, FenvBundleManifest& manifest, std::string* error = nullptr) {
    fenv_bundle_detail::OpenBundle bundle;
    std::string why;
    if (!bundle.load(path, why)) { if (error) *error = why; return false; }
    manifest = bundle.manifest;
    return true;
}

// One environment's own manifest (and, if asked, its thumbnail bytes) read in place inside the bundle. `memberFile`
// is its "file" from the bundle's manifest.
inline bool fenvBundleReadEnvironment(const std::string& bundlePath, const std::string& memberFile, FenvManifest& manifest,
                                      std::vector<uint8_t>* thumbnail = nullptr, std::string* error = nullptr) {
    using namespace fenv_bundle_detail;
    std::string why;
    const auto fail = [&](const std::string& w) { if (error) *error = w; return false; };
    OpenBundle bundle;
    if (!bundle.load(bundlePath, why)) return fail(why);
    const auto it = std::find_if(bundle.manifest.environments.begin(), bundle.manifest.environments.end(),
                                 [&](const FenvBundleEntry& e) { return e.file == memberFile; });
    if (it == bundle.manifest.environments.end()) return fail("the bundle lists no " + memberFile);
    mz_uint index = 0;
    mz_zip_archive_file_stat st;
    if (!bundle.member(*it, index, st, why)) return fail(why);
    FileSlice slice;
    if (!memberDataOffset(bundlePath, st, slice.start)) return fail("damaged bundle: " + bundlePath);
    slice.file.open(std::filesystem::u8path(bundlePath), std::ios::binary);
    mz_zip_archive inner = {};
    inner.m_pRead = &FileSlice::read;
    inner.m_pIO_opaque = &slice;
    if (!slice.file.is_open() || !mz_zip_reader_init(&inner, st.m_uncomp_size, 0)) return fail(memberFile + " in the bundle isn't a Frame environment");
    const std::vector<uint8_t> json = fenv_detail::readMember(inner, "fenv.json");
    bool ok = !json.empty() && fenvParseManifest(std::string(json.begin(), json.end()), manifest);
    if (ok && thumbnail) *thumbnail = manifest.thumbnail.empty() ? std::vector<uint8_t>() : fenv_detail::readMember(inner, manifest.thumbnail.c_str());
    mz_zip_reader_end(&inner);
    return ok || fail(memberFile + " in the bundle isn't a Frame environment");
}

// Unpacks the bundle's environments into outDir as separate .fenv files (`onlyMember`: just that one, by its "file").
// Each is written as "<name>.part" and renamed when whole and checked against the bundle's checksum. A file already
// there of the same name is never replaced: if it is the same environment byte for byte it counts as unpacked,
// otherwise this one becomes "<name> 2.fenv". `unpacked` gets the environments' paths, in the bundle's order.
inline bool fenvBundleExtract(const std::string& bundlePath, const std::string& outDir, std::vector<std::string>& unpacked, std::string& error,
                              const std::string& onlyMember = {}, const FenvBundleProgress& progress = {}) {
    namespace fs = std::filesystem;
    using namespace fenv_bundle_detail;
    unpacked.clear();
    OpenBundle bundle;
    if (!bundle.load(bundlePath, error)) return false;
    std::vector<FenvBundleEntry> wanted;
    for (const FenvBundleEntry& e : bundle.manifest.environments)
        if (onlyMember.empty() || e.file == onlyMember) wanted.push_back(e);
    if (wanted.empty()) { error = "the bundle lists no " + onlyMember; return false; }
    uint64_t total = 0, done = 0;
    for (const FenvBundleEntry& e : wanted) total += e.size;
    std::error_code ec;
    const fs::path dir = fs::u8path(outDir);
    fs::create_directories(dir, ec);
    for (const FenvBundleEntry& e : wanted) {
        mz_uint index = 0;
        mz_zip_archive_file_stat st;
        if (!bundle.member(e, index, st, error)) return false;
        const std::string stem = fs::u8path(e.file).stem().u8string();
        if (stem.empty() || stem[0] == '.') { error = "the bundle names an environment \"" + e.file + "\""; return false; }
        // The first free name, or the file that already is this environment.
        std::string existing;
        const std::string name = freeName(stem, [&](const std::string& candidate) {
            const fs::path p = dir / fs::u8path(candidate);
            if (!fs::exists(p, ec)) return false;
            if (existing.empty() && fs::is_regular_file(p, ec) && fs::file_size(p, ec) == st.m_uncomp_size) {
                MemberSink check;
                check.compare.open(p, std::ios::binary);
                if (check.compare.is_open() && mz_zip_reader_extract_to_callback(&bundle.zip, index, &MemberSink::write, &check, 0) && check.same) {
                    existing = candidate;
                    return false;             // stop here: this one is it
                }
            }
            return true;
        });
        const fs::path target = dir / fs::u8path(name);
        if (!existing.empty()) {
            done += e.size;
            if (progress) progress(done, total);
            unpacked.push_back(target.u8string());
            continue;
        }
        const fs::path part = dir / fs::u8path(name + ".part");
        MemberSink sink;
        sink.out.open(part, std::ios::binary | std::ios::trunc);
        sink.done = &done;
        sink.total = total;
        sink.progress = &progress;
        bool ok = sink.out.is_open() && mz_zip_reader_extract_to_callback(&bundle.zip, index, &MemberSink::write, &sink, 0);
        sink.out.close();
        ok = ok && static_cast<bool>(sink.out) && sink.crc == st.m_crc32;
        if (!ok) { fs::remove(part, ec); error = "couldn't unpack " + e.file + " from " + bundlePath + " (damaged, or the disk is full)"; return false; }
        fs::rename(part, target, ec);
        if (ec) { fs::remove(part, ec); error = "couldn't write " + target.u8string() + ": " + ec.message(); return false; }
        unpacked.push_back(target.u8string());
    }
    return true;
}

// Checks a bundle without unpacking it: its manifest, each environment present, stored whole and of the size listed,
// with a readable fenv.json of its own; with `checkData`, each one's bytes against its checksum too (reads them all).
inline bool fenvBundleValidate(const std::string& path, std::string& error, bool checkData = true) {
    using namespace fenv_bundle_detail;
    OpenBundle bundle;
    if (!bundle.load(path, error)) return false;
    for (const FenvBundleEntry& e : bundle.manifest.environments) {
        mz_uint index = 0;
        mz_zip_archive_file_stat st;
        if (!bundle.member(e, index, st, error)) return false;
        if (st.m_uncomp_size != e.size) { error = e.file + " is " + std::to_string(st.m_uncomp_size) + " bytes, the bundle says " + std::to_string(e.size); return false; }
        FenvManifest m;
        if (!fenvBundleReadEnvironment(path, e.file, m, nullptr, &error)) return false;
        if (!checkData) continue;
        MemberSink sum;                        // (nothing open: checksums only)
        if (!mz_zip_reader_extract_to_callback(&bundle.zip, index, &MemberSink::write, &sum, 0) || sum.crc != st.m_crc32) {
            error = e.file + " in the bundle is damaged (checksum)";
            return false;
        }
    }
    return true;
}
