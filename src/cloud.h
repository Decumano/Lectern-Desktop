// Cloud sync engine: connects the desktop app's active work folder to a
// Lectern server, always over HTTP — even when the server happens to be on the
// same machine, so the app's folder never has to change out from under the
// user. A background loop + push-on-write reconcile the local folder against
// the server, using a Git-style three-way merge: edits to different regions of
// a file combine automatically, edits to the same region are flagged as a
// conflict for the user to resolve.
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace lectern::cloud {

/// Fired after a local write/move/delete/create succeeds, so pushes feel close
/// to instant instead of waiting for the next poll tick. Cheap no-op when
/// cloud sync isn't configured.
void nudge_sync();

/// Called once at startup so a connection resumes syncing after the app
/// restarts without the user reconnecting.
void resume_on_startup();

/// Stops the background loop and waits for it. Called on shutdown.
void shutdown();

// ── The functions exposed to the frontend (see main.cpp) ──

nlohmann::json connect(const std::string &server_url,
                       const std::string &email,
                       const std::string &password,
                       const std::string &root);
void disconnect();
nlohmann::json status();
nlohmann::json sync_now();
nlohmann::json list_conflicts();
void resolve_conflict(const std::string &rel_path, const std::string &choice);
nlohmann::json list_fonts();
std::string font_data_url(const std::string &font_id);

}  // namespace lectern::cloud
