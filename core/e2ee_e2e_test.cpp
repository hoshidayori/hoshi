// End-to-end encryption between two Hoshi devices on the local test Synapse: device keys, key query and
// claim, an Olm-encrypted m.room_key over /sendToDevice, a Megolm-encrypted room message, and SAS verification
// of the two devices.
// Run from synapse/data like e2e_test.
#include "client.h"
#include "e2ee.h"
#include "unix_http.h"
#include "verification.h"

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
        TSyncState bobState;
        const TJson initial = bob.FetchSync({}, 0);
        bobState.Apply(initial);
        std::string since = initial.StringOr("next_batch");

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

        // Bob's crypto state survives a save and load, as when the app restarts
        bobMachine = *TMachine::Load(TJson::Parse(bobMachine.Save().Dump()));
        aliceMachine = *TMachine::Load(TJson::Parse(aliceMachine.Save().Dump()));

        bool gotKey = false;
        bool gotMessage = false;
        bool serverSawPlaintext = false;
        for (int i = 0; i < 6 && !(gotKey && gotMessage); ++i) {
            const TJson sync = bob.FetchSync(since, 2000);
            since = sync.StringOr("next_batch");
            bobState.Apply(sync);
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

        // The same through the timeline model: the message waits for its key, then decrypts in place
        Expect(DecryptWaiting(bobState, bobMachine), "waiting messages decrypt once the key is there");
        bool inTimeline = false;
        for (const auto& message : bobState.Rooms[roomId].Timeline) {
            inTimeline = inTimeline || (message.EventId == eventId && message.Body == secret && message.MsgType == "m.text");
        }
        Expect(inTimeline, "the decrypted message is in bob's timeline");
        Expect(bobState.Rooms[roomId].Encrypted && bobState.Rooms[roomId].Members.count(alice.UserId()) &&
            bobState.Rooms[roomId].Members.count(bob.UserId()), "room is encrypted and lists both members");

        // Alice can read her own message with the inbound copy of her session
        TJson own(TJson::TObject{{"type", "m.room.encrypted"}, {"content", encrypted}});
        const auto mine = aliceMachine.DecryptRoomEvent(roomId, own);
        Expect(mine && (*mine)["content"].StringOr("body") == secret, "alice reads her own message");
        Expect(!bobMachine.DecryptRoomEvent("!other:localhost", own), "a key is bound to its room");

        // Device verification through the server, with the to-device messages in clear as the app sends them
        {
            const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::optional<TDevice> aliceDevice;
            for (const auto& device : DevicesFromQuery(bob.QueryKeys({alice.UserId()}))) {
                if (device.DeviceId == alice.DeviceId()) {
                    aliceDevice = device;
                }
            }
            Expect(aliceDevice.has_value(), "bob sees alice's device keys");
            const NVerification::TIdentity aliceId{alice.UserId(), alice.DeviceId(), aliceMachine.Account().FingerprintKey()};
            const NVerification::TIdentity bobId{bob.UserId(), bob.DeviceId(), bobMachine.Account().FingerprintKey()};
            std::string aliceSince = alice.FetchSync({}, 0).StringOr("next_batch");

            auto send = [](TClient& from, NVerification::TVerification& verification, const TDevice& to) {
                for (const auto& message : verification.TakeOutgoing()) {
                    from.SendToDevice(message.Type, TJson(TJson::TObject{{to.UserId, TJson::TObject{{to.DeviceId, message.Content}}}}),
                        Unique("verify"));
                }
            };
            auto receive = [](TClient& client, std::string& since) {
                std::vector<TJson> events;
                const TJson sync = client.FetchSync(since, 1000);
                since = sync.StringOr("next_batch");
                if (sync["to_device"]["events"].IsArray()) {
                    for (const auto& event : sync["to_device"]["events"].AsArray()) {
                        if (event.StringOr("type").rfind("m.key.verification.", 0) == 0) {
                            events.push_back(event);
                        }
                    }
                }
                return events;
            };

            NVerification::TVerification a = NVerification::TVerification::Request(aliceId, *bobDevice, Unique("verify"), now);
            send(alice, a, *bobDevice);
            std::optional<NVerification::TVerification> b;
            for (int i = 0; i < 5 && !b; ++i) {
                for (const auto& event : receive(bob, since)) {
                    if (event.StringOr("type") == "m.key.verification.request" && aliceDevice) {
                        b = NVerification::TVerification::FromRequest(bobId, *aliceDevice, event["content"], now + 1000);
                    }
                }
            }
            Expect(b.has_value(), "bob gets the verification request");
            if (b && aliceDevice) {
                b->AcceptRequest();
                send(bob, *b, *aliceDevice);
                bool started = false;
                bool confirmed = false;
                for (int i = 0; i < 12 && !(a.State() == NVerification::EState::Done && b->State() == NVerification::EState::Done); ++i) {
                    for (const auto& event : receive(alice, aliceSince)) {
                        a.Receive(event.StringOr("type"), event["content"], event.StringOr("sender"));
                    }
                    if (a.State() == NVerification::EState::Ready && !started) {
                        a.StartSas();
                        started = true;
                    }
                    send(alice, a, *bobDevice);
                    for (const auto& event : receive(bob, since)) {
                        b->Receive(event.StringOr("type"), event["content"], event.StringOr("sender"));
                    }
                    if (a.State() == NVerification::EState::ShowingSas && b->State() == NVerification::EState::ShowingSas && !confirmed) {
                        const auto x = a.Emoji();
                        const auto y = b->Emoji();
                        bool same = true;
                        for (size_t k = 0; k < x.size(); ++k) {
                            same = same && std::string(x[k].Symbol) == y[k].Symbol;
                        }
                        Expect(same, "both devices show the same emoji");
                        a.ConfirmSas();
                        b->ConfirmSas();
                        confirmed = true;
                        send(alice, a, *bobDevice);
                    }
                    send(bob, *b, *aliceDevice);
                }
                Expect(a.State() == NVerification::EState::Done && b->State() == NVerification::EState::Done,
                    "both devices verified each other through the server");
            }
        }

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
