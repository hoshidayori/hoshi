#pragma once

#include "json.h"
#include "sync.h"

#include <cstdint>
#include <memory>
#include <string>

namespace NHoshi {

struct THttpRequest {
    std::string Method;  // GET, POST, PUT
    std::string Url;
    std::string AccessToken;  // sent as a bearer token when not empty
    std::string Body;         // JSON, empty for GET
};

struct THttpResponse {
    int Status = 0;  // 0 means the request did not reach the server
    std::string Body;
};

// Network access. The iOS and macOS implementation uses NSURLSession; tests use a fake.
class IHttp {
public:
    virtual ~IHttp() = default;
    virtual THttpResponse Send(const THttpRequest& request) = 0;
};

// A Matrix error: the server answered with a non-2xx status or the transport failed
class TMatrixError : public std::runtime_error {
public:
    TMatrixError(int status, std::string errcode, const std::string& message)
        : std::runtime_error(message)
        , Status(status)
        , ErrCode(std::move(errcode))
    {}

    const int Status;
    const std::string ErrCode;  // M_FORBIDDEN, M_UNKNOWN_TOKEN, ... empty for transport errors
};

// Client-server API calls the first version needs. Not thread-safe: one owner calls it.
class TClient {
public:
    TClient(std::shared_ptr<IHttp> http, std::string homeserver);

    // m.login.password; on success keeps the access token, user id and device id
    void Login(const std::string& user, const std::string& password, const std::string& deviceName);
    // Uses a token saved from an earlier login
    void Restore(std::string userId, std::string deviceId, std::string accessToken);

    // One /sync round; long-polls up to timeoutMs when there is a previous batch
    void Sync(int timeoutMs);

    // Creates an unencrypted room that anyone on the server can join; returns its id
    std::string CreatePublicRoom(const std::string& name);
    void JoinRoom(const std::string& roomIdOrAlias);
    void LeaveRoom(const std::string& roomId);

    // Sends an m.text message and returns the event id. The transaction id makes a retry idempotent.
    std::string SendText(const std::string& roomId, const std::string& text, const std::string& txnId);

    const TSyncState& State() const { return State_; }
    const std::string& UserId() const { return UserId_; }
    const std::string& DeviceId() const { return DeviceId_; }
    const std::string& AccessToken() const { return AccessToken_; }

private:
    TJson Call(const std::string& method, const std::string& path, const TJson* body);

    std::shared_ptr<IHttp> Http;
    std::string Homeserver;  // https://example.com, without a trailing slash
    std::string UserId_;
    std::string DeviceId_;
    std::string AccessToken_;
    TSyncState State_;
};

// Percent-encodes a path segment or a query value (room ids contain '!' and ':')
std::string UrlEncode(const std::string& s);

} // namespace NHoshi
