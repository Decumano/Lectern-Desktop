// The desktop's live connection to the server: one WebSocket that tells it when
// the server's files changed, tells the server when ours did, and replaces the
// timer that used to poll every 45 seconds. See sync_protocol.h for the rules
// about acknowledgements.
//
// Two threads. The socket thread owns the connection, answers the server and
// reconnects (with a growing pause, since a server that is down is expected and
// should not be hammered) whenever it is lost. The worker thread runs the actual
// reconciliation (cloud.cpp's three-way merge over HTTP) whenever the protocol
// says there is something to do, so a long sync never delays an acknowledgement.
#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "sync_protocol.h"

typedef void CURL;

namespace lectern::cloud {

struct SyncEndpoint
{
    /// The server's base URL as the user typed it (http:// or https://).
    std::string server_url;
    std::string api_token;
    std::string device_id;
};

/// "https://host:8080/" -> "wss://host:8080/api/sync/ws"; empty for a URL that
/// is neither http nor https.
std::string fSyncSocketUrl(const std::string &server_url);

class SyncSocket
{
  public:
    struct Hooks
    {
        /// The current connection settings, read afresh on every (re)connect;
        /// nullopt once the user has disconnected, which ends the thread.
        std::function<std::optional<SyncEndpoint>()> endpoint;
        /// Pull the server's changes and push ours, for just `paths` or, when
        /// that is empty, the whole folder. May throw; runs on the worker thread.
        std::function<void(const std::vector<std::string> &paths)> reconcile;
    };

    explicit SyncSocket(Hooks hooks);
    ~SyncSocket();

    SyncSocket(const SyncSocket &) = delete;
    SyncSocket &operator=(const SyncSocket &) = delete;

    /// Starts connecting. If already running, drops the current connection so
    /// the next one uses the settings as they are now.
    void fStart();

    /// Closes the connection and stops both threads.
    void fStop();

    /// A local file was saved. If the server has answered our greeting it is
    /// told, and the push happens once it acknowledges; otherwise nothing is
    /// pushed now and the change goes up with the reconcile that follows the
    /// next successful greeting.
    void fAnnounceLocalChange(const std::vector<std::string> &paths);

    /// True while connected and the server has answered the greeting.
    bool fLive() const;

  private:
    void fSocketThread();
    void fWorkerThread();
    /// One connection, from handshake to loss. Returns whether it got as far
    /// as being established.
    bool fRunSession(const SyncEndpoint &endpoint);
    /// Sleeps, but wakes early for a stop or a reconnect request.
    void fPause(std::chrono::milliseconds duration);
    void fRequestReconcile(const SyncProtocol::Reaction &reaction);
    bool fStopping() const;

    Hooks hooks_;

    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool stop_ = false;
    bool reconnect_ = false;
    bool reconcile_requested_ = false;
    bool reconcile_everything_ = false;
    std::set<std::string> reconcile_paths_;
    bool running_ = false;

    std::mutex lifecycle_mutex_;
    std::thread socket_thread_;
    std::thread worker_thread_;

    /// Guards the curl handle and the protocol: the socket thread and the
    /// threads that announce saves both talk through them.
    mutable std::mutex io_mutex_;
    CURL *curl_ = nullptr;
    std::unique_ptr<SyncProtocol> protocol_;
};

}  // namespace lectern::cloud
