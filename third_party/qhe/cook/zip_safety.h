#pragma once

#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hslcook::zipsafety {

// Quest Home archives use the classic (non-ZIP64) ZIP layout. Keep the limits
// explicit so a failed narrowing conversion can never produce a corrupt APK.
constexpr uint64_t kZip32Max = std::numeric_limits<uint32_t>::max();
constexpr uint64_t kZip16Max = std::numeric_limits<uint16_t>::max();

enum class NameStatus {
    Ok,
    Empty,
    Absolute,
    DriveQualified,
    ParentTraversal,
    InvalidCharacter,
    TooLong,
    Directory,
    Duplicate,
    TooManyEntries,
};

// Read-side limits for APKs and their nested scene archives.  The editor loads
// archives supplied by users, so trusting central-directory sizes directly
// would allow a tiny ZIP bomb to request an effectively unbounded allocation.
// These defaults deliberately leave ample room for the largest known official
// and community Homes while keeping malformed/hostile inputs finite.  Callers
// may pass a different limits object for a known, trusted workflow.
struct ArchiveReadLimits {
    uint64_t maxEntries = 32768;
    uint64_t maxEntryUncompressedBytes = 1024ull * 1024ull * 1024ull; // 1 GiB
    uint64_t maxTotalUncompressedBytes = kZip32Max;                    // ~4 GiB
};

constexpr ArchiveReadLimits kApkReadLimits{
    kZip16Max, 2ull * 1024ull * 1024ull * 1024ull, kZip32Max
};
constexpr ArchiveReadLimits kSceneReadLimits{
    32768, 1024ull * 1024ull * 1024ull, kZip32Max
};

enum class ArchiveReadStatus {
    Ok,
    TooManyEntries,
    EntryTooLarge,
    TotalTooLarge,
    UnsafeName,
    DuplicateName,
};

struct ArchiveReadResult {
    ArchiveReadStatus status = ArchiveReadStatus::Ok;
    NameStatus nameStatus = NameStatus::Ok;
    uint64_t entryCount = 0;
    uint64_t totalUncompressedBytes = 0;
};

// Canonicalize archive separators without making an unsafe path safe by
// accident. Empty and "." segments are removed, but "..", absolute/UNC paths,
// drive-qualified paths, control characters, and NTFS alternate-stream names
// are rejected. ZIP entries emitted by this project are files, not directory
// placeholders, unless allowDirectory is explicitly requested by a reader.
inline NameStatus normalizeEntryName(std::string_view raw, std::string& normalized,
                                     bool allowDirectory = false) {
    normalized.clear();
    if (raw.empty()) return NameStatus::Empty;

    const auto isSep = [](char c) { return c == '/' || c == '\\'; };
    if (isSep(raw.front())) return NameStatus::Absolute;
    if (raw.size() >= 2 && std::isalpha(static_cast<unsigned char>(raw[0])) && raw[1] == ':')
        return NameStatus::DriveQualified;
    if (isSep(raw.back()) && !allowDirectory) return NameStatus::Directory;

    size_t begin = 0;
    while (begin <= raw.size()) {
        size_t end = begin;
        while (end < raw.size() && !isSep(raw[end])) ++end;
        std::string_view part = raw.substr(begin, end - begin);

        if (part == "..") return NameStatus::ParentTraversal;
        if (!part.empty() && part != ".") {
            for (char c : part) {
                const unsigned char uc = static_cast<unsigned char>(c);
                if (uc < 0x20 || c == ':') return NameStatus::InvalidCharacter;
            }
            if (!normalized.empty()) normalized.push_back('/');
            normalized.append(part.data(), part.size());
            if (normalized.size() > kZip16Max) return NameStatus::TooLong;
        }

        if (end == raw.size()) break;
        begin = end + 1;
    }

    if (normalized.empty()) return NameStatus::Empty;
    return NameStatus::Ok;
}

class EntryNameRegistry {
public:
    NameStatus add(std::string_view raw, std::string* normalizedOut = nullptr,
                   bool allowDirectory = false) {
        std::string normalized;
        NameStatus status = normalizeEntryName(raw, normalized, allowDirectory);
        if (status != NameStatus::Ok) return status;
        if (names_.size() >= kZip16Max) return NameStatus::TooManyEntries;
        if (!names_.insert(normalized).second) return NameStatus::Duplicate;
        if (normalizedOut) *normalizedOut = std::move(normalized);
        return NameStatus::Ok;
    }

private:
    std::unordered_set<std::string> names_;
};

// Pure, streaming archive preflight. Feed one central-directory entry at a
// time, before extracting any payload.  The running total uses the declared
// uncompressed sizes (not compressed bytes), and equivalent normalized names
// are rejected so lookup cannot be redirected by entries such as a//b and
// a/./b. Directory placeholders participate in duplicate detection too.
class ArchiveReadBudget {
public:
    explicit ArchiveReadBudget(ArchiveReadLimits limits = {}) : limits_(limits) {}

    ArchiveReadResult add(std::string_view rawName, uint64_t uncompressedBytes,
                          bool isDirectory = false, std::string* normalizedOut = nullptr) {
        if (result_.status != ArchiveReadStatus::Ok) return result_;
        if (result_.entryCount >= limits_.maxEntries) {
            result_.status = ArchiveReadStatus::TooManyEntries;
            return result_;
        }
        if (uncompressedBytes > limits_.maxEntryUncompressedBytes) {
            result_.status = ArchiveReadStatus::EntryTooLarge;
            return result_;
        }
        if (result_.totalUncompressedBytes > limits_.maxTotalUncompressedBytes ||
            uncompressedBytes > limits_.maxTotalUncompressedBytes - result_.totalUncompressedBytes) {
            result_.status = ArchiveReadStatus::TotalTooLarge;
            return result_;
        }

        const NameStatus name = names_.add(rawName, normalizedOut, isDirectory);
        if (name != NameStatus::Ok) {
            result_.nameStatus = name;
            result_.status = (name == NameStatus::Duplicate)
                ? ArchiveReadStatus::DuplicateName
                : ArchiveReadStatus::UnsafeName;
            return result_;
        }

        ++result_.entryCount;
        result_.totalUncompressedBytes += uncompressedBytes;
        return result_;
    }

    const ArchiveReadResult& result() const { return result_; }

private:
    ArchiveReadLimits limits_;
    EntryNameRegistry names_;
    ArchiveReadResult result_;
};

struct StoredEntrySize {
    uint64_t nameBytes = 0;
    uint64_t dataBytes = 0;
};

struct StoredZipLayout {
    uint64_t centralDirectoryOffset = 0;
    uint64_t centralDirectoryBytes = 0;
    uint64_t totalBytes = 0;
};

inline bool checkedAdd(uint64_t lhs, uint64_t rhs, uint64_t limit, uint64_t& result) {
    if (lhs > limit || rhs > limit - lhs) return false;
    result = lhs + rhs;
    return true;
}

// Preflight the exact byte layout used by buildStoredZip. This is deliberately
// independent of CookFile so it stays tiny and can be regression-tested without
// pulling the full cooker and its third-party dependencies into the test binary.
inline bool computeStoredZipLayout(const std::vector<StoredEntrySize>& entries,
                                   StoredZipLayout& layout) {
    layout = {};
    if (entries.size() > kZip16Max) return false;

    uint64_t localBytes = 0;
    uint64_t centralBytes = 0;
    for (const StoredEntrySize& entry : entries) {
        if (entry.nameBytes == 0 || entry.nameBytes > kZip16Max || entry.dataBytes > kZip32Max)
            return false;

        uint64_t localEntry = 0;
        if (!checkedAdd(30, entry.nameBytes, kZip32Max, localEntry) ||
            !checkedAdd(localEntry, entry.dataBytes, kZip32Max, localEntry) ||
            !checkedAdd(localBytes, localEntry, kZip32Max, localBytes))
            return false;

        uint64_t centralEntry = 0;
        if (!checkedAdd(46, entry.nameBytes, kZip32Max, centralEntry) ||
            !checkedAdd(centralBytes, centralEntry, kZip32Max, centralBytes))
            return false;
    }

    uint64_t total = 0;
    if (!checkedAdd(localBytes, centralBytes, kZip32Max, total) ||
        !checkedAdd(total, 22, kZip32Max, total))
        return false;

    layout.centralDirectoryOffset = localBytes;
    layout.centralDirectoryBytes = centralBytes;
    layout.totalBytes = total;
    return true;
}

} // namespace hslcook::zipsafety
