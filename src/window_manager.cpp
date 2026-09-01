#include "window_manager.h"

#include <spdlog/spdlog.h>

#ifdef _WIN32
// Gives us the window's HWND. saucer 6 has no set_position of its own, and a
// tear-off that ignores where it was dropped feels broken, so the placement
// goes through the platform handle.
//
// objbase.h first: WebView2.h is a COM header and needs the `interface`
// macro, which WIN32_LEAN_AND_MEAN keeps windows.h from defining.
#include <windows.h>
#include <objbase.h>
#include <saucer/modules/stable/webview2.hpp>
#endif

#include <algorithm>
#include <mutex>
#include <vector>

#include "paths.h"
#include "util.h"

namespace lectern::windows {

namespace {

// application::make hands back a unique_ptr with saucer's own deleter, which
// destroys the object on the UI thread. Naming the type lets us keep them in
// a container.
using ViewPtr = std::unique_ptr<View, saucer::safe_deleter<View>>;

struct Registry
{
    std::mutex mutex;
    std::vector<ViewPtr> views;
    Configure configure;
};

Registry &registry()
{
    static Registry instance;
    return instance;
}

/// One profile for every window. The frontend coordinates windows through
/// localStorage and its `storage` events, which only reach documents in the
/// same origin *and the same profile* — give each window its own and the
/// windows stop seeing each other entirely.
std::filesystem::path storage_path()
{
    return paths::app_config_dir() / "webview";
}

constexpr const char *kScheme = "app";
constexpr const char *kHost = "lectern";

/// Turns the frontend's relative `index.html?win=w1` into a full app:// URL,
/// refusing anything that is already absolute.
std::string resolve_url(const std::string &url)
{
    if (url.find("://") != std::string::npos)
    {
        throw std::runtime_error("only relative URLs may open a window");
    }

    std::string path = url;
    while (!path.empty() && path.front() == '/')
    {
        path.erase(0, 1);
    }
    if (path.empty())
    {
        path = "index.html";
    }

    // The query is the whole point here (?win=, ?file=), so validate only the
    // path part — with the same rule the scheme handler applies when serving.
    const size_t query = path.find('?');
    const std::string bare = query == std::string::npos ? path
                                                        : path.substr(0, query);
    paths::safe_rel_path(bare);

    return std::string(kScheme) + "://" + kHost + "/" + path;
}

}  // namespace

void set_configurator(Configure configure)
{
    std::lock_guard<std::mutex> lock(registry().mutex);
    registry().configure = std::move(configure);
}

size_t count()
{
    std::lock_guard<std::mutex> lock(registry().mutex);
    return registry().views.size();
}

View *open(const std::shared_ptr<saucer::application> &app,
          const std::string &url,
          const std::string &title,
          const Placement &placement)
{
    const std::string full_url = resolve_url(url);

    Configure configure;
    {
        std::lock_guard<std::mutex> lock(registry().mutex);
        configure = registry().configure;
    }

    // make() constructs on the UI thread whichever thread calls it, which is
    // what makes this safe to call straight from an exposed function.
    ViewPtr view = app->make<View>(saucer::preferences{
        .application = app,
        .storage_path = storage_path(),
    });

    View *raw = view.get();

    if (configure)
    {
        configure(*raw);
    }

    raw->set_title(title.empty() ? "Lectern" : title);
    raw->set_min_size(640, 480);
    raw->set_size(std::max(400, placement.width),
                  std::max(300, placement.height));
    raw->set_url(full_url);

#ifdef _WIN32
    if (placement.x && placement.y)
    {
        const HWND hwnd =
            static_cast<saucer::window &>(*raw).native<true>().hwnd;
        if (hwnd != nullptr)
        {
            SetWindowPos(hwnd, nullptr, *placement.x, *placement.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
#endif

    // Dropping the window from the registry destroys it, so that has to
    // happen after this handler returns rather than inside it. Posting the
    // erase back to the application defers it to the next turn of the loop.
    raw->on<saucer::window_event::closed>([app, raw]() {
        app->post([app, raw]() {
            bool empty = false;
            {
                std::lock_guard<std::mutex> lock(registry().mutex);
                auto &views = registry().views;
                views.erase(std::remove_if(views.begin(),
                                           views.end(),
                                           [raw](const ViewPtr &held) {
                                               return held.get() == raw;
                                           }),
                            views.end());
                empty = views.empty();
            }

            // The application keeps running with no windows on screen, so
            // closing the last one has to end it explicitly.
            if (empty)
            {
                spdlog::debug("last window closed; quitting");
                app->quit();
            }
        });
    });

    raw->show();

    {
        std::lock_guard<std::mutex> lock(registry().mutex);
        registry().views.push_back(std::move(view));
    }

    spdlog::debug("opened window at {} ({} open)", full_url, count());

    return raw;
}

void close_all()
{
    std::lock_guard<std::mutex> lock(registry().mutex);
    for (auto &view : registry().views)
    {
        view->close();
    }
}

}  // namespace lectern::windows
