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

void TClient::Logout() {
    const TJson body(TJson::TObject{});
    try {
        Call("POST", "/_matrix/client/v3/logout", &body);
    } catch (const TMatrixError& e) {
        // An already invalid token is as good as logged out; other errors still forget the token locally
        if (e.ErrCode != "M_UNKNOWN_TOKEN") {
            AccessToken_.clear();
            throw;
        }
    }
    AccessToken_.clear();
}

TJson TClient::FetchSync(const std::string& since, int timeoutMs) {
    std::string path = "/_matrix/client/v3/sync";
    if (!since.empty()) {
        path += "?since=" + UrlEncode(since) + "&timeout=" + std::to_string(timeoutMs);
    }
    return Call("GET", path, nullptr);
}

void TClient::Sync(int timeoutMs) {
    State_.Apply(FetchSync(State_.NextBatch, timeoutMs));
}

TJson TClient::FetchMessages(const std::string& roomId, const std::string& from, int limit) {
    return Call("GET", "/_matrix/client/v3/rooms/" + UrlEncode(roomId) + "/messages?dir=b&from=" + UrlEncode(from) +
        "&limit=" + std::to_string(limit), nullptr);
}

void TClient::LoadOlder(const std::string& roomId, int limit) {
    const auto it = State_.Rooms.find(roomId);
    if (it == State_.Rooms.end() || it->second.HistoryComplete || it->second.PrevBatch.empty()) {
        return;
    }
    State_.PrependHistory(roomId, FetchMessages(roomId, it->second.PrevBatch, limit));
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

void TClient::LeaveRoom(const std::string& roomId) {
    const TJson body(TJson::TObject{});
    Call("POST", "/_matrix/client/v3/rooms/" + UrlEncode(roomId) + "/leave", &body);
}

TJson TClient::UploadKeys(const TJson& body) {
    return Call("POST", "/_matrix/client/v3/keys/upload", &body);
}

TJson TClient::QueryKeys(const std::vector<std::string>& userIds) {
    TJson::TObject users;
    for (const auto& id : userIds) {
        users[id] = TJson(TJson::TArray{});  // an empty list asks for all devices
    }
    const TJson body(TJson::TObject{{"device_keys", TJson(std::move(users))}});
    return Call("POST", "/_matrix/client/v3/keys/query", &body);
}

TJson TClient::ClaimKeys(const TJson& oneTimeKeys) {
    const TJson body(TJson::TObject{{"one_time_keys", oneTimeKeys}});
    return Call("POST", "/_matrix/client/v3/keys/claim", &body);
}

void TClient::SendToDevice(const std::string& type, const TJson& messages, const std::string& txnId) {
    const TJson body(TJson::TObject{{"messages", messages}});
    Call("PUT", "/_matrix/client/v3/sendToDevice/" + UrlEncode(type) + "/" + UrlEncode(txnId), &body);
}

std::string TClient::SendEvent(const std::string& roomId, const std::string& type, const TJson& content, const std::string& txnId) {
    const TJson response = Call("PUT",
        "/_matrix/client/v3/rooms/" + UrlEncode(roomId) + "/send/" + UrlEncode(type) + "/" + UrlEncode(txnId), &content);
    return response.StringOr("event_id");
}

std::string TClient::CreateEncryptedRoom(const std::string& name, const std::vector<std::string>& invite) {
    TJson::TArray invitees(invite.begin(), invite.end());
    const TJson body(TJson::TObject{
        {"name", name},
        {"preset", "private_chat"},
        {"invite", TJson(std::move(invitees))},
        {"initial_state", TJson::TArray{TJson::TObject{
            {"type", "m.room.encryption"},
            {"state_key", ""},
            {"content", TJson::TObject{{"algorithm", "m.megolm.v1.aes-sha2"}}},
        }}},
    });
    const TJson response = Call("POST", "/_matrix/client/v3/createRoom", &body);
    if (!response["room_id"].IsString()) {
        throw TMatrixError(200, {}, "createRoom response has no room_id");
    }
    return response["room_id"].AsString();
}

std::string TClient::SendText(const std::string& roomId, const std::string& text, const std::string& txnId) {
    const TJson body(TJson::TObject{{"msgtype", "m.text"}, {"body", text}});
    const TJson response = Call("PUT",
        "/_matrix/client/v3/rooms/" + UrlEncode(roomId) + "/send/m.room.message/" + UrlEncode(txnId), &body);
    return response.StringOr("event_id");
}

} // namespace NHoshi
