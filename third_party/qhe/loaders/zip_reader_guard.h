#pragma once

#include "cook/zip_safety.h"
#include "miniz.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zipread {

inline const char* statusText(hslcook::zipsafety::ArchiveReadStatus status) {
    using Status = hslcook::zipsafety::ArchiveReadStatus;
    switch (status) {
        case Status::Ok: return "ok";
        case Status::TooManyEntries: return "too many entries";
        case Status::EntryTooLarge: return "entry is too large";
        case Status::TotalTooLarge: return "declared uncompressed total is too large";
        case Status::UnsafeName: return "unsafe entry name";
        case Status::DuplicateName: return "duplicate normalized entry name";
    }
    return "unknown archive error";
}

// Validate the entire central directory before any entry is extracted.  This
// adapter intentionally stays tiny; the policy and arithmetic live in the pure
// zip_safety helper so they can be unit-tested without miniz.
inline bool validateArchive(mz_zip_archive& zip,
                            const hslcook::zipsafety::ArchiveReadLimits& limits,
                            std::string* error = nullptr) {
    using namespace hslcook::zipsafety;
    ArchiveReadBudget budget(limits);
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (static_cast<uint64_t>(count) > limits.maxEntries) {
        if (error) *error = "too many entries";
        return false;
    }

    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            if (error) *error = "invalid central-directory entry " + std::to_string(i);
            return false;
        }

        const mz_uint nameBytesWithNull = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
        if (nameBytesWithNull == 0 || nameBytesWithNull > kZip16Max + 1) {
            if (error) *error = "invalid or overlong entry name at index " + std::to_string(i);
            return false;
        }
        std::vector<char> name(nameBytesWithNull);
        if (mz_zip_reader_get_filename(&zip, i, name.data(), nameBytesWithNull) == 0) {
            if (error) *error = "failed to read entry name at index " + std::to_string(i);
            return false;
        }

        const bool directory = mz_zip_reader_is_file_a_directory(&zip, i) != 0;
        const ArchiveReadResult result = budget.add(
            std::string_view(name.data(), nameBytesWithNull - 1),
            static_cast<uint64_t>(stat.m_uncomp_size), directory);
        if (result.status != ArchiveReadStatus::Ok) {
            if (error) {
                *error = std::string(statusText(result.status)) + " at entry " +
                         std::to_string(i) + " ('" + name.data() + "')";
            }
            return false;
        }
    }
    return true;
}

} // namespace zipread
