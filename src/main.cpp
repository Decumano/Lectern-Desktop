// Lectern desktop: a saucer webview hosting the shared frontend
//
// The frontend is served over a custom `app://lectern/` scheme
// The editor imports ES modules (pdf.js) and Chromium blocks
// module imports across file:// origins
#include <saucer/smartview.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <expected>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "assets.h"
#include "bridge.h"
#include "cloud.h"
#include "dialogs.h"
#include "embedded.h"
#include "paths.h"
#include "pdf_export.h"
#include "tauri_shim.h"
#include "updater.h"
#include "util.h"
#include "window_manager.h"
#include "workfolder.h"

// Namespace definitions
namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace lectern;

namespace
{
    // custom URL scheme definition
    constexpr const char *cScheme = "app";
    using Answer = std::expected<bridge::raw_json, std::string>;

    // Turns exceptions into rejected promises
    template <typename Fn>

    Answer fAnswer(Fn &&fn)
    {
        try
        {
            return bridge::raw_json(fn());
        }
        catch (const std::exception &error)
        {
            return std::unexpected<std::string>(error.what());
        }
    }

    // translates files to their website name format
    std::string fMimeFor(const std::string &extension)
    {
        static const std::unordered_map<std::string, std::string> cTypes =
        {
            {"html", "text/html"},
            {"js", "text/javascript"},
            {"mjs", "text/javascript"},
            {"css", "text/css"},
            {"json", "application/json"},
            {"svg", "image/svg+xml"},
            {"png", "image/png"},
            {"jpg", "image/jpeg"},
            {"jpeg", "image/jpeg"},
            {"gif", "image/gif"},
            {"webp", "image/webp"},
            {"ico", "image/x-icon"},
            {"woff", "font/woff"},
            {"woff2", "font/woff2"},
            {"ttf", "font/ttf"},
            {"otf", "font/otf"},
            {"wasm", "application/wasm"},
            {"map", "application/json"},
            {"md", "text/markdown"},
            {"txt", "text/plain"},
        };

        const auto it = cTypes.find(extension);
        return it == cTypes.end() ? "application/octet-stream" : it->second;
    }

    // ensures directories URLs can be found by the frontend
    std::string fPathFromUrl(const std::string &url)
    {
        std::string rest = url;

        const size_t scheme_end = rest.find("://");
        if (scheme_end != std::string::npos)
            rest = rest.substr(scheme_end + 3);

        // Drop the host component.
        const size_t slash = rest.find('/');
        rest = (slash == std::string::npos)
                ?
                    std::string{}
                :
                    rest.substr(slash + 1);

        // get HTML file
        const size_t pos = rest.find_first_of("?#");
        if (pos != std::string::npos)
            rest.erase(pos);

        // guarantee a page if none was in the link
        if (rest.empty())
            rest = "index.html";

        // gets relative path
        return paths::fSafeRelPath(rest);
    }

    void fServerFrontend(saucer::scheme::request request, saucer::scheme::executor exec)
    {
        std::string relative;
        try
        {
            // try to get relative path of requested file
            relative = fPathFromUrl(request.url());
        }
        catch (const std::exception &)
        {
            // return error
            exec.reject(saucer::scheme::error::invalid);
            return;
        }

        // read file
        std::vector<std::uint8_t> data;
        
        if (const auto packed = embedded::fRead(relative))
        {
            data.assign(packed->begin(), packed->end());
        }
        else if (const auto bytes = util::fReadFile(assets::fFrontendDir() / relative))
        {
            data.assign(bytes->begin(), bytes->end());
        }
        else
        {
            exec.reject(saucer::scheme::error::not_found);
            return;
        }

        saucer::scheme::response response
        {
            saucer::stash<>::from(std::move(data)),
            fMimeFor(util::fExtensionOf(relative)),
            {},
            200,
        };

        exec.resolve(std::move(response));
    }

    void fExposeCommands(windows::View &webview, const std::shared_ptr<saucer::application> &app)
    {
        // Legacy commands

        webview.expose
        (
            "greet",
            [](std::string name) -> Answer
            {
                return fAnswer
                (
                    [&] 
                    {
                        return json("Hello, " + name + "! You've been greeted from C++!");
                    }
                );
            }
        );

        webview.expose
        (
            "defaultFile",
            [](std::string name) -> Answer 
            {
                return fAnswer
                (
                    [&]
                    {
                        return workfolder::fDefaultFile(name);
                    }
                );
            }
        );

        // save the files

        webview.expose
        (
            "save_file",
            [](std::string name, std::string content) -> Answer
            {
                return fAnswer
                (
                    [&] 
                    {
                        std::string extension = util::fExtensionOf(name);
                        if (extension.empty())
                        {
                            extension = "txt";
                        }

                        const auto chosen = dialogs::fSaveFile("Save file", name, extension);

                        if (!chosen)
                        {
                            return json(false);  // cancelled
                        }

                        util::fWriteFileAtomic(*chosen, content);
                        return json(true);
                    }
                );
            }
        );

        // Desktop PDF export: the frontend has already laid out the pages.
        webview.expose
        (
            "save_pdf",
            [](std::string name, std::string html, double width_mm, double height_mm) -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        if (!pdf::fSupported())
                        {
                            throw std::runtime_error("native PDF export is not available in this build");
                        }

                        const auto chosen = dialogs::fSaveFile("Save PDF", name, "pdf");

                        if (!chosen)
                        {
                            return json(false);  // cancelled
                        }

                        pdf::fRender(html, *chosen, width_mm, height_mm);
                        return json(true);
                    }
                );
            }
        );

        // Work utilities
        webview.expose
        (
            "pick_work_folder", []() -> Answer
            {
                return fAnswer
                (
                    [&] 
                    {
                        const auto picked = dialogs::fPickFolder("Choose a work folder");
                        if (!picked)
                        {
                            return json(nullptr);
                        }
                        // The dialog is the trust boundary: only folders that passed
                        // through it are ever accepted as `root` by the other commands.
                        paths::fAuthorizeRoot(*picked);
                        return json(picked->string());
                    }
               );
            }
        );

        webview.expose
        (
            "list_work_folder",
            [](std::string root) -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        return workfolder::fWalkWorkDir(paths::fCheckRoot(root), "");
                    }
                );
            },
            saucer::launch::async
        );

        webview.expose
        (
            "read_work_file",
            [](std::string root, std::string rel_path) -> Answer
            {
                return fAnswer
                (
                    [&] 
                    {
                        const auto content = util::fReadFile(paths::fResolveWorkPath(root, rel_path));
                        if (!content)
                        {
                            throw std::runtime_error("cannot read file");
                        }
                        return json(*content);
                    }
                );
            },
            saucer::launch::async
        );

        webview.expose
        (
            "write_work_file",
            [](std::string root, std::string rel_path, std::string content) -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        util::fWriteFileAtomic(paths::fResolveWorkPath(root, rel_path), content);
                        cloud::fNudgeSync(root, {rel_path});

                        return json(nullptr);
                    }
                );
            },
            saucer::launch::async
        );

        // Binary files travel as base64, transformed for JSON
        webview.expose
        (
            "read_work_file_base64",
            [](std::string root, std::string rel_path) -> Answer
            {
                return fAnswer
                (
                    [&] 
                    {
                        const auto content = util::fReadFile(paths::fResolveWorkPath(root, rel_path));

                        if (!content)
                            throw std::runtime_error("cannot read file");

                        return json(util::fBase64Encode(std::string_view(*content)));
                    }
                );
            },
            saucer::launch::async
        );

        webview.expose
        (
            "write_work_file_base64",
            [](std::string root, std::string rel_path, std::string data) -> Answer
            {
                return fAnswer
                (
                    [&] 
                    {
                        const auto bytes = util::fBase64Decode(data);
                    
                        if (!bytes)
                            throw std::runtime_error("malformed file data");

                        util::fWriteFileAtomic(paths::fResolveWorkPath(root, rel_path), *bytes);
                        cloud::fNudgeSync(root, {rel_path});

                        return json(nullptr);
                    }
                );
            },
            saucer::launch::async
        );

        webview.expose
        (
            "fetch_remote_file",
            [](std::string url) -> Answer
            {
                return fAnswer
                (
                    [&] 
                    {
                        return workfolder::fFetchRemote(url);
                    }
                );
            },
            saucer::launch::async
        );

        webview.expose
        (
            "create_work_folder",
            [](std::string root, std::string rel_path) -> Answer
            {
                return fAnswer
                (
                    [&] 
                    {
                        std::error_code ec;
                        fs::create_directories(paths::fResolveWorkPath(root, rel_path), ec);

                        if (ec)
                            throw std::runtime_error(ec.message());

                        cloud::fNudgeSync(root, {});  // a folder: the whole tree

                        return json(nullptr);
                    }
                );
            }
        );

        webview.expose
        (
            "delete_work_entry",
            [](std::string root, std::string rel_path, bool is_dir) -> Answer
            {
                return fAnswer
                (
                    [&] 
                    {
                        const fs::path path = paths::fResolveWorkPath(root, rel_path);
                        std::error_code ec;

                        if (is_dir)
                            fs::remove_all(path, ec);
                        
                        else
                            fs::remove(path, ec);

                        if (ec)
                            throw std::runtime_error(ec.message());
                        
                        cloud::fNudgeSync(root, is_dir ? std::vector<std::string>{} : std::vector<std::string>{rel_path});

                        return json(nullptr);
                    }
                );
            }
        );

        webview.expose
        (
            "move_work_entry",
            [](std::string root, std::string from_rel_path, std::string to_rel_path) -> Answer
            {
                return fAnswer
                (
                    [&] 
                    {
                        const fs::path from = paths::fResolveWorkPath(root, from_rel_path);
                        const fs::path to = paths::fResolveWorkPath(root, to_rel_path);

                        std::error_code ec;
                        const bool was_dir = fs::is_directory(from, ec);
                        const fs::path parent = to.parent_path();

                        if (!parent.empty())
                            fs::create_directories(parent, ec);

                        fs::rename(from, to, ec);
                        
                        if (ec)
                            throw std::runtime_error(ec.message());
                        
                        cloud::fNudgeSync(root, was_dir ? std::vector<std::string>{}
                                                        : std::vector<std::string>{from_rel_path, to_rel_path});

                        return json(nullptr);
                    }
                );
            }
        );

        // window/s utilities

        webview.expose
        (
            "open_secondary_window",
            [app](std::string label, std::string url, std::string title, double width, double height, double x, double y) -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        (void)label;  // the frontend's handle name; nothing here needs it

                        windows::Placement placement;
                        placement.width = static_cast<int>(width > 0 ? width : 1100);
                        placement.height = static_cast<int>(height > 0 ? height : 750);
                        // The frontend sends -1 when it has no drop point to place at.

                        if (x >= 0 && y >= 0)
                        {
                            placement.x = static_cast<int>(x);
                            placement.y = static_cast<int>(y);
                        }

                        windows::fOpen(app, url, title, placement);
                        return json(nullptr);
                    }
                );
            }
        );

        // cloud sync utilities (talking to the server)

        webview.expose
        (
            "cloud_connect",
            [](std::string server_url, std::string email, std::string password, std::string root) -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        return cloud::fConnect(server_url, email, password, root);
                    }
                );
            },
            saucer::launch::async
        );

        webview.expose
        (
            "cloud_disconnect",
            []() -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        cloud::fDisconnect();

                        return json(nullptr);
                    }
                );
            }
        );

        webview.expose
        (
            "cloud_status",
            []() -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        return cloud::fStatus();
                    }
                );
            }
        );

        webview.expose
        (
            "cloud_sync_now",
            []() -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        return cloud::fSyncNow();
                    }
                );
            },
            saucer::launch::async
        );

        webview.expose
        (
            "cloud_list_conflicts",
            []() -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        return cloud::fListConflicts();
                    }
                );
            }
        );

        webview.expose
        (
            "cloud_resolve_conflict",
            [](std::string rel_path, std::string choice) -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        cloud::fResolveConflict(rel_path, choice);

                        return json(nullptr);
                    }
                );
            },
            saucer::launch::async
        );

        webview.expose
        (
            "cloud_list_fonts",
            []() -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        return cloud::fListFonts();
                    }
                );
            },
            saucer::launch::async
        );

        webview.expose
        (
            "cloud_font_data_url",
            [](std::string font_id) -> Answer
            {
                return fAnswer
                (
                    [&]
                    {
                        return json(cloud::fFontDataUrl(font_id));
                    }
                );
            },
            saucer::launch::async
        );
    } // fExposeCommands
}  // namespace

int main(int argc, char *argv[])
{
    try
    {
        util::fInitCrypto();
        spdlog::set_level
        (
            spdlog::level::from_str
            (
                util::fEnvOr
                (
                    "LOG_LEVEL",
                    "warn"
                )
            )
        );

        if (embedded::fAvailable())
        {
            spdlog::debug("frontend: {} files embedded in the executable", embedded::fCount());
        }
        else
        {
            const fs::path frontend = assets::fFrontendDir();

            if (!fs::exists(frontend / "index.html"))
            {
                spdlog::error
                (
                    "no index.html under {} — check out the frontend submodule "
                    "(git submodule update --init) or set LECTERN_FRONTEND_DIR",
                    frontend.string()
                );
                return 1;
            }
        }

        // portable versions need to store information wherever the executable lies
        if (embedded::fAvailable())
        {
            const fs::path exe = assets::fExecutablePath();

            if (!exe.empty())
            {
                paths::fUsePortableDataDir(exe.parent_path() / "lectern-data");
            }
        }

        paths::fLoadAuthorizedRoots();

        // the backends register custom schemes with the web engine at initialisation
        saucer::webview::register_scheme(cScheme);

        auto app = saucer::application::init
        (
            {
                .id = "com.lectern.app",
                .argc = argc,
                .argv = argv
            }
        );

        // window configuration
        windows::fSetConfigurator
        (
            [app](windows::View &view)
            {
                view.handle_scheme(cScheme, &fServerFrontend);
                fExposeCommands(view, app);

                // Runs before the page's scripts to avoid 'missing' errors
                view.inject
                (
                    {
                        .code = cTauriShim,
                        .time = saucer::load_time::creation,
                        .frame = saucer::web_frame::top,
                        .permanent = true
                    }
                );
            }
        );

        // When the sync engine writes files into the work folder (a change from
        // the server), tell every window, so an open document shows the new text.
        cloud::fSetFilesChangedHandler
        (
            [](const std::vector<std::string> &changed)
            {
                windows::fBroadcastScript
                (
                    "if (window.fOnCloudFilesChanged) window.fOnCloudFilesChanged(" +
                    json(changed).dump() + ");"
                );
            }
        );

        windows::fOpen(app, "index.html", "Lectern", {});

        // Resumes syncing after a restart without the user reconnecting.
        cloud::fResumeOnStartup();

        // Asks about a newer release, if this build was given a signing key
        // and a manifest URL. No-op otherwise, and inside Flatpak.
        updater::fRunStartupCheck();

        app->run();

        cloud::fShutdown();
        return 0;
    }
    catch (const std::exception &error)
    {
        spdlog::critical("startup failed: {}", error.what());
        return 1;
    }
}
