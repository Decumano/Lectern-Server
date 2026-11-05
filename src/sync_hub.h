// The change-notification hub behind /api/sync/ws.
//
// File contents never travel over the WebSocket: every client keeps reading
// and writing through the ordinary HTTP endpoints, where the sizes, quotas,
// tombstones and merge logic already live. The socket only announces that
// something changed and carries the acknowledgement that proves the other end
// is alive, so a sender can tell "nobody answered" from "delivered".
//
// Frames are JSON objects with a "type" and an "id" chosen by the sender:
//
//   client -> server  hello           {device}          then the server acks
//   client -> server  changes         {paths}           local save; server acks
//   client -> server  ping                              heartbeat; server acks
//   client -> server  watch/unwatch   {share|link,path} follow one shared file
//   server -> client  request_changes                   sent on every new
//                                                       connection, so a client
//                                                       catches up after the
//                                                       server (re)started
//   server -> client  changes         {paths}           a workspace changed
//   either way        ack             {id}              answers the frame `id`
//
// A frame the hub sends that is not acked within the timeout means the peer is
// gone or wedged: the connection is dropped, and the client's reconnect runs
// the catch-up it missed. Nothing here is persisted; a restart is covered by
// request_changes.
//
// This file deliberately knows nothing about drogon, so the logic is testable
// without a server; sync_ws.cpp is the thin glue.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace lectern::sync {

/// One end of a WebSocket, as far as the hub is concerned.
class Peer
{
  public:
    virtual ~Peer() = default;
    virtual void fSend(const std::string &text) = 0;
    virtual void fClose() = 0;
};

/// Runs `task` once after `seconds`. Drogon's event loop in production, a
/// manual queue in tests.
using Scheduler = std::function<void(double seconds, std::function<void()> task)>;

/// Maps a `watch` request to the owner workspace and path prefix it refers to,
/// or nullopt when this connection may not follow it.
using WatchResolver = std::function<std::optional<std::pair<std::string, std::string>>(
    const std::string &user_id, const nlohmann::json &request)>;

class Hub
{
  public:
    Hub(Scheduler schedule, WatchResolver resolve_watch,
        double ack_timeout_seconds = 10.0);

    /// Registers a connection. `user_id` is empty for an anonymous one (a share
    /// link viewer), which only ever receives changes it has `watch`ed.
    uint64_t fAdd(const std::string &user_id, std::shared_ptr<Peer> peer);
    void fRemove(uint64_t connection);

    void fOnMessage(uint64_t connection, const std::string &text);

    /// Tells everyone concerned that `paths` changed in `owner_id`'s workspace:
    /// the owner's connections, and any connection watching a matching path.
    /// An empty `paths` means a folder changed: the owner's devices are told to
    /// look at everything, and nobody watching a single file hears anything.
    /// `origin_device` (from the request's X-Lectern-Device header) is skipped,
    /// so a client isn't told about its own write.
    void fNotify(const std::string &owner_id,
                 const std::vector<std::string> &paths,
                 const std::string &origin_device);

    size_t fConnectionCount() const;

  private:
    struct Watch
    {
        std::string owner;
        std::string prefix;
    };

    struct Connection
    {
        std::string user_id;
        std::string device;
        std::shared_ptr<Peer> peer;
        std::set<std::string> pending;
        std::vector<Watch> watches;
    };

    /// Sends `frame` with a fresh id and arms the ack timeout.
    void fSendTracked(uint64_t connection, nlohmann::json frame);
    void fExpire(uint64_t connection, const std::string &id);
    void fHandle(uint64_t connection, const nlohmann::json &frame);

    Scheduler schedule_;
    WatchResolver resolve_watch_;
    double ack_timeout_;

    mutable std::mutex mutex_;
    std::map<uint64_t, Connection> connections_;
    uint64_t next_connection_ = 1;
    uint64_t next_frame_ = 1;
};

}  // namespace lectern::sync
