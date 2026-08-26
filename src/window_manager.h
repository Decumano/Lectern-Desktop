// Multiple application windows.
//
// The frontend supports tearing a tab out into its own window: dragging a tab
// past the edge of the window opens a new one, and dropping it on another
// Lectern window moves it there. Both halves need the backend to be able to
// create a second window, which the Tauri build does through
// `new WebviewWindow(label, options)` — so the shim in tauri_shim.h presents
// that same API and it lands here.
//
// Every window is the whole app booted at a different URL: the main one at
// index.html, a torn-off one at index.html?win=<id>&file=<id>, which the
// frontend renders with the sidebar hidden and its own tab list. Windows talk
// to each other over localStorage, so they must share one storage profile —
// that is why the storage path is decided once, here, rather than per window.
// Named window_manager rather than windows: a header called windows.h in
// the target's include path shadows the Windows SDK's, and every downstream
// translation unit fails deep inside rpcasync.h with errors that name
// neither file.
#pragma once

#include <saucer/smartview.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "bridge.h"

namespace lectern::windows {

using View = saucer::smartview<bridge::serializer>;

/// Where a new window should appear. Absent means "let the OS decide".
struct Placement
{
    std::optional<int> x;
    std::optional<int> y;
    int width = 1280;
    int height = 800;
};

/// Called for each new window so it gets the same scheme handler, exposed
/// functions and injected shim as the first one. Set once from main().
using Configure = std::function<void(View &)>;

void set_configurator(Configure configure);

/// Creates, shows and registers a window at `url`, which must be a path
/// relative to the frontend root — `index.html?win=w1` and the like. Absolute
/// URLs are refused: a page must not be able to talk this into opening a
/// window onto somewhere else.
///
/// Safe to call from an exposed function; the window is created on the UI
/// thread regardless of which thread asks.
View *open(const std::shared_ptr<saucer::application> &app,
          const std::string &url,
          const std::string &title,
          const Placement &placement);

/// How many windows are currently open.
size_t count();

/// Closes every window, which lets the application's run loop finish.
void close_all();

}  // namespace lectern::windows
