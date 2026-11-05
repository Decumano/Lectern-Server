#include "sync_ws.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/WebSocketController.h>
#include <spdlog/spdlog.h>

#include "auth.h"
#include "session.h"
#include "share.h"
#include "sync_hub.h"
#include "util.h"

namespace lectern::sync {

namespace {

class DrogonPeer : public Peer
{
  public:
    explicit DrogonPeer(drogon::WebSocketConnectionPtr connection)
        : connection_(std::move(connection))
    {
    }

    void fSend(const std::string &text) override
    {
        connection_->send(text);
    }

    void fClose() override
    {
        connection_->shutdown(drogon::CloseCode::kNormalClosure, "no acknowledgement");
    }

  private:
    drogon::WebSocketConnectionPtr connection_;
};

Hub &fHub()
{
    static Hub hub(
        [](double seconds, std::function<void()> task) {
            drogon::app().getLoop()->runAfter(seconds, std::move(task));
        },
        [](const std::string &user_id, const nlohmann::json &request) {
            return share::fResolveWatch(user_id, request);
        });
    return hub;
}

/// A browser sends an Origin; it must be this server. (The session cookie is
/// SameSite=Strict, which already keeps other sites from riding it, but a
/// socket is cheap to check.) Clients that aren't browsers send none.
bool fOriginAllowed(const drogon::HttpRequestPtr &req)
{
    const std::string origin = req->getHeader("origin");
    if (origin.empty())
    {
        return true;
    }
    const size_t scheme_end = origin.find("://");
    const std::string origin_host =
        scheme_end == std::string::npos ? origin : origin.substr(scheme_end + 3);
    return util::fToLower(origin_host) == util::fToLower(req->getHeader("host"));
}

class SyncSocket : public drogon::WebSocketController<SyncSocket, false>
{
  public:
    void handleNewConnection(const drogon::HttpRequestPtr &req,
                             const drogon::WebSocketConnectionPtr &connection) override
    {
        try
        {
            // Credentials that are present but wrong are refused outright; a
            // client with none is an anonymous link viewer.
            const bool offered = !req->getHeader("authorization").empty() ||
                                 !req->getCookie(session::cCookieName).empty();
            const auto user = auth::fOptionalUserId(req);

            if (!fOriginAllowed(req) || (offered && !user))
            {
                connection->shutdown(drogon::CloseCode::kViolation, "unauthorized");
                return;
            }

            const uint64_t id = fHub().fAdd(user.value_or(""),
                                            std::make_shared<DrogonPeer>(connection));
            connection->setContext(std::make_shared<uint64_t>(id));
        }
        catch (const std::exception &error)
        {
            spdlog::warn("sync: could not accept a connection: {}", error.what());
            connection->shutdown(drogon::CloseCode::kUnexpectedCondition, "error");
        }
    }

    void handleNewMessage(const drogon::WebSocketConnectionPtr &connection,
                          std::string &&message,
                          const drogon::WebSocketMessageType &type) override
    {
        if (type != drogon::WebSocketMessageType::Text)
        {
            return;  // pings and pongs are answered by drogon itself
        }
        if (const auto id = connection->getContext<uint64_t>())
        {
            fHub().fOnMessage(*id, message);
        }
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr &connection) override
    {
        if (const auto id = connection->getContext<uint64_t>())
        {
            fHub().fRemove(*id);
        }
    }
};

}  // namespace

void fRegisterRoutes()
{
    drogon::app().registerWebSocketController("/api/sync/ws", "lectern::sync::SyncSocket",
                                              {drogon::Get});
}

void fNotifyChanges(const std::string &owner_id,
                    const std::vector<std::string> &paths,
                    const std::string &origin_device)
{
    try
    {
        fHub().fNotify(owner_id, paths, origin_device);
    }
    catch (const std::exception &error)
    {
        spdlog::warn("sync: could not announce a change: {}", error.what());
    }
}

}  // namespace lectern::sync
