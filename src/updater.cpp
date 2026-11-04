#include "updater.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "dialogs.h"
#include "util.h"
#include "version.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lectern::updater {

namespace {

/// Which entry of the manifest's `platforms` map this build wants.
constexpr const char *cPlatformKey =
#if defined(_WIN32)
    "windows-x86_64";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    "darwin-aarch64";
#else
    "darwin-x86_64";
#endif
#else
    "linux-x86_64";
#endif

/// Flatpak's own store handles updates; checking again would offer the user an
/// installer they cannot run inside the sandbox.
bool fRunningInFlatpak()
{
    std::error_code ec;
    return fs::exists("/.flatpak-info", ec) && !ec;
}

/// The manifest is fetched alongside a detached signature at `<url>.sig`.
std::string fSignatureUrl(const std::string &manifest_url)
{
    return manifest_url + ".sig";
}

}  // namespace

int fCompareVersions(const std::string &left, const std::string &right)
{
    const auto fParts = [](const std::string &value) {
        // Stops at the first pre-release/build separator: 1.2.3-beta.1 is
        // compared as 1.2.3, which is enough for the "is there something
        // newer" question and avoids implementing all of semver.
        std::vector<long long> out;
        long long current = 0;
        bool has_digit = false;
        for (const char c : value)
        {
            if (std::isdigit(static_cast<unsigned char>(c)) != 0)
            {
                current = current * 10 + (c - '0');
                has_digit = true;
                continue;
            }
            if (c == '.')
            {
                out.push_back(has_digit ? current : 0);
                current = 0;
                has_digit = false;
                continue;
            }
            break;
        }
        out.push_back(has_digit ? current : 0);
        return out;
    };

    const auto a = fParts(left);
    const auto b = fParts(right);
    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i)
    {
        const long long lhs = i < a.size() ? a[i] : 0;
        const long long rhs = i < b.size() ? b[i] : 0;
        if (lhs != rhs)
        {
            return lhs < rhs ? -1 : 1;
        }
    }
    return 0;
}

bool fVerifySignature(const std::string &payload,
                      const std::string &signature_hex,
                      const std::string &public_key_hex)
{
    const auto signature = util::fFromHex(util::fTrim(signature_hex));
    const auto public_key = util::fFromHex(util::fTrim(public_key_hex));

    if (!signature || signature->size() != crypto_sign_BYTES)
    {
        return false;
    }
    if (!public_key || public_key->size() != crypto_sign_PUBLICKEYBYTES)
    {
        return false;
    }

    return crypto_sign_verify_detached(
               signature->data(),
               reinterpret_cast<const unsigned char *>(payload.data()),
               payload.size(),
               public_key->data()) == 0;
}

bool fEnabled()
{
    return std::string_view(cUpdatePublicKey).size() ==
               crypto_sign_PUBLICKEYBYTES * 2 &&
           !std::string_view(cUpdateManifestUrl).empty() &&
           !fRunningInFlatpak();
}

std::optional<Update> fCheck()
{
    if (!fEnabled())
    {
        return std::nullopt;
    }

    const std::string manifest_url = cUpdateManifestUrl;

    const auto manifest = cpr::Get(cpr::Url{manifest_url},
                                   cpr::Timeout{15000},
                                   cpr::Redirect{5L});
    if (manifest.status_code < 200 || manifest.status_code >= 300)
    {
        spdlog::debug("update check: manifest returned {}",
                      manifest.status_code);
        return std::nullopt;
    }

    const auto signature = cpr::Get(cpr::Url{fSignatureUrl(manifest_url)},
                                    cpr::Timeout{15000},
                                    cpr::Redirect{5L});
    if (signature.status_code < 200 || signature.status_code >= 300)
    {
        spdlog::debug("update check: signature returned {}",
                      signature.status_code);
        return std::nullopt;
    }

    // Verified before parsing: a manifest that fails here is never interpreted
    // at all, so a hostile one gets no chance to exercise the JSON reader.
    if (!fVerifySignature(manifest.text, signature.text, cUpdatePublicKey))
    {
        spdlog::warn("update check: manifest signature did not verify");
        return std::nullopt;
    }

    const auto parsed = json::parse(manifest.text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
    {
        return std::nullopt;
    }

    const std::string latest = parsed.value("version", "");
    if (latest.empty() || fCompareVersions(latest, cVersion) <= 0)
    {
        return std::nullopt;  // up to date
    }

    if (!parsed.contains("platforms") || !parsed["platforms"].is_object())
    {
        return std::nullopt;
    }
    const auto &platforms = parsed["platforms"];
    if (!platforms.contains(cPlatformKey))
    {
        return std::nullopt;  // no build published for this platform
    }
    const auto &entry = platforms[cPlatformKey];

    Update update;
    update.version = latest;
    update.notes = parsed.value("notes", "");
    update.url = entry.value("url", "");
    update.sha256 = util::fToLower(entry.value("sha256", ""));
    update.size = entry.value("size", uint64_t{0});

    // Only https, and only a digest we can actually check against.
    if (!util::fStartsWith(update.url, "https://") ||
        update.sha256.size() != 64)
    {
        spdlog::warn("update check: manifest entry is not usable");
        return std::nullopt;
    }
    return update;
}

std::optional<std::string> fDownload(const Update &update)
{
    std::error_code ec;
    const fs::path directory =
        fs::temp_directory_path(ec) / ("lectern-update-" + util::fRandomHex(8));
    fs::create_directories(directory, ec);
    if (ec)
    {
        return std::nullopt;
    }

    // The filename comes from the signed manifest's URL, but it still becomes
    // a path here, so keep it to a plain basename.
    std::string filename(util::fBasenameOf(update.url));
    const size_t query = filename.find('?');
    if (query != std::string::npos)
    {
        filename = filename.substr(0, query);
    }
    if (filename.empty() || filename.find("..") != std::string::npos ||
        filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos)
    {
        filename = "lectern-update";
    }

    const fs::path target = directory / filename;

    {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return std::nullopt;
        }
        const auto response = cpr::Download(out,
                                            cpr::Url{update.url},
                                            cpr::Timeout{0},
                                            cpr::Redirect{5L});
        if (response.status_code < 200 || response.status_code >= 300)
        {
            spdlog::warn("update download failed: {}", response.status_code);
            fs::remove_all(directory, ec);
            return std::nullopt;
        }
    }

    // The digest is the whole point of the signature: it binds the manifest to
    // these exact bytes, wherever they were served from.
    if (util::fSha256File(target) != update.sha256)
    {
        spdlog::warn("update download failed its checksum; discarding");
        fs::remove_all(directory, ec);
        return std::nullopt;
    }

    return target.string();
}

bool fInstall(const std::string &downloaded_path)
{
#if defined(_WIN32)
    // The NSIS installer takes over from here: it stops the running app,
    // replaces the files and restarts. Quitting immediately keeps this
    // process from holding its own binary open.
    return util::fSpawnDetached(fs::path(downloaded_path), {});
#elif defined(__APPLE__)
    // Opening the .dmg mounts it and shows the drag-to-Applications window,
    // which is how a macOS app has always been installed.
    dialogs::reveal(fs::path(downloaded_path));
    return true;
#else
    // A .deb/.rpm/AppImage is the package manager's business, not ours;
    // replacing a system-installed binary from inside the app would fight
    // whatever installed it. Show the file and stay running.
    dialogs::fReveal(fs::path(downloaded_path).parent_path());
    return false;
#endif
}

void fRunStartupCheck()
{
    if (!fEnabled())
    {
        return;
    }

    std::thread([] {
        try
        {
            const auto update = fCheck();
            if (!update)
            {
                return;
            }

            std::string message = "Lectern " + update->version +
                                  " is available. You have " +
                                  std::string(cVersion) + ".";
            if (!update->notes.empty())
            {
                message += "\n\n" + update->notes;
            }
            message += "\n\nDownload and install it now?";

            if (!dialogs::fConfirm("Update available", message))
            {
                return;
            }

            const auto downloaded = fDownload(*update);
            if (!downloaded)
            {
                dialogs::fConfirm(
                    "Update failed",
                    "The update could not be downloaded or failed its "
                    "integrity check. Nothing has been changed.");
                return;
            }

            if (fInstall(*downloaded))
            {
                // The installer is running; get out of its way.
                std::exit(0);
            }
        }
        catch (const std::exception &error)
        {
            spdlog::debug("update check failed: {}", error.what());
        }
    }).detach();
}

}  // namespace lectern::updater
