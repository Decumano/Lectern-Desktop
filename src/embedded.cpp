#include "embedded.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef LECTERN_EMBED_FRONTEND
#ifdef _WIN32
#include <windows.h>
#else
// Provided by the generated .S file (see cmake/EmbedFrontend.cmake).
extern "C" const unsigned char lectern_frontend_pack_start[];
extern "C" const unsigned char lectern_frontend_pack_end[];
#endif
#endif

namespace lectern::embedded {

namespace {

struct Index
{
    std::string_view blob;
    /// Sorted by name, matching the order the packer wrote.
    std::vector<std::pair<std::string_view, std::string_view>> entries;
    bool valid = false;
};

uint32_t fReadU32(const unsigned char *data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t fReadU64(const unsigned char *data)
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
    {
        value = (value << 8) | data[i];
    }
    return value;
}

/// Locates the linked blob, if this build has one.
std::string_view fLocateBlob()
{
#ifndef LECTERN_EMBED_FRONTEND
    return {};
#elif defined(_WIN32)
    // RT_RCDATA expands to MAKEINTRESOURCE, which is the ANSI form unless
    // UNICODE is defined; spell out the wide one to match FindResourceW.
    HRSRC found =
        FindResourceW(nullptr, L"LECTERN_FRONTEND", MAKEINTRESOURCEW(10));
    if (found == nullptr)
    {
        return {};
    }
    HGLOBAL loaded = LoadResource(nullptr, found);
    if (loaded == nullptr)
    {
        return {};
    }
    const void *data = LockResource(loaded);
    const DWORD size = SizeofResource(nullptr, found);
    if (data == nullptr || size == 0)
    {
        return {};
    }
    return {static_cast<const char *>(data), size};
#else
    const auto size = static_cast<size_t>(lectern_frontend_pack_end -
                                          lectern_frontend_pack_start);
    return {reinterpret_cast<const char *>(lectern_frontend_pack_start), size};
#endif
}

/// Parses the index once. Every bound is checked against the blob's real size:
/// the pack is part of the binary, but a truncated or corrupted one must fail
/// closed rather than read past the end.
Index fParse()
{
    Index index;
    index.blob = fLocateBlob();

    const auto *bytes =
        reinterpret_cast<const unsigned char *>(index.blob.data());
    const size_t size = index.blob.size();

    constexpr size_t cHeader = 4 + 4 + 4;
    if (bytes == nullptr || size < cHeader)
    {
        return index;
    }
    if (std::memcmp(bytes, "LKPK", 4) != 0)
    {
        return index;
    }
    if (fReadU32(bytes + 4) != 1)
    {
        return index;  // a version this build does not understand
    }

    const uint32_t count = fReadU32(bytes + 8);
    size_t cursor = cHeader;

    index.entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (cursor + 4 > size)
        {
            return {};
        }
        const uint32_t name_len = fReadU32(bytes + cursor);
        cursor += 4;

        if (name_len == 0 || cursor + name_len + 16 > size)
        {
            return {};
        }
        const std::string_view name(
            reinterpret_cast<const char *>(bytes + cursor), name_len);
        cursor += name_len;

        const uint64_t offset = fReadU64(bytes + cursor);
        cursor += 8;
        const uint64_t length = fReadU64(bytes + cursor);
        cursor += 8;

        // The payload must lie inside the blob, and offset + length must not
        // wrap around.
        if (offset > size || length > size - offset)
        {
            return {};
        }

        index.entries.emplace_back(
            name,
            std::string_view(reinterpret_cast<const char *>(bytes + offset),
                             static_cast<size_t>(length)));
    }

    // The packer sorts by name; rely on it, but do not trust it.
    if (!std::is_sorted(index.entries.begin(),
                        index.entries.end(),
                        [](const auto &a, const auto &b) {
                            return a.first < b.first;
                        }))
    {
        std::sort(index.entries.begin(),
                  index.entries.end(),
                  [](const auto &a, const auto &b) {
                      return a.first < b.first;
                  });
    }

    index.valid = true;
    return index;
}

const Index &fIndex()
{
    static const Index parsed = fParse();
    return parsed;
}

}  // namespace

bool fAvailable()
{
    return fIndex().valid && !fIndex().entries.empty();
}

size_t fCount()
{
    return fIndex().valid ? fIndex().entries.size() : 0;
}

std::optional<std::string_view> fRead(std::string_view name)
{
    const Index &parsed = fIndex();
    if (!parsed.valid)
    {
        return std::nullopt;
    }

    const auto it = std::lower_bound(
        parsed.entries.begin(),
        parsed.entries.end(),
        name,
        [](const auto &entry, std::string_view key) {
            return entry.first < key;
        });

    if (it == parsed.entries.end() || it->first != name)
    {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace lectern::embedded
