#pragma once

#include "json.h"
#include "sync.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NHoshi {

struct THttpRequest {
    std::string Method;  // GET, POST, PUT
    std::string Url;
    std::string AccessToken;  // sent as a bearer token when not empty
    std::string Body;         // JSON, empty for GET
    std::string ContentType = "application/json";  // of the body
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
    // Invalidates the access token on the server and forgets it here
    void Logout();

    // One /sync round; long-polls up to timeoutMs when there is a previous batch
    void Sync(int timeoutMs);
    // Fetches older messages of a room into State() via /messages; does nothing when the history is complete
    void LoadOlder(const std::string& roomId, int limit);

    // The requests without the state, for callers that keep the state themselves (the app shares it between
    // a sync queue and a history queue). An empty since asks for the initial sync.
    TJson FetchSync(const std::string& since, int timeoutMs);
    TJson FetchMessages(const std::string& roomId, const std::string& from, int limit);

    // Creates an unencrypted room that anyone on the server can join; returns its id
    std::string CreatePublicRoom(const std::string& name);
    void JoinRoom(const std::string& roomIdOrAlias);
    void LeaveRoom(const std::string& roomId);

    // End-to-end encryption endpoints; bodies and responses as in the spec
    TJson UploadKeys(const TJson& body);                       // POST /keys/upload
    TJson QueryKeys(const std::vector<std::string>& userIds);  // POST /keys/query
    TJson ClaimKeys(const TJson& oneTimeKeys);                 // POST /keys/claim, user -> device -> algorithm
    void SendToDevice(const std::string& type, const TJson& messages, const std::string& txnId);
    // Any room event; returns the event id
    std::string SendEvent(const std::string& roomId, const std::string& type, const TJson& content, const std::string& txnId);
    // Creates a private room with m.room.encryption (Megolm) in its initial state, inviting the given users
    std::string CreateEncryptedRoom(const std::string& name, const std::vector<std::string>& invite);

    // Sends an m.text message and returns the event id. The transaction id makes a retry idempotent.
    std::string SendText(const std::string& roomId, const std::string& text, const std::string& txnId);

    // Uploads a file to the content repository; returns its mxc:// URI
    std::string UploadMedia(const std::string& bytes, const std::string& contentType);
    // A thumbnail of an mxc:// image, cropped to size x size pixels, through the authenticated media API.
    // Returns the image bytes; throws TMatrixError for a malformed URI or when the server has no thumbnail.
    std::string FetchThumbnail(const std::string& mxc, int size);
    // Sets the user's avatar in their profile, which the server copies into their member events
    void SetAvatarUrl(const std::string& mxc);

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

// The server name and media id of an mxc://server/id URI, if it has the spec's form: the media id only
// letters, digits, '_' and '-', the server name a host with an optional port. Anything else (a path, a
// query, "..") could steer the request elsewhere, so it is refused.
struct TMxc {
    std::string Server;
    std::string MediaId;
};
std::optional<TMxc> ParseMxc(const std::string& uri);

} // namespace NHoshi
