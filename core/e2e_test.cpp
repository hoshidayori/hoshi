// End-to-end test against the local test Synapse (synapse/start.sh), users alice and bob from synapse/register.py.
// Run from synapse/data: the socket name hs.sock is relative because the absolute path is too long for macOS.
#include "client.h"
#include "unix_http.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace NHoshi;

namespace {

int Failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++Failures;
    }
}

std::string Unique(const char* prefix) {
    return prefix + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
}

} // namespace

int main() {
    auto http = std::make_shared<TUnixHttp>("hs.sock");
    TClient alice(http, "http://localhost");
    TClient bob(http, "http://localhost");

    try {
        alice.Login("alice", "alice-test-pw", "hoshi e2e");
        bob.Login("bob", "bob-test-pw", "hoshi e2e");
        Expect(alice.UserId() == "@alice:localhost", "alice logged in");
        Expect(bob.UserId() == "@bob:localhost", "bob logged in");

        const std::string roomId = alice.CreatePublicRoom("Огоньки");
        bob.JoinRoom(roomId);
        bob.Sync(0);  // initial sync; later ones only bring new events

        const std::string text = Unique("Привет, Боб! ");
        const std::string eventId = alice.SendText(roomId, text, Unique("txn"));
        Expect(!eventId.empty(), "send returned an event id");

        bool seen = false;
        for (int i = 0; i < 5 && !seen; ++i) {
            bob.Sync(2000);
            const auto it = bob.State().Rooms.find(roomId);
            if (it != bob.State().Rooms.end()) {
                Expect(it->second.Name == "Огоньки", "bob sees the room name");
                for (const auto& message : it->second.Timeline) {
                    if (message.EventId == eventId) {
                        seen = true;
                        Expect(message.Body == text, "bob sees alice's text");
                        Expect(message.Sender == "@alice:localhost", "sender is alice");
                    }
                }
            }
        }
        Expect(seen, "bob received the message");

        // History: more messages than an initial sync returns, read by a fresh client of bob's
        const int count = 25;
        for (int i = 0; i < count; ++i) {
            alice.SendText(roomId, "история " + std::to_string(i), Unique("txn"));
        }
        // A separate login: Synapse caches the initial /sync response per user and device for a while, so
        // reusing bob's token would return the snapshot from before these messages
        TClient late(http, "http://localhost");
        late.Login("bob", "bob-test-pw", "hoshi e2e history");
        late.Sync(0);
        const TRoom& before = late.State().Rooms.at(roomId);
        Expect(before.Limited, "initial sync timeline is limited");
        for (int i = 0; i < 20 && !late.State().Rooms.at(roomId).HistoryComplete; ++i) {
            late.LoadOlder(roomId, 10);
        }
        const TRoom& after = late.State().Rooms.at(roomId);
        Expect(after.HistoryComplete, "history reaches the start of the room");
        std::vector<std::string> history;
        for (const auto& message : after.Timeline) {
            if (message.Body.rfind("история ", 0) == 0) {
                history.push_back(message.Body);
            }
        }
        bool inOrder = static_cast<int>(history.size()) == count;
        for (int i = 0; inOrder && i < count; ++i) {
            inOrder = history[static_cast<size_t>(i)] == "история " + std::to_string(i);
        }
        Expect(inOrder, "all history messages once and in order");
        Expect(!after.Timeline.empty() && after.Timeline.front().Body == text, "the first message of the room comes first");
        late.Logout();

        // Leave, so that test rooms do not pile up in the users' room lists
        bob.LeaveRoom(roomId);
        alice.LeaveRoom(roomId);

        try {
            TClient wrong(http, "http://localhost");
            wrong.Login("alice", "wrong-password", "hoshi e2e");
            Expect(false, "wrong password must fail");
        } catch (const TMatrixError& e) {
            Expect(e.Status == 403 && e.ErrCode == "M_FORBIDDEN", "wrong password is M_FORBIDDEN");
        }
    } catch (const TMatrixError& e) {
        std::printf("FAIL: matrix error %d %s: %s\n", e.Status, e.ErrCode.c_str(), e.what());
        ++Failures;
    }

    if (Failures == 0) {
        std::printf("e2e_test: all passed\n");
    }
    return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
