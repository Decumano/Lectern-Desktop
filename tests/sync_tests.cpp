// The desktop's side of the change-notification protocol: when it may push,
// and when it gives up on a server that has stopped answering.
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "sync_protocol.h"

using lectern::cloud::SyncProtocol;
using nlohmann::json;

namespace {

struct Fixture
{
    SyncProtocol::TimePoint now = std::chrono::steady_clock::now();
    SyncProtocol protocol{"dev-1", [this] { return now; }};

    void fAdvance(int seconds)
    {
        now += std::chrono::seconds(seconds);
    }

    static std::string fAckFor(const std::string &frame)
    {
        return json{{"type", "ack"}, {"id", json::parse(frame)["id"]}}.dump();
    }

    /// Connects and has the server answer the greeting.
    void fHandshake()
    {
        const std::string hello = protocol.fOnConnected();
        protocol.fOnFrame(fAckFor(hello));
    }
};

}  // namespace

TEST_CASE("the greeting names this device", "[sync]")
{
    Fixture f;
    const json hello = json::parse(f.protocol.fOnConnected());
    REQUIRE(hello["type"] == "hello");
    REQUIRE(hello["device"] == "dev-1");
    REQUIRE(hello["id"].is_string());
}

TEST_CASE("nothing may be pushed until the server has answered the greeting", "[sync]")
{
    Fixture f;
    REQUIRE_FALSE(f.protocol.fReady());
    REQUIRE_FALSE(f.protocol.fAnnounce());  // never connected

    const std::string hello = f.protocol.fOnConnected();
    REQUIRE_FALSE(f.protocol.fReady());
    REQUIRE_FALSE(f.protocol.fAnnounce());  // connected, unanswered

    const auto reaction = f.protocol.fOnFrame(Fixture::fAckFor(hello));
    REQUIRE(f.protocol.fReady());
    REQUIRE(reaction.reconcile);  // startup: ask for the server's changes, push ours
    REQUIRE(f.protocol.fAnnounce());
}

TEST_CASE("a saved file is announced and pushed only once the announcement is answered", "[sync]")
{
    Fixture f;
    f.fHandshake();

    const auto frame = f.protocol.fAnnounce();
    REQUIRE(frame);
    REQUIRE(json::parse(*frame)["type"] == "changes");

    REQUIRE(f.protocol.fOnFrame(Fixture::fAckFor(*frame)).reconcile);
}

TEST_CASE("an unanswered announcement means the server is down", "[sync]")
{
    Fixture f;
    f.fHandshake();
    REQUIRE_FALSE(f.protocol.fExpired());

    f.protocol.fAnnounce();
    f.fAdvance(4);
    REQUIRE_FALSE(f.protocol.fExpired());
    f.fAdvance(2);
    REQUIRE(f.protocol.fExpired());
}

TEST_CASE("an unanswered greeting also means the server is down", "[sync]")
{
    Fixture f;
    f.protocol.fOnConnected();
    f.fAdvance(11);
    REQUIRE(f.protocol.fExpired());
}

TEST_CASE("an answered frame no longer expires", "[sync]")
{
    Fixture f;
    f.fHandshake();
    const auto frame = f.protocol.fAnnounce();
    f.protocol.fOnFrame(Fixture::fAckFor(*frame));

    f.fAdvance(60);
    REQUIRE_FALSE(f.protocol.fExpired());
}

TEST_CASE("heartbeats are tracked and acknowledged without reconciling", "[sync]")
{
    Fixture f;
    f.fHandshake();

    const std::string ping = f.protocol.fPing();
    f.fAdvance(11);
    REQUIRE(f.protocol.fExpired());

    Fixture g;
    g.fHandshake();
    const std::string ping2 = g.protocol.fPing();
    REQUIRE_FALSE(g.protocol.fOnFrame(Fixture::fAckFor(ping2)).reconcile);
    g.fAdvance(60);
    REQUIRE_FALSE(g.protocol.fExpired());
}

TEST_CASE("a change announced by the server is acknowledged, then pulled", "[sync]")
{
    Fixture f;
    f.fHandshake();

    const auto reaction =
        f.protocol.fOnFrame(R"({"type":"changes","id":"s7","paths":["a.mdp"]})");
    REQUIRE(reaction.reconcile);
    REQUIRE(reaction.send.size() == 1);
    const json ack = json::parse(reaction.send[0]);
    REQUIRE(ack["type"] == "ack");
    REQUIRE(ack["id"] == "s7");
}

TEST_CASE("a server that has just restarted asks for our changes", "[sync]")
{
    Fixture f;
    const std::string hello = f.protocol.fOnConnected();

    // The server sends request_changes as soon as we connect, before it has
    // answered the greeting: it is acknowledged, but nothing is synced yet.
    const auto early = f.protocol.fOnFrame(R"({"type":"request_changes","id":"s1"})");
    REQUIRE(json::parse(early.send.at(0))["id"] == "s1");
    REQUIRE_FALSE(early.reconcile);

    // The answer to the greeting then starts the pass.
    REQUIRE(f.protocol.fOnFrame(Fixture::fAckFor(hello)).reconcile);

    // A later request, with the greeting answered, syncs at once.
    REQUIRE(f.protocol.fOnFrame(R"({"type":"request_changes","id":"s2"})").reconcile);
}

TEST_CASE("a server that never answers the greeting is never synced with", "[sync]")
{
    Fixture f;
    f.protocol.fOnConnected();

    REQUIRE_FALSE(f.protocol.fOnFrame(R"({"type":"request_changes","id":"s1"})").reconcile);
    REQUIRE_FALSE(f.protocol.fOnFrame(R"({"type":"changes","id":"s2","paths":[]})").reconcile);
    f.fAdvance(11);
    REQUIRE(f.protocol.fExpired());
}

TEST_CASE("reconnecting forgets what the last connection was waiting for", "[sync]")
{
    Fixture f;
    f.fHandshake();
    f.protocol.fAnnounce();

    f.protocol.fOnDisconnected();
    REQUIRE_FALSE(f.protocol.fReady());
    REQUIRE_FALSE(f.protocol.fAnnounce());
    f.fAdvance(60);
    REQUIRE_FALSE(f.protocol.fExpired());
}

TEST_CASE("an acknowledgement nobody asked for is ignored", "[sync]")
{
    Fixture f;
    f.protocol.fOnConnected();
    const auto reaction = f.protocol.fOnFrame(R"({"type":"ack","id":"d999"})");
    REQUIRE_FALSE(reaction.reconcile);
    REQUIRE_FALSE(f.protocol.fReady());
}

TEST_CASE("malformed frames from the network are ignored", "[sync]")
{
    Fixture f;
    f.fHandshake();
    for (const char *junk : {"", "nope", "[]", "7", R"({"type":3})", R"({"type":"ack","id":null})",
                             R"({"type":"changes","id":{}})"})
    {
        REQUIRE_NOTHROW(f.protocol.fOnFrame(junk));
    }
    REQUIRE(f.protocol.fReady());
}

// ── Which files a reconcile is about ──

TEST_CASE("the greeting and a restarted server reconcile everything", "[sync]")
{
    Fixture f;
    const std::string hello = f.protocol.fOnConnected();

    const auto reaction = f.protocol.fOnFrame(Fixture::fAckFor(hello));
    REQUIRE(reaction.reconcile);
    REQUIRE(reaction.everything);

    REQUIRE(f.protocol.fOnFrame(R"({"type":"request_changes","id":"s1"})").everything);
}

TEST_CASE("a change announced file by file reconciles just those files", "[sync]")
{
    Fixture f;
    f.fHandshake();

    const auto reaction = f.protocol.fOnFrame(
        R"({"type":"changes","id":"s2","paths":["a.mdp","lore/b.mdp"]})");
    REQUIRE(reaction.reconcile);
    REQUIRE_FALSE(reaction.everything);
    REQUIRE(reaction.paths == std::vector<std::string>{"a.mdp", "lore/b.mdp"});
}

TEST_CASE("a change with no file list means look at everything", "[sync]")
{
    Fixture f;
    f.fHandshake();

    // A folder was made or removed, or too many files changed to list.
    REQUIRE(f.protocol.fOnFrame(R"({"type":"changes","id":"s3","paths":[]})").everything);
    REQUIRE(f.protocol.fOnFrame(R"({"type":"changes","id":"s4"})").everything);
}

TEST_CASE("a saved file is announced by name and only that file is pushed", "[sync]")
{
    Fixture f;
    f.fHandshake();

    const auto frame = f.protocol.fAnnounce({"notes/a.mdp"});
    REQUIRE(frame);
    REQUIRE(json::parse(*frame)["paths"] == json::array({"notes/a.mdp"}));

    const auto reaction = f.protocol.fOnFrame(Fixture::fAckFor(*frame));
    REQUIRE(reaction.reconcile);
    REQUIRE_FALSE(reaction.everything);
    REQUIRE(reaction.paths == std::vector<std::string>{"notes/a.mdp"});
}

TEST_CASE("an announcement with no files, or too many, pushes everything", "[sync]")
{
    Fixture f;
    f.fHandshake();

    const auto none = f.protocol.fAnnounce({});
    REQUIRE(f.protocol.fOnFrame(Fixture::fAckFor(*none)).everything);

    std::vector<std::string> many;
    for (int i = 0; i < 150; ++i)
    {
        many.push_back("f" + std::to_string(i) + ".mdp");
    }
    const auto big = f.protocol.fAnnounce(many);
    REQUIRE(json::parse(*big)["paths"].empty());
    REQUIRE(f.protocol.fOnFrame(Fixture::fAckFor(*big)).everything);
}
