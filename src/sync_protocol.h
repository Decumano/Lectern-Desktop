// The desktop's side of the change-notification protocol with the server's
// /api/sync/ws (see Lectern-Server's sync_hub.h for the full description).
//
// The WebSocket never carries file contents. It announces changes and carries
// acknowledgements; the files themselves move over the ordinary HTTP endpoints
// in cloud.cpp. What the protocol buys is the second condition: nothing is
// pushed unless the server has answered, so a server that is down, restarting
// or unreachable is noticed instead of being written at, and the local changes
// simply wait for the next handshake.
//
// This class holds the rules only: no sockets, no threads, an injectable clock.
// sync_socket.cpp owns the connection and calls in here.
#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lectern::cloud {

class SyncProtocol
{
  public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Clock = std::function<TimePoint()>;

    /// How long the server has to answer the greeting or a heartbeat.
    static constexpr std::chrono::seconds cAckTimeout{10};
    /// How long it has to answer "I saved something" before that is taken to
    /// mean it is down.
    static constexpr std::chrono::seconds cAnnounceTimeout{5};

    explicit SyncProtocol(std::string device_id, Clock clock = {});

    /// A connection opened: the frame to send first. Forgets the last one.
    std::string fOnConnected();
    void fOnDisconnected();

    /// True once the server has answered the greeting. Until then (and while
    /// disconnected) no push is allowed.
    bool fReady() const;

    struct Reaction
    {
        std::vector<std::string> send;
        /// Pull the server's changes and push ours: the server answered the
        /// greeting or an announcement, or (once it has answered the greeting)
        /// told us something changed or asked for our changes. Never set
        /// before the greeting is answered.
        bool reconcile = false;
        /// What to reconcile: the named files only (a save, or a change the
        /// server announced file by file), or, when `everything`, the whole
        /// folder (the greeting, a restarted server, a folder operation).
        std::vector<std::string> paths;
        bool everything = false;
    };

    Reaction fOnFrame(const std::string &text);

    /// A local save of `paths` (empty: something structural, like a folder).
    /// The frame to send, or nullopt when the server hasn't answered yet; the
    /// change is then simply pushed by the reconcile that follows the next
    /// acknowledged greeting.
    std::optional<std::string> fAnnounce(const std::vector<std::string> &paths = {});

    /// Heartbeat: tells a quietly dead connection from an idle one.
    std::string fPing();

    /// True when something we sent went unanswered past its deadline, meaning
    /// the server is down or wedged and the connection should be dropped.
    bool fExpired() const;

  private:
    enum class Kind
    {
        Hello,
        Announce,
        Ping
    };

    struct Pending
    {
        Kind kind;
        TimePoint deadline;
        std::vector<std::string> paths;  // for an announcement: what it was about
    };

    std::string fTrack(const char *type, Kind kind, std::chrono::seconds timeout);
    TimePoint fNow() const;

    std::string device_id_;
    Clock clock_;
    bool ready_ = false;
    uint64_t next_id_ = 1;
    std::map<std::string, Pending> pending_;
};

}  // namespace lectern::cloud
