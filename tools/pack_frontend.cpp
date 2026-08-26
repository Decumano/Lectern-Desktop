// Packs the frontend directory into one blob the binary can embed.
//
// A portable single-file build has nowhere to put a `frontend/` directory, so
// the assets go inside the executable. This produces the container: an index
// of relative paths followed by their bytes, uncompressed.
//
// Uncompressed on purpose. The frontend is ~10 MB and the alternative is
// pulling in a compression library and paying to inflate on every asset
// request; an executable a few megabytes larger is the cheaper trade, and it
// keeps the reader (src/embedded.cpp) small enough to audit at a glance.
//
// Format, all integers little-endian:
//
//   "LKPK"          4 bytes  magic
//   version         u32      currently 1
//   count           u32      number of entries
//   count entries:
//     name_len      u32      bytes of `name`
//     name          bytes    relative path, forward slashes, no leading slash
//     offset        u64      from the start of the file
//     size          u64
//   ... payload bytes, in index order ...
//
// Usage: lectern-pack-frontend <input-dir> <output.pack>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Entry
{
    std::string name;
    fs::path path;
    uint64_t size = 0;
    uint64_t offset = 0;
};

void write_u32(std::ostream &out, uint32_t value)
{
    unsigned char bytes[4] = {
        static_cast<unsigned char>(value & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff),
        static_cast<unsigned char>((value >> 16) & 0xff),
        static_cast<unsigned char>((value >> 24) & 0xff),
    };
    out.write(reinterpret_cast<const char *>(bytes), 4);
}

void write_u64(std::ostream &out, uint64_t value)
{
    unsigned char bytes[8];
    for (int i = 0; i < 8; ++i)
    {
        bytes[i] = static_cast<unsigned char>((value >> (i * 8)) & 0xff);
    }
    out.write(reinterpret_cast<const char *>(bytes), 8);
}

}  // namespace

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        std::cerr << "usage: lectern-pack-frontend <input-dir> <output.pack>\n";
        return 2;
    }

    const fs::path input = argv[1];
    const fs::path output = argv[2];

    std::error_code ec;
    if (!fs::is_directory(input, ec) || ec)
    {
        std::cerr << "not a directory: " << input.string() << "\n";
        return 1;
    }

    std::vector<Entry> entries;
    for (fs::recursive_directory_iterator it(input, ec), end; it != end;
         it.increment(ec))
    {
        if (ec)
        {
            std::cerr << "walk failed: " << ec.message() << "\n";
            return 1;
        }

        // Skip the submodule's own git metadata; it is not part of the app.
        const std::string filename = it->path().filename().string();
        if (!filename.empty() && filename.front() == '.')
        {
            if (it->is_directory())
            {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!it->is_regular_file())
        {
            continue;
        }

        Entry entry;
        entry.path = it->path();
        entry.name = fs::relative(it->path(), input, ec).generic_string();
        if (ec || entry.name.empty())
        {
            std::cerr << "cannot relativise " << it->path().string() << "\n";
            return 1;
        }
        entry.size = fs::file_size(it->path(), ec);
        if (ec)
        {
            std::cerr << "cannot size " << it->path().string() << "\n";
            return 1;
        }
        entries.push_back(std::move(entry));
    }

    if (entries.empty())
    {
        std::cerr << "no files found under " << input.string() << "\n";
        return 1;
    }

    // Sorted so the reader can binary-search, and so the same input always
    // produces a byte-identical pack.
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.name < b.name;
    });

    // The index is fixed-size once the names are known, so offsets can be
    // computed before anything is written.
    uint64_t offset = 4 + 4 + 4;  // magic + version + count
    for (const auto &entry : entries)
    {
        offset += 4 + entry.name.size() + 8 + 8;
    }
    for (auto &entry : entries)
    {
        entry.offset = offset;
        offset += entry.size;
    }

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        std::cerr << "cannot write " << output.string() << "\n";
        return 1;
    }

    out.write("LKPK", 4);
    write_u32(out, 1);
    write_u32(out, static_cast<uint32_t>(entries.size()));
    for (const auto &entry : entries)
    {
        write_u32(out, static_cast<uint32_t>(entry.name.size()));
        out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        write_u64(out, entry.offset);
        write_u64(out, entry.size);
    }

    for (const auto &entry : entries)
    {
        std::ifstream in(entry.path, std::ios::binary);
        if (!in)
        {
            std::cerr << "cannot read " << entry.path.string() << "\n";
            return 1;
        }
        out << in.rdbuf();
        if (!out)
        {
            std::cerr << "write failed\n";
            return 1;
        }
    }

    out.close();
    std::cout << "packed " << entries.size() << " files ("
              << (offset / 1024) << " KB) into " << output.string() << "\n";
    return 0;
}
