#include "client.h"

namespace NHoshi {

std::string UrlEncode(const std::string& s) {
    static const char Hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += Hex[c >> 4];
            out += Hex[c & 0xF];
        }
    }
    return out;
}

TClient::TClient(std::shared_ptr<IHttp> http, std::string homeserver)
    : Http(std::move(http))
    , Homeserver(std::move(homeserver))
{
    while (!Homeserver.empty() && Homeserver.back() == '/') {
        Homeserver.pop_back();
    }
}

TJson TClient::Call(const std::string& method, const std::string& path, const TJson* body) {
    THttpRequest request{method, Homeserver + path, AccessToken_, body ? body->Dump() : std::string()};
    const THttpResponse response = Http->Send(request);
    if (response.Status == 0) {
        throw TMatrixError(0, {}, "request did not reach " + Homeserver);
    }
    TJson json;
    try {
        json = TJson::Parse(response.Body);
    } catch (const std::runtime_error& e) {
        if (response.Status >= 200 && response.Status < 300) {
            throw TMatrixError(response.Status, {}, std::string("bad response from server: ") + e.what());
        }
    }
    if (response.Status < 200 || response.Status >= 300) {
        const std::string errcode = json.StringOr("errcode");
        const std::string message = json.StringOr("error", "HTTP " + std::to_string(response.Status));
        throw TMatrixError(response.Status, errcode, message);
    }
    return json;
}

void TClient::Login(const std::string& user, const std::string& password, const std::string& deviceName) {
    const TJson body(TJson::TObject{
        {"type", "m.login.password"},
        {"identifier", TJson::TObject{{"type", "m.id.user"}, {"user", user}}},
        {"password", password},
        {"initial_device_display_name", deviceName},
    });
    AccessToken_.clear();
    const TJson response = Call("POST", "/_matrix/client/v3/login", &body);
    if (!response["access_token"].IsString() || !response["user_id"].IsString()) {
        throw TMatrixError(200, {}, "login response has no access_token or user_id");
    }
    AccessToken_ = response["access_token"].AsString();
    UserId_ = response["user_id"].AsString();
    DeviceId_ = response.StringOr("device_id");
}

void TClient::Restore(std::string userId, std::string deviceId, std::string accessToken) {
    UserId_ = std::move(userId);
    DeviceId_ = std::move(deviceId);
    AccessToken_ = std::move(accessToken);
}

void TClient::Sync(int timeoutMs) {
    std::string path = "/_matrix/client/v3/sync";
    if (!State_.NextBatch.empty()) {
        path += "?since=" + UrlEncode(State_.NextBatch) + "&timeout=" + std::to_string(timeoutMs);
    }
    State_.Apply(Call("GET", path, nullptr));
}

std::string TClient::CreatePublicRoom(const std::string& name) {
    const TJson body(TJson::TObject{{"name", name}, {"preset", "public_chat"}});
    const TJson response = Call("POST", "/_matrix/client/v3/createRoom", &body);
    if (!response["room_id"].IsString()) {
        throw TMatrixError(200, {}, "createRoom response has no room_id");
    }
    return response["room_id"].AsString();
}

void TClient::JoinRoom(const std::string& roomIdOrAlias) {
    const TJson body(TJson::TObject{});
    Call("POST", "/_matrix/client/v3/join/" + UrlEncode(roomIdOrAlias), &body);
}

std::string TClient::SendText(const std::string& roomId, const std::string& text, const std::string& txnId) {
    const TJson body(TJson::TObject{{"msgtype", "m.text"}, {"body", text}});
    const TJson response = Call("PUT",
        "/_matrix/client/v3/rooms/" + UrlEncode(roomId) + "/send/m.room.message/" + UrlEncode(txnId), &body);
    return response.StringOr("event_id");
}

} // namespace NHoshi
