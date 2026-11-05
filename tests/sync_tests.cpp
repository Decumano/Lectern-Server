// The change-notification hub: acknowledgements, timeouts, routing. No server
// involved; peers and the scheduler are fakes.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "sync_hub.h"

using nlohmann::json;
using lectern::sync::Hub;
using lectern::sync::Peer;

namespace {

struct FakePeer : Peer
{
    std::vector<json> frames;
    bool closed = false;

    void fSend(const std::string &text) override
    {
        frames.push_back(json::parse(text));
    }
    void fClose() override
    {
        closed = true;
    }

    /// The last frame of this type, or null.
    json fLast(const std::string &type) const
    {
        for (auto it = frames.rbegin(); it != frames.rend(); ++it)
        {
            if ((*it)["type"] == type)
            {
                return *it;
            }
        }
        return nullptr;
    }
    size_t fCount(const std::string &type) const
    {
        size_t n = 0;
        for (const auto &f : frames)
        {
            n += f["type"] == type ? 1 : 0;
        }
        return n;
    }
};

struct Harness
{
    std::vector<std::function<void()>> timers;
    Hub hub{[this](double, std::function<void()> task) { timers.push_back(std::move(task)); },
            [](const std::string &user, const json &request)
                -> std::optional<std::pair<std::string, std::string>> {
                // "owner1/notes.mdp" is shareable by anyone signed in; anything else is refused.
                if (!user.empty() && request.value("share", "") == "good")
                {
                    return std::pair<std::string, std::string>{"owner1", request.value("path", "")};
                }
                if (request.value("link", "") == "tok")
                {
                    return std::pair<std::string, std::string>{"owner1", request.value("path", "")};
                }
                return std::nullopt;
            }};

    void fFireTimers()
    {
        auto due = std::move(timers);
        timers.clear();
        for (auto &t : due)
        {
            t();
        }
    }

    std::shared_ptr<FakePeer> fConnect(const std::string &user, const std::string &device,
                                       uint64_t *id_out = nullptr)
    {
        auto peer = std::make_shared<FakePeer>();
        const uint64_t id = hub.fAdd(user, peer);
        if (id_out)
        {
            *id_out = id;
        }
        if (!device.empty())
        {
            hub.fOnMessage(id, json{{"type", "hello"}, {"id", "c1"}, {"device", device}}.dump());
        }
        return peer;
    }
};

}  // namespace

TEST_CASE("a new connection is asked to catch up and acks it", "[sync]")
{
    Harness h;
    uint64_t id = 0;
    auto peer = h.fConnect("u1", "", &id);

    const json ask = peer->fLast("request_changes");
    REQUIRE(ask.is_object());
    REQUIRE(ask["id"].is_string());

    h.hub.fOnMessage(id, json{{"type", "ack"}, {"id", ask["id"]}}.dump());
    h.fFireTimers();
    REQUIRE_FALSE(peer->closed);
}

TEST_CASE("an anonymous connection is not asked to reconcile a workspace", "[sync]")
{
    Harness h;
    auto peer = h.fConnect("", "");
    REQUIRE(peer->fCount("request_changes") == 0);
}

TEST_CASE("hello, changes and ping are each acknowledged with their own id", "[sync]")
{
    Harness h;
    uint64_t id = 0;
    auto peer = h.fConnect("u1", "", &id);

    for (const char *type : {"hello", "changes", "ping"})
    {
        const std::string frame_id = std::string("x-") + type;
        h.hub.fOnMessage(id, json{{"type", type}, {"id", frame_id}, {"device", "d"}}.dump());
        const json ack = peer->frames.back();
        REQUIRE(ack["type"] == "ack");
        REQUIRE(ack["id"] == frame_id);
    }
}

TEST_CASE("a frame the peer never acknowledges drops the connection", "[sync]")
{
    Harness h;
    auto peer = h.fConnect("u1", "dev-a");
    // Never answer request_changes.
    h.fFireTimers();

    REQUIRE(peer->closed);
    REQUIRE(h.hub.fConnectionCount() == 0);
}

TEST_CASE("a change reaches the owner's other devices but not the one that wrote it", "[sync]")
{
    Harness h;
    auto writer = h.fConnect("u1", "dev-a");
    auto other = h.fConnect("u1", "dev-b");
    auto stranger = h.fConnect("u2", "dev-c");

    h.hub.fNotify("u1", {"a/b.mdp"}, "dev-a");

    REQUIRE(writer->fCount("changes") == 0);
    REQUIRE(stranger->fCount("changes") == 0);
    const json note = other->fLast("changes");
    REQUIRE(note.is_object());
    REQUIRE(note["paths"] == json::array({"a/b.mdp"}));
}

TEST_CASE("a change from a client with no device id reaches every device", "[sync]")
{
    Harness h;
    auto a = h.fConnect("u1", "dev-a");
    auto b = h.fConnect("u1", "dev-b");

    h.hub.fNotify("u1", {"x.mdp"}, "");

    REQUIRE(a->fCount("changes") == 1);
    REQUIRE(b->fCount("changes") == 1);
}

TEST_CASE("an unacknowledged change drops that connection and spares the others", "[sync]")
{
    Harness h;
    uint64_t good_id = 0, bad_id = 0;
    auto good = h.fConnect("u1", "dev-a", &good_id);
    auto bad = h.fConnect("u1", "dev-b", &bad_id);
    for (auto [id, peer] : {std::pair{good_id, good}, std::pair{bad_id, bad}})
    {
        h.hub.fOnMessage(id, json{{"type", "ack"}, {"id", peer->fLast("request_changes")["id"]}}.dump());
    }

    h.hub.fNotify("u1", {"x.mdp"}, "");
    h.hub.fOnMessage(good_id, json{{"type", "ack"}, {"id", good->fLast("changes")["id"]}}.dump());
    h.fFireTimers();

    REQUIRE_FALSE(good->closed);
    REQUIRE(bad->closed);
    REQUIRE(h.hub.fConnectionCount() == 1);
}

TEST_CASE("a watcher is told only about the file it follows", "[sync]")
{
    Harness h;
    uint64_t id = 0;
    auto guest = h.fConnect("guest", "", &id);
    h.hub.fOnMessage(id, json{{"type", "watch"}, {"id", "w1"}, {"share", "good"}, {"path", "notes.mdp"}}.dump());
    REQUIRE(guest->frames.back()["type"] == "ack");

    h.hub.fNotify("owner1", {"private.mdp"}, "");
    REQUIRE(guest->fCount("changes") == 0);

    h.hub.fNotify("owner1", {"private.mdp", "notes.mdp"}, "");
    const json note = guest->fLast("changes");
    REQUIRE(note["paths"] == json::array({"notes.mdp"}));
}

TEST_CASE("watching a folder covers what is inside it and nothing beside it", "[sync]")
{
    Harness h;
    uint64_t id = 0;
    auto guest = h.fConnect("guest", "", &id);
    h.hub.fOnMessage(id, json{{"type", "watch"}, {"id", "w"}, {"share", "good"}, {"path", "lore"}}.dump());

    h.hub.fNotify("owner1", {"lorebook.mdp"}, "");
    REQUIRE(guest->fCount("changes") == 0);
    h.hub.fNotify("owner1", {"lore/map.mdn"}, "");
    REQUIRE(guest->fCount("changes") == 1);
}

TEST_CASE("a refused watch is nacked and follows nothing", "[sync]")
{
    Harness h;
    uint64_t id = 0;
    auto guest = h.fConnect("guest", "", &id);
    h.hub.fOnMessage(id, json{{"type", "watch"}, {"id", "w"}, {"share", "bad"}, {"path", "notes.mdp"}}.dump());

    REQUIRE(guest->frames.back()["type"] == "nack");
    h.hub.fNotify("owner1", {"notes.mdp"}, "");
    REQUIRE(guest->fCount("changes") == 0);
}

TEST_CASE("an anonymous link viewer can watch through the link token", "[sync]")
{
    Harness h;
    uint64_t id = 0;
    auto viewer = h.fConnect("", "", &id);
    h.hub.fOnMessage(id, json{{"type", "watch"}, {"id", "w"}, {"link", "tok"}, {"path", "notes.mdp"}}.dump());
    h.hub.fNotify("owner1", {"notes.mdp"}, "");
    REQUIRE(viewer->fCount("changes") == 1);
}

TEST_CASE("garbage frames are ignored, not fatal", "[sync]")
{
    Harness h;
    uint64_t id = 0;
    auto peer = h.fConnect("u1", "", &id);

    for (const char *junk : {"", "not json", "[]", "42", "{\"type\":5}", "{\"type\":\"ack\",\"id\":null}",
                             "{\"type\":\"watch\",\"id\":1}", "{\"type\":\"hello\",\"device\":{}}"})
    {
        REQUIRE_NOTHROW(h.hub.fOnMessage(id, junk));
    }
    REQUIRE_NOTHROW(h.hub.fOnMessage(9999, "{\"type\":\"ping\",\"id\":\"p\"}"));
    REQUIRE_FALSE(peer->closed);
}

TEST_CASE("a large burst of changes is sent without a path list", "[sync]")
{
    Harness h;
    auto peer = h.fConnect("u1", "dev");
    std::vector<std::string> paths;
    for (int i = 0; i < 500; ++i)
    {
        paths.push_back("f" + std::to_string(i) + ".mdp");
    }

    h.hub.fNotify("u1", paths, "");
    REQUIRE(peer->fLast("changes")["paths"].empty());
}

TEST_CASE("a folder change tells the owner's devices to look at everything", "[sync]")
{
    Harness h;
    auto mine = h.fConnect("u1", "dev-a");
    uint64_t gid = 0;
    auto guest = h.fConnect("guest", "", &gid);
    h.hub.fOnMessage(gid, json{{"type", "watch"}, {"id", "w"}, {"share", "good"}, {"path", "notes.mdp"}}.dump());

    h.hub.fNotify("owner1", {}, "");
    h.hub.fNotify("u1", {}, "");

    // The owner's own device is told, with an empty list...
    REQUIRE(mine->fCount("changes") == 1);
    REQUIRE(mine->fLast("changes")["paths"].empty());
    // ...and someone following one file is not bothered.
    REQUIRE(guest->fCount("changes") == 0);
}
