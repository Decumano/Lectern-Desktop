#include "sync_protocol.h"

namespace lectern::cloud {

namespace {

using json = nlohmann::json;

std::string fStringField(const json &frame, const char *key)
{
    const auto it = frame.find(key);
    return (it != frame.end() && it->is_string()) ? it->get<std::string>()
                                                    : std::string{};
}

}  // namespace

SyncProtocol::SyncProtocol(std::string device_id, Clock clock)
    : device_id_(std::move(device_id)),
      clock_(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); }))
{
}

SyncProtocol::TimePoint SyncProtocol::fNow() const
{
    return clock_();
}

std::string SyncProtocol::fTrack(const char *type, Kind kind,
                                  std::chrono::seconds timeout)
{
    const std::string id = "d" + std::to_string(next_id_++);
    pending_[id] = {kind, fNow() + timeout, {}};
    return json{{"type", type}, {"id", id}}.dump();
}

std::string SyncProtocol::fOnConnected()
{
    ready_ = false;
    pending_.clear();

    const std::string id = "d" + std::to_string(next_id_++);
    pending_[id] = {Kind::Hello, fNow() + cAckTimeout, {}};
    return json{{"type", "hello"}, {"id", id}, {"device", device_id_}, {"client", "desktop"}}.dump();
}

void SyncProtocol::fOnDisconnected()
{
    ready_ = false;
    pending_.clear();
}

bool SyncProtocol::fReady() const
{
    return ready_;
}

SyncProtocol::Reaction SyncProtocol::fOnFrame(const std::string &text)
{
    Reaction reaction;

    // The frame comes off the network: anything malformed is ignored rather
    // than allowed to throw into the socket thread.
    const json frame = json::parse(text, nullptr, false);
    if (frame.is_discarded() || !frame.is_object())
    {
        return reaction;
    }

    const std::string type = fStringField(frame, "type");
    const std::string id = fStringField(frame, "id");

    if (type == "ack")
    {
        const auto it = pending_.find(id);
        if (it == pending_.end())
        {
            return reaction;
        }
        const Kind kind = it->second.kind;
        std::vector<std::string> announced = std::move(it->second.paths);
        pending_.erase(it);

        if (kind == Kind::Hello)
        {
            ready_ = true;
            reaction.reconcile = true;  // the server answered: ask it for changes
            reaction.everything = true;
        }
        else if (kind == Kind::Announce)
        {
            reaction.reconcile = true;  // the server answered: now push
            reaction.paths = std::move(announced);
            reaction.everything = reaction.paths.empty();
        }
        return reaction;
    }

    if (type == "changes" || type == "request_changes")
    {
        // Answer first, so the server knows we are here, then go and look. The
        // looking waits for the server to have answered our own greeting: a
        // server that asks but never answers is not one to sync with, and the
        // acknowledged greeting runs the same pass as soon as it comes.
        if (!id.empty())
        {
            reaction.send.push_back(json{{"type", "ack"}, {"id", id}}.dump());
        }
        reaction.reconcile = ready_;

        // "changes" says which files; anything else (or a list too long to send)
        // means look at everything.
        const auto listed = frame.find("paths");
        if (type == "changes" && listed != frame.end() && listed->is_array())
        {
            for (const auto &entry : *listed)
            {
                if (entry.is_string())
                {
                    reaction.paths.push_back(entry.get<std::string>());
                }
            }
        }
        reaction.everything = reaction.paths.empty();
    }
    return reaction;
}

std::optional<std::string> SyncProtocol::fAnnounce(const std::vector<std::string> &paths)
{
    if (!ready_)
    {
        return std::nullopt;
    }

    const std::string id = "d" + std::to_string(next_id_++);
    pending_[id] = {Kind::Announce, fNow() + cAnnounceTimeout, paths};

    // Too long a list is not worth sending; "everything" says the same.
    constexpr size_t cMaxPathsPerFrame = 100;
    json frame = {{"type", "changes"}, {"id", id}, {"paths", json::array()}};
    if (paths.size() <= cMaxPathsPerFrame)
    {
        for (const auto &path : paths)
        {
            frame["paths"].push_back(path);
        }
    }
    else
    {
        pending_[id].paths.clear();
    }
    return frame.dump();
}

std::string SyncProtocol::fPing()
{
    return fTrack("ping", Kind::Ping, cAckTimeout);
}

bool SyncProtocol::fExpired() const
{
    const TimePoint now = fNow();
    for (const auto &[id, pending] : pending_)
    {
        if (now >= pending.deadline)
        {
            return true;
        }
    }
    return false;
}

}  // namespace lectern::cloud
