#include "workfolder.h"

#include <cpr/cpr.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>

#include "util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lectern::workfolder {

namespace {

uint64_t fModifiedMs(const fs::path &path)
{
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec)
    {
        return 0;
    }
    const auto system_time =
        std::chrono::clock_cast<std::chrono::system_clock>(time);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            system_time.time_since_epoch())
                            .count();
    return millis < 0 ? 0 : static_cast<uint64_t>(millis);
}

}  // namespace

bool fIsWorkFile(std::string_view name)
{
    const std::string ext = util::fExtensionOf(name);
    return std::find(cWorkFileExtensions.begin(),
                     cWorkFileExtensions.end(),
                     ext) != cWorkFileExtensions.end();
}

json fWalkWorkDir(const fs::path &dir, const std::string &rel_prefix)
{
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
    {
        throw std::runtime_error("cannot read " + dir.string());
    }

    struct Entry
    {
        std::string name;
        bool is_dir = false;
        json value;
    };
    std::vector<Entry> entries;

    for (const auto &entry : it)
    {
        const std::string name = entry.path().filename().string();

        // Skip dot-prefixed entries (e.g. `.officesuite-sync/`, the cloud
        // sync engine's metadata folder) so internal bookkeeping never shows
        // up in the file tree.
        if (!name.empty() && name.front() == '.')
        {
            continue;
        }

        std::error_code kind_ec;
        const bool is_dir = entry.is_directory(kind_ec);
        if (kind_ec)
        {
            continue;
        }

        const std::string rel_path =
            rel_prefix.empty() ? name : rel_prefix + "/" + name;

        if (is_dir)
        {
            entries.push_back(
                {name,
                 true,
                 json{{"name", name},
                      {"relPath", rel_path},
                      {"isDir", true},
                      {"modified", 0},
                      {"children", fWalkWorkDir(entry.path(), rel_path)}}});
            continue;
        }

        // Listed whatever its extension, to match what sync now carries and
        // what the server lists. fIsWorkFile still exists and still gates
        // sharing, which stays document-only.

        entries.push_back({name,
                           false,
                           json{{"name", name},
                                {"relPath", rel_path},
                                {"isDir", false},
                                {"modified", fModifiedMs(entry.path())},
                                {"children", json::array()}}});
    }

    std::sort(entries.begin(),
              entries.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.is_dir != b.is_dir)
                  {
                      return a.is_dir;
                  }
                  return util::fToLower(a.name) < util::fToLower(b.name);
              });

    json out = json::array();
    for (auto &entry : entries)
    {
        out.push_back(std::move(entry.value));
    }
    return out;
}

json fDefaultFile(const std::string &name)
{
    static constexpr const char *cContent =
        "# Welcome to Lectern\n\n"
        "Lectern is a lightweight office suite that stores everything in "
        "**Markdown**.\n\n"
        "## Features\n\n"
        "**Documents** rich markdown editing with live preview\n"
        "**Spreadsheets** formula-capable grid stored as cell=value pairs\n"
        "## Markdown Quick Reference\n"
        "| Element | Syntax |\n"
        "|---------|--------|\n"
        "| Bold | **text** |\n"
        "| Italic | *text* |\n"
        "| Heading | # H1 ## H2 |\n"
        "| List | - item |\n"
        "| Blockquote | > text |\n"
        "| Code | ``` code ``` |\n"
        "| Link | [text](url) |\n\n"
        "## Getting Started\n\n"
        "1. Click **New** in the sidebar to create a file\n"
        "2. Write in Markdown, use the toolbar for formatting\n"
        "3. Toggle the **split** view button to preview alongside your "
        "writing\n"
        "4. Click the **download** button to export your file\n\n"
        "> All files are saved automatically to your browser's local "
        "storage.\n\n"
        "Happy writing!";

    return json{{"name", "Welcome to Lectern " + name},
                {"docType", "doc"},
                {"content", cContent}};
}

bool fIsFetchableUrl(std::string_view url)
{
    const std::string lower = util::fToLower(std::string(url));
    const bool http = lower.rfind("http://", 0) == 0;
    const bool https = lower.rfind("https://", 0) == 0;
    if (!http && !https)
    {
        return false;
    }
    // Something has to follow the scheme, and no whitespace or control
    // characters anywhere: those never belong in a URL a user pasted.
    const size_t host = http ? 7 : 8;
    if (url.size() <= host)
    {
        return false;
    }
    return std::none_of(url.begin(), url.end(), [](char c) {
        return static_cast<unsigned char>(c) <= 0x20 || c == 0x7f;
    });
}

json fFetchRemote(const std::string &url)
{
    if (!fIsFetchableUrl(url))
    {
        throw std::runtime_error("only http and https addresses can be fetched");
    }

    // Abort as soon as the body outgrows the limit instead of buffering
    // whatever the server chooses to send.
    std::string body;
    bool too_large = false;
    const auto response = cpr::Get(
        cpr::Url{url},
        cpr::Timeout{30000},
        cpr::Redirect{10L},
        cpr::WriteCallback{[&](std::string_view chunk, intptr_t) {
            if (body.size() + chunk.size() > cMaxRemoteFetchBytes)
            {
                too_large = true;
                return false;
            }
            body.append(chunk);
            return true;
        }});

    if (too_large)
    {
        throw std::runtime_error(
            "the file is too large (" +
            std::to_string(cMaxRemoteFetchBytes / (1024 * 1024)) + " MB max)");
    }
    if (response.error)
    {
        throw std::runtime_error("could not download: " +
                                 response.error.message);
    }
    if (response.status_code < 200 || response.status_code >= 300)
    {
        throw std::runtime_error("the server answered " +
                                 std::to_string(response.status_code));
    }

    std::string content_type;
    const auto it = response.header.find("content-type");
    if (it != response.header.end())
    {
        content_type = it->second;
    }

    return json{{"data", util::fBase64Encode(std::string_view(body))},
                {"contentType", content_type}};
}

}  // namespace lectern::workfolder
