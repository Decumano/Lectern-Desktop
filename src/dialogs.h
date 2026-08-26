// Native file dialogs.
//
// saucer 6.0.1 as packaged by vcpkg ships no desktop/dialog module (Tauri had
// tauri-plugin-dialog), so these are implemented per platform: IFileDialog on
// Windows, and the desktop's own dialog helper elsewhere — zenity/kdialog on
// Linux, osascript on macOS — which keeps GTK and Cocoa out of the build
// graph entirely. If a distribution has neither helper installed, the picker
// reports "no dialog available" rather than silently failing.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace lectern::dialogs {

/// Folder picker. Empty result means the user cancelled.
std::optional<std::filesystem::path> pick_folder(const std::string &title);

/// Save-as dialog, pre-filled with `suggested_name`. Empty result means the
/// user cancelled.
std::optional<std::filesystem::path> save_file(
    const std::string &title,
    const std::string &suggested_name,
    const std::string &extension);

/// Yes/no prompt. Used by the updater, which has no UI of its own — the
/// frontend is shared with the web app and knows nothing about updates.
bool confirm(const std::string &title, const std::string &message);

/// Opens a path with the desktop's default handler (Explorer, Finder,
/// xdg-open). Best effort; failure is silent.
void reveal(const std::filesystem::path &path);

}  // namespace lectern::dialogs
