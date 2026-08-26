// Lectern desktop: a saucer webview hosting the shared frontend, with the
// same set of functions exposed to JavaScript that the Tauri build exposed as
// commands (see the frontend's platform.js).
//
// The frontend is served over a custom `app://` scheme rather than loaded from
// a file:// URL: the editor imports ES modules (pdf.js) and Chromium blocks
// module imports across file:// origins, so a file-loaded page would lose PDF
// import and annotation entirely.
#include <saucer/smartview.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <expected>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>

#include "assets.h"
#include "bridge.h"
#include "cloud.h"
#include "dialogs.h"
#include "embedded.h"
#include "paths.h"
#include "tauri_shim.h"
#include "updater.h"
#include "util.h"
#include "window_manager.h"
#include "workfolder.h"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace lectern;

namespace {

constexpr const char *kScheme = "app";

/// The equivalent of the Tauri commands' `Result<T, String>`: saucer resolves
/// the JS promise with the value, or rejects it with the error string.
using Answer = std::expected<bridge::raw_json, std::string>;

/// Runs a command body, turning any exception into a rejected promise. Every
/// command answers with JSON built by nlohmann, handed over verbatim.
template <typename Fn>
Answer answer(Fn &&fn)
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

std::string mime_for(const std::string &extension)
{
    static const std::unordered_map<std::string, std::string> kTypes = {
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

    const auto it = kTypes.find(extension);
    return it == kTypes.end() ? "application/octet-stream" : it->second;
}

/// Turns `app://lectern/vendor/katex/katex.min.css?v=3` into
/// `vendor/katex/katex.min.css`, refusing anything that would escape the
/// frontend directory.
std::string path_from_url(const std::string &url)
{
    std::string rest = url;

    const size_t scheme_end = rest.find("://");
    if (scheme_end != std::string::npos)
    {
        rest = rest.substr(scheme_end + 3);
    }

    // Drop the host component.
    const size_t slash = rest.find('/');
    rest = slash == std::string::npos ? std::string{} : rest.substr(slash + 1);

    // Drop query and fragment — index.html uses `?v=N` for cache busting.
    for (const char terminator : {'?', '#'})
    {
        const size_t pos = rest.find(terminator);
        if (pos != std::string::npos)
        {
            rest = rest.substr(0, pos);
        }
    }

    if (rest.empty())
    {
        rest = "index.html";
    }

    // Same guarantee as the work-folder commands: no `..`, no absolute path,
    // no drive prefix. A page can't read outside its own bundle.
    return paths::safe_rel_path(rest);
}

void serve_frontend(saucer::scheme::request request,
                    saucer::scheme::executor exec)
{
    std::string relative;
    try
    {
        relative = path_from_url(request.url());
    }
    catch (const std::exception &)
    {
        exec.reject(saucer::scheme::error::invalid);
        return;
    }

    // A portable single-file build carries the assets inside the executable;
    // every other build reads them from disk beside it.
    std::vector<std::uint8_t> data;
    if (const auto packed = embedded::read(relative))
    {
        data.assign(packed->begin(), packed->end());
    }
    else if (const auto bytes = util::read_file(assets::frontend_dir() / relative))
    {
        data.assign(bytes->begin(), bytes->end());
    }
    else
    {
        exec.reject(saucer::scheme::error::not_found);
        return;
    }

    saucer::scheme::response response{
        .data = saucer::stash<>::from(std::move(data)),
        .mime = mime_for(util::extension_of(relative)),
        .headers = {},
        .status = 200,
    };
    exec.resolve(std::move(response));
}

void expose_commands(windows::View &webview,
                     const std::shared_ptr<saucer::application> &app)
{
    // ── Legacy/demo commands the frontend still references ──

    webview.expose("greet", [](std::string name) -> Answer {
        return answer([&] {
            return json("Hello, " + name + "! You've been greeted from C++!");
        });
    });

    webview.expose("defaultFile", [](std::string name) -> Answer {
        return answer([&] { return workfolder::default_file(name); });
    });

    // ── Save-as ──

    webview.expose(
        "save_file", [](std::string name, std::string content) -> Answer {
            return answer([&] {
                std::string extension = util::extension_of(name);
                if (extension.empty())
                {
                    extension = "txt";
                }

                const auto chosen =
                    dialogs::save_file("Save file", name, extension);
                if (!chosen)
                {
                    return json(false);  // cancelled
                }

                util::write_file_atomic(*chosen, content);
                return json(true);
            });
        });

    // ── Work folder ──

    webview.expose("pick_work_folder", []() -> Answer {
        return answer([&] {
            const auto picked = dialogs::pick_folder("Choose a work folder");
            if (!picked)
            {
                return json(nullptr);
            }
            // The dialog is the trust boundary: only folders that passed
            // through it are ever accepted as `root` by the other commands.
            paths::authorize_root(*picked);
            return json(picked->string());
        });
    });

    webview.expose("list_work_folder", [](std::string root) -> Answer {
        return answer(
            [&] { return workfolder::walk_work_dir(paths::check_root(root), ""); });
    });

    webview.expose(
        "read_work_file",
        [](std::string root, std::string rel_path) -> Answer {
            return answer([&] {
                const auto content =
                    util::read_file(paths::resolve_work_path(root, rel_path));
                if (!content)
                {
                    throw std::runtime_error("cannot read file");
                }
                return json(*content);
            });
        });

    webview.expose("write_work_file",
                   [](std::string root, std::string rel_path,
                      std::string content) -> Answer {
                       return answer([&] {
                           util::write_file_atomic(
                               paths::resolve_work_path(root, rel_path),
                               content);
                           cloud::nudge_sync();
                           return json(nullptr);
                       });
                   });

    webview.expose(
        "create_work_folder",
        [](std::string root, std::string rel_path) -> Answer {
            return answer([&] {
                std::error_code ec;
                fs::create_directories(paths::resolve_work_path(root, rel_path),
                                       ec);
                if (ec)
                {
                    throw std::runtime_error(ec.message());
                }
                cloud::nudge_sync();
                return json(nullptr);
            });
        });

    webview.expose(
        "delete_work_entry",
        [](std::string root, std::string rel_path, bool is_dir) -> Answer {
            return answer([&] {
                const fs::path path =
                    paths::resolve_work_path(root, rel_path);
                std::error_code ec;
                if (is_dir)
                {
                    fs::remove_all(path, ec);
                }
                else
                {
                    fs::remove(path, ec);
                }
                if (ec)
                {
                    throw std::runtime_error(ec.message());
                }
                cloud::nudge_sync();
                return json(nullptr);
            });
        });

    webview.expose("move_work_entry",
                   [](std::string root, std::string from_rel_path,
                      std::string to_rel_path) -> Answer {
                       return answer([&] {
                           const fs::path from = paths::resolve_work_path(
                               root, from_rel_path);
                           const fs::path to =
                               paths::resolve_work_path(root, to_rel_path);

                           std::error_code ec;
                           const fs::path parent = to.parent_path();
                           if (!parent.empty())
                           {
                               fs::create_directories(parent, ec);
                           }
                           fs::rename(from, to, ec);
                           if (ec)
                           {
                               throw std::runtime_error(ec.message());
                           }
                           cloud::nudge_sync();
                           return json(nullptr);
                       });
                   });

    // ── Windows ──
    //
    // The frontend asks for a new window through the Tauri WebviewWindow API,
    // which tauri_shim.h maps onto this. Runs on the UI thread (launch::sync)
    // because that is where a window has to be created.

    webview.expose(
        "open_secondary_window",
        [app](std::string label, std::string url, std::string title,
              double width, double height, double x, double y) -> Answer {
            return answer([&] {
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

                windows::open(app, url, title, placement);
                return json(nullptr);
            });
        });

    // ── Cloud sync. These talk to a server, so they run off the UI thread. ──

    webview.expose(
        "cloud_connect",
        [](std::string server_url, std::string email, std::string password,
           std::string root) -> Answer {
            return answer([&] {
                return cloud::connect(server_url, email, password, root);
            });
        },
        saucer::launch::async);

    webview.expose("cloud_disconnect", []() -> Answer {
        return answer([&] {
            cloud::disconnect();
            return json(nullptr);
        });
    });

    webview.expose("cloud_status", []() -> Answer {
        return answer([&] { return cloud::status(); });
    });

    webview.expose(
        "cloud_sync_now",
        []() -> Answer { return answer([&] { return cloud::sync_now(); }); },
        saucer::launch::async);

    webview.expose("cloud_list_conflicts", []() -> Answer {
        return answer([&] { return cloud::list_conflicts(); });
    });

    webview.expose(
        "cloud_resolve_conflict",
        [](std::string rel_path, std::string choice) -> Answer {
            return answer([&] {
                cloud::resolve_conflict(rel_path, choice);
                return json(nullptr);
            });
        },
        saucer::launch::async);

    webview.expose(
        "cloud_list_fonts",
        []() -> Answer { return answer([&] { return cloud::list_fonts(); }); },
        saucer::launch::async);

    webview.expose(
        "cloud_font_data_url",
        [](std::string font_id) -> Answer {
            return answer(
                [&] { return json(cloud::font_data_url(font_id)); });
        },
        saucer::launch::async);
}

}  // namespace

int main(int argc, char *argv[])
{
    try
    {
        util::init_crypto();
        spdlog::set_level(
            spdlog::level::from_str(util::env_or("LOG_LEVEL", "warn")));

        if (embedded::available())
        {
            spdlog::debug("frontend: {} files embedded in the executable",
                          embedded::count());
        }
        else
        {
            const fs::path frontend = assets::frontend_dir();
            if (!fs::exists(frontend / "index.html"))
            {
                spdlog::error(
                    "no index.html under {} — check out the frontend submodule "
                    "(git submodule update --init) or set LECTERN_FRONTEND_DIR",
                    frontend.string());
                return 1;
            }
        }

        // A portable build keeps its state — config, and the webview's
        // profile — beside the executable rather than in the user's profile,
        // so copying the .exe somewhere else takes everything with it.
        if (embedded::available())
        {
            const fs::path exe = assets::executable_path();
            if (!exe.empty())
            {
                paths::use_portable_data_dir(exe.parent_path() /
                                             "lectern-data");
            }
        }

        paths::load_authorized_roots();

        // Must happen before the application exists: the backends register
        // custom schemes with the web engine at initialisation time.
        saucer::webview::register_scheme(kScheme);

        auto app = saucer::application::init({
            .id = "com.lectern.app",
            .argc = argc,
            .argv = argv,
        });

        // Every window — the first one and any tab torn off into its own —
        // gets the same wiring, so this is set up once and reused.
        windows::set_configurator([app](windows::View &view) {
            view.handle_scheme(kScheme, &serve_frontend);
            expose_commands(view, app);

            // Runs before the page's own scripts, so platform.js finds the
            // desktop backend where it looks for it. Top frame only, matching
            // Tauri: the split-pane iframe borrows the parent's __TAURI__.
            view.inject({
                .code = kTauriShim,
                .time = saucer::load_time::creation,
                .frame = saucer::web_frame::top,
                .permanent = true,
            });
        });

        windows::open(app, "index.html", "Lectern", {});

        // Resumes syncing after a restart without the user reconnecting.
        cloud::resume_on_startup();

        // Asks about a newer release, if this build was given a signing key
        // and a manifest URL. No-op otherwise, and inside Flatpak.
        updater::run_startup_check();

        app->run();

        cloud::shutdown();
        return 0;
    }
    catch (const std::exception &error)
    {
        spdlog::critical("startup failed: {}", error.what());
        return 1;
    }
}
