#include "sync_hub.h"

#include <algorithm>

namespace lectern::sync {

namespace {

using json = nlohmann::json;

/// More than this and the list is dropped: a client treats a notification as
/// "reconcile now" anyway, and a bulk operation shouldn't build a huge frame.
constexpr size_t cMaxPathsPerFrame = 100;

std::string fStringField(const json &frame, const char *key)
{
    const auto it = frame.find(key);
    return (it != frame.end() && it->is_string()) ? it->get<std::string>()
                                                    : std::string{};
}

bool fMatchesPrefix(const std::string &path, const std::string &prefix)
{
    return path == prefix ||
           (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
            path[prefix.size()] == '/');
}

}  // namespace

Hub::Hub(Scheduler schedule, WatchResolver resolve_watch, double ack_timeout_seconds)
    : schedule_(std::move(schedule)),
      resolve_watch_(std::move(resolve_watch)),
      ack_timeout_(ack_timeout_seconds)
{
}

uint64_t Hub::fAdd(const std::string &user_id, std::shared_ptr<Peer> peer)
{
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_connection_++;
        connections_[id] = Connection{user_id, "", std::move(peer), {}, {}};
    }

    // After a server restart nothing remembers what clients were behind on, so
    // each new connection is asked to catch up. An anonymous link viewer has no
    // workspace to reconcile.
    if (!user_id.empty())
    {
        fSendTracked(id, json{{"type", "request_changes"}});
    }
    return id;
}

void Hub::fRemove(uint64_t connection)
{
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.erase(connection);
}

size_t Hub::fConnectionCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

void Hub::fSendTracked(uint64_t connection, json frame)
{
    std::shared_ptr<Peer> peer;
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = connections_.find(connection);
        if (it == connections_.end())
        {
            return;
        }
        id = "s" + std::to_string(next_frame_++);
        it->second.pending.insert(id);
        peer = it->second.peer;
    }

    frame["id"] = id;
    try
    {
        peer->fSend(frame.dump());
    }
    catch (const std::exception &)
    {
        // The connection is going away; the timeout below cleans up.
    }

    schedule_(ack_timeout_, [this, connection, id] { fExpire(connection, id); });
}

void Hub::fExpire(uint64_t connection, const std::string &id)
{
    std::shared_ptr<Peer> peer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = connections_.find(connection);
        if (it == connections_.end() || it->second.pending.count(id) == 0)
        {
            return;  // acked, or already gone
        }
        peer = it->second.peer;
        connections_.erase(it);
    }

    // No answer: the peer is down or wedged. Dropping the connection makes the
    // client reconnect and run the catch-up it may have missed.
    try
    {
        peer->fClose();
    }
    catch (const std::exception &)
    {
    }
}

void Hub::fOnMessage(uint64_t connection, const std::string &text)
{
    // This runs on the socket's thread, where an escaping exception ends the
    // whole process, so a frame that can't be understood is simply dropped.
    try
    {
        const json frame = json::parse(text, nullptr, false);
        if (frame.is_discarded() || !frame.is_object())
        {
            return;
        }
        fHandle(connection, frame);
    }
    catch (const std::exception &)
    {
    }
}

void Hub::fHandle(uint64_t connection, const json &frame)
{
    const std::string type = fStringField(frame, "type");
    const std::string id = fStringField(frame, "id");

    std::shared_ptr<Peer> peer;
    std::string user_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = connections_.find(connection);
        if (it == connections_.end())
        {
            return;
        }
        peer = it->second.peer;
        user_id = it->second.user_id;

        if (type == "ack")
        {
            it->second.pending.erase(id);
            return;
        }
        if (type == "hello")
        {
            it->second.device = fStringField(frame, "device");
        }
    }

    const auto reply = [&](const char *kind) {
        if (!id.empty())
        {
            peer->fSend(json{{"type", kind}, {"id", id}}.dump());
        }
    };

    if (type == "hello" || type == "ping" || type == "changes")
    {
        // Receiving the frame is the whole point of the acknowledgement: it is
        // what tells the client it is safe to go and push over HTTP.
        reply("ack");
        return;
    }

    if (type == "watch" || type == "unwatch")
    {
        std::optional<std::pair<std::string, std::string>> target;
        if (type == "watch" && resolve_watch_)
        {
            target = resolve_watch_(user_id, frame);
            if (!target)
            {
                reply("nack");
                return;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = connections_.find(connection);
            if (it == connections_.end())
            {
                return;
            }
            // One followed file at a time: a new watch replaces the last.
            it->second.watches.clear();
            if (target)
            {
                it->second.watches.push_back({target->first, target->second});
            }
        }
        reply("ack");
    }
}

void Hub::fNotify(const std::string &owner_id,
                  const std::vector<std::string> &paths,
                  const std::string &origin_device)
{
    // An empty list is a message too: "something structural changed (a folder),
    // look at everything". Only the owner's own devices get it; a watcher is told
    // about its one file or nothing.
    struct Target
    {
        uint64_t connection;
        json paths;
    };
    std::vector<Target> targets;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &[id, conn] : connections_)
        {
            if (!origin_device.empty() && conn.device == origin_device)
            {
                continue;
            }

            std::vector<std::string> visible;
            const bool owners_device = !conn.user_id.empty() && conn.user_id == owner_id;
            if (owners_device)
            {
                visible = paths;
            }
            else
            {
                // A watcher only learns about the path it follows, never the
                // rest of the owner's workspace.
                for (const auto &watch : conn.watches)
                {
                    if (watch.owner != owner_id)
                    {
                        continue;
                    }
                    for (const auto &path : paths)
                    {
                        if (fMatchesPrefix(path, watch.prefix) &&
                            std::find(visible.begin(), visible.end(), path) == visible.end())
                        {
                            visible.push_back(path);
                        }
                    }
                }
            }

            if (visible.empty() && !owners_device)
            {
                continue;
            }

            json list = json::array();
            if (visible.size() <= cMaxPathsPerFrame)
            {
                for (const auto &path : visible)
                {
                    list.push_back(path);
                }
            }
            targets.push_back({id, std::move(list)});
        }
    }

    for (auto &target : targets)
    {
        fSendTracked(target.connection,
                     json{{"type", "changes"}, {"paths", std::move(target.paths)}});
    }
}

}  // namespace lectern::sync
