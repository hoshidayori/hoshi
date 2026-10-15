// End-to-end encryption between two Hoshi devices on the local test Synapse: device keys, key query and
// claim, an Olm-encrypted m.room_key over /sendToDevice, and a Megolm-encrypted room message.
// Run from synapse/data like e2e_test.
#include "client.h"
#include "e2ee.h"
#include "unix_http.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace NHoshi;
using namespace NHoshi::NE2ee;

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
        alice.Login("alice", "alice-test-pw", "hoshi e2ee test");
        bob.Login("bob", "bob-test-pw", "hoshi e2ee test");
        TMachine aliceMachine(alice.UserId(), alice.DeviceId(), NOlm::TAccount::Create());
        TMachine bobMachine(bob.UserId(), bob.DeviceId(), NOlm::TAccount::Create());

        // Device keys and one-time keys
        for (auto* pair : {&aliceMachine, &bobMachine}) {
            TClient& client = pair == &aliceMachine ? alice : bob;
            const TJson uploaded = client.UploadKeys(pair->KeysUploadBody(5, true));
            pair->MarkKeysAsPublished();
            Expect(uploaded["one_time_key_counts"].IsObject() && uploaded["one_time_key_counts"]["signed_curve25519"].IsInt() &&
                uploaded["one_time_key_counts"]["signed_curve25519"].AsInt() == 5, "server counts five one-time keys");
        }

        // Alice finds Bob's new device, with its self-signature checked
        const auto devices = DevicesFromQuery(alice.QueryKeys({bob.UserId()}));
        const TDevice* bobDevice = nullptr;
        for (const auto& device : devices) {
            if (device.DeviceId == bob.DeviceId()) {
                bobDevice = &device;
            }
        }
        Expect(bobDevice && bobDevice->Curve25519 == bobMachine.Account().IdentityKey(), "alice sees bob's device keys");
        if (!bobDevice) {
            return EXIT_FAILURE;
        }

        // An encrypted room with bob in it
        const std::string roomId = alice.CreateEncryptedRoom(Unique("Тайна "), {bob.UserId()});
        bob.JoinRoom(roomId);
        std::string since = bob.FetchSync({}, 0).StringOr("next_batch");

        // Alice claims a one-time key of bob's device and sends the room key over Olm
        const TJson claim = alice.ClaimKeys(TJson(TJson::TObject{{bob.UserId(), TJson::TObject{{bob.DeviceId(), "signed_curve25519"}}}}));
        const TToDevice keys = aliceMachine.ShareRoomKey(roomId, {*bobDevice}, claim);
        Expect(keys.count(bob.UserId()) && keys.at(bob.UserId()).count(bob.DeviceId()), "room key encrypted for bob's device");
        TJson::TObject messages;
        for (const auto& [user, perDevice] : keys) {
            TJson::TObject devicesJson(perDevice.begin(), perDevice.end());
            messages[user] = TJson(std::move(devicesJson));
        }
        alice.SendToDevice("m.room.encrypted", TJson(std::move(messages)), Unique("txn"));

        // And the message itself, which the server only sees as ciphertext
        const std::string secret = Unique("Только для Боба ");
        const TJson encrypted = aliceMachine.EncryptRoomEvent(roomId, "m.room.message",
            TJson(TJson::TObject{{"msgtype", "m.text"}, {"body", secret}}));
        const std::string eventId = alice.SendEvent(roomId, "m.room.encrypted", encrypted, Unique("txn"));
        Expect(!eventId.empty(), "encrypted event sent");

        bool gotKey = false;
        bool gotMessage = false;
        bool serverSawPlaintext = false;
        for (int i = 0; i < 6 && !(gotKey && gotMessage); ++i) {
            const TJson sync = bob.FetchSync(since, 2000);
            since = sync.StringOr("next_batch");
            if (sync["to_device"]["events"].IsArray()) {
                for (const auto& event : sync["to_device"]["events"].AsArray()) {
                    gotKey = bobMachine.ReceiveToDevice(event) || gotKey;
                }
            }
            const TJson& timeline = sync["rooms"]["join"][roomId]["timeline"]["events"];
            if (timeline.IsArray()) {
                for (const auto& event : timeline.AsArray()) {
                    if (event.StringOr("event_id") != eventId) {
                        continue;
                    }
                    serverSawPlaintext = event["content"]["body"].IsString();
                    if (const auto plain = bobMachine.DecryptRoomEvent(roomId, event)) {
                        gotMessage = plain->StringOr("type") == "m.room.message" && plain->operator[]("content").StringOr("body") == secret;
                    }
                }
            }
        }
        Expect(gotKey, "bob decrypted the room key from the Olm to-device message");
        Expect(gotMessage, "bob decrypted alice's Megolm message");
        Expect(!serverSawPlaintext, "the event on the server has no plaintext body");

        // Alice can read her own message with the inbound copy of her session
        TJson own(TJson::TObject{{"type", "m.room.encrypted"}, {"content", encrypted}});
        const auto mine = aliceMachine.DecryptRoomEvent(roomId, own);
        Expect(mine && (*mine)["content"].StringOr("body") == secret, "alice reads her own message");
        Expect(!bobMachine.DecryptRoomEvent("!other:localhost", own), "a key is bound to its room");

        bob.LeaveRoom(roomId);
        alice.LeaveRoom(roomId);
        alice.Logout();
        bob.Logout();
    } catch (const TMatrixError& e) {
        std::printf("FAIL: matrix error %d %s: %s\n", e.Status, e.ErrCode.c_str(), e.what());
        ++Failures;
    }
    if (Failures == 0) {
        std::printf("e2ee_e2e_test: all passed\n");
    }
    return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
