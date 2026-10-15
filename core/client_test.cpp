#include "client.h"

#include <cstdio>
#include <cstdlib>
#include <deque>

using namespace NHoshi;

namespace {

int Failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++Failures;
    }
}

// Returns queued responses in order and records every request
class TFakeHttp : public IHttp {
public:
    THttpResponse Send(const THttpRequest& request) override {
        Requests.push_back(request);
        if (Responses.empty()) {
            return {0, {}};
        }
        THttpResponse response = Responses.front();
        Responses.pop_front();
        return response;
    }

    std::deque<THttpResponse> Responses;
    std::vector<THttpRequest> Requests;
};

} // namespace

int main() {
    Expect(UrlEncode("!abc:example.com") == "%21abc%3Aexample.com", "room id encoding");
    Expect(UrlEncode("s72595_4483_1934") == "s72595_4483_1934", "batch token unchanged");

    auto http = std::make_shared<TFakeHttp>();
    TClient client(http, "https://example.com/");

    http->Responses.push_back({200, R"({"user_id": "@yuki:example.com", "access_token": "syt_secret", "device_id": "HOSHI1"})"});
    client.Login("yuki", "pw", "Hoshi test");
    Expect(client.UserId() == "@yuki:example.com", "user id after login");
    Expect(client.DeviceId() == "HOSHI1", "device id after login");
    Expect(client.AccessToken() == "syt_secret", "token after login");
    const THttpRequest& login = http->Requests.at(0);
    Expect(login.Method == "POST" && login.Url == "https://example.com/_matrix/client/v3/login", "login url");
    Expect(login.AccessToken.empty(), "login sent without token");
    const TJson loginBody = TJson::Parse(login.Body);
    Expect(loginBody["identifier"].StringOr("user") == "yuki", "login identifier");
    Expect(loginBody.StringOr("type") == "m.login.password", "login type");

    http->Responses.push_back({200, R"({"next_batch": "b1", "rooms": {"join": {"!r:example.com": {"timeline": {"events": [
        {"type": "m.room.message", "sender": "@a:example.com", "event_id": "$1", "content": {"msgtype": "m.text", "body": "hi"}}]}}}}})"});
    client.Sync(30000);
    Expect(http->Requests.at(1).Url == "https://example.com/_matrix/client/v3/sync", "first sync has no since");
    Expect(http->Requests.at(1).AccessToken == "syt_secret", "sync sends token");
    Expect(client.State().Rooms.at("!r:example.com").Timeline.at(0).Body == "hi", "sync applied");

    http->Responses.push_back({200, R"({"next_batch": "b2"})"});
    client.Sync(30000);
    Expect(http->Requests.at(2).Url == "https://example.com/_matrix/client/v3/sync?since=b1&timeout=30000", "next sync uses since");
    Expect(client.State().NextBatch == "b2", "batch advanced");

    http->Responses.push_back({200, R"({"event_id": "$sent"})"});
    Expect(client.SendText("!r:example.com", "привет", "txn1") == "$sent", "send returns event id");
    const THttpRequest& send = http->Requests.at(3);
    Expect(send.Method == "PUT", "send uses PUT");
    Expect(send.Url == "https://example.com/_matrix/client/v3/rooms/%21r%3Aexample.com/send/m.room.message/txn1", "send url");
    Expect(TJson::Parse(send.Body).StringOr("body") == "привет", "send body");

    http->Responses.push_back({401, R"({"errcode": "M_UNKNOWN_TOKEN", "error": "Invalid access token passed."})"});
    try {
        client.Sync(30000);
        Expect(false, "401 must throw");
    } catch (const TMatrixError& e) {
        Expect(e.Status == 401 && e.ErrCode == "M_UNKNOWN_TOKEN", "matrix error code");
    }

    http->Responses.push_back({502, "<html>Bad Gateway</html>"});
    try {
        client.Sync(30000);
        Expect(false, "502 must throw");
    } catch (const TMatrixError& e) {
        Expect(e.Status == 502 && e.ErrCode.empty(), "non-json error page");
    }

    try {
        client.Sync(30000);  // no queued response: transport failure
        Expect(false, "transport failure must throw");
    } catch (const TMatrixError& e) {
        Expect(e.Status == 0, "transport failure status");
    }

    http->Responses.push_back({200, "{}"});
    client.Logout();
    Expect(http->Requests.back().Url == "https://example.com/_matrix/client/v3/logout", "logout url");
    Expect(http->Requests.back().AccessToken == "syt_secret", "logout sends the token it invalidates");
    Expect(client.AccessToken().empty(), "token forgotten after logout");

    client.Restore("@yuki:example.com", "HOSHI1", "syt_old");
    http->Responses.push_back({401, R"({"errcode": "M_UNKNOWN_TOKEN", "error": "gone"})"});
    client.Logout();
    Expect(client.AccessToken().empty(), "logout with an already invalid token succeeds");

    if (Failures == 0) {
        std::printf("client_test: all passed\n");
    }
    return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
