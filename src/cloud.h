// Cloud sync engine: connects the desktop app's active work folder to a
// Lectern server. File contents always move over HTTP — even when the server
// happens to be on the same machine, so the app's folder never has to change
// out from under the user — and are reconciled with a Git-style three-way
// merge: edits to different regions of a file combine automatically, edits to
// the same region are flagged as a conflict for the user to resolve.
//
// What triggers a reconciliation is a WebSocket to the server (sync_socket.h),
// not a timer. On start the desktop greets the server and, once it answers,
// pulls what changed and pushes what it has; afterwards the server announces
// its changes and the desktop announces its own on every save. Nothing is
// pushed unless the other side acknowledged, so a server that is down is
// noticed instead of written at, and local changes wait for the next
// handshake.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace lectern::cloud {

/// Fired after a local write/move/delete/create succeeds in `root`, with the
/// files (relative to it) it was about (none: a folder, or anything not a
/// single file). Tells the server and,
/// once it acknowledges, pushes just those. A no-op when cloud sync isn't configured, and
/// while the server isn't answering (the change goes up on reconnect).
void fNudgeSync(const std::string &root, const std::vector<std::string> &rel_paths);

/// Called with the work-folder-relative paths of files the sync engine has just
/// written (pulled from the server, merged, or deleted), from a worker thread.
/// main() uses it to tell the page, so an open document shows the new text.
using FilesChangedHandler = std::function<void(const std::vector<std::string> &)>;
void fSetFilesChangedHandler(FilesChangedHandler handler);

/// Called once at startup so a connection resumes after the app restarts
/// without the user reconnecting: it greets the server and syncs on its answer.
void fResumeOnStartup();

/// Closes the live connection and waits for its threads. Called on shutdown.
void fShutdown();

// ── The functions exposed to the frontend (see main.cpp) ──

nlohmann::json fConnect(const std::string &server_url,
                       const std::string &email,
                       const std::string &password,
                       const std::string &root);
void fDisconnect();
nlohmann::json fStatus();
nlohmann::json fSyncNow();
nlohmann::json fListConflicts();
void fResolveConflict(const std::string &rel_path, const std::string &choice);
nlohmann::json fListFonts();
std::string fFontDataUrl(const std::string &font_id);

}  // namespace lectern::cloud
