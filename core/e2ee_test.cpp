// TMachine without a server: key sharing between machines in memory and replacing the room's Megolm session
// when it is due
#include "e2ee.h"

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

const std::string Room = "!room:x";
const std::int64_t Start = 1760000000000;

struct TPeer {
    TMachine Machine;
    TJson OneTimeKeys;

    TPeer(const std::string& user, const std::string& device)
        : Machine(user, device, NOlm::TAccount::Create())
    {
        OneTimeKeys = Machine.KeysUploadBody(1, true)["one_time_keys"];
        Machine.MarkKeysAsPublished();
    }

    TDevice Device() const {
        return {Machine.UserId(), Machine.DeviceId(), Machine.Account().IdentityKey(), Machine.Account().FingerprintKey()};
    }
};

// A /keys/claim response with each peer's one-time key
TJson Claim(const std::vector<const TPeer*>& peers) {
    TJson::TObject users;
    for (const auto* peer : peers) {
        users[peer->Machine.UserId()] = TJson(TJson::TObject{{peer->Machine.DeviceId(), peer->OneTimeKeys}});
    }
    return TJson(TJson::TObject{{"one_time_keys", TJson(std::move(users))}});
}

// Delivers the to-device messages addressed to the peer; returns whether a room key arrived
bool Deliver(const TToDevice& messages, const std::string& sender, TPeer& peer) {
    const auto user = messages.find(peer.Machine.UserId());
    if (user == messages.end()) {
        return false;
    }
    const auto device = user->second.find(peer.Machine.DeviceId());
    if (device == user->second.end()) {
        return false;
    }
    return peer.Machine.ReceiveToDevice(TJson(TJson::TObject{{"type", "m.room.encrypted"}, {"sender", sender}, {"content", device->second}}));
}

TJson Encrypt(TMachine& machine, const std::string& body) {
    const TJson content = machine.EncryptRoomEvent(Room, "m.room.message", TJson(TJson::TObject{{"msgtype", "m.text"}, {"body", body}}));
    return TJson(TJson::TObject{{"type", "m.room.encrypted"}, {"content", content}});
}

std::string BodyOf(const TMachine& machine, const TJson& event) {
    const auto payload = machine.DecryptRoomEvent(Room, event);
    return payload ? (*payload)["content"].StringOr("body") : "";
}

} // namespace

int main() {
    TPeer alice("@alice:x", "A");
    TPeer bob("@bob:x", "B");
    TPeer carol("@carol:x", "C");
    const TRotation rotation;

    Expect(alice.Machine.PrepareRoomKey(Room, {bob.Device(), carol.Device()}, rotation, Start), "the first message starts a session");
    const TJson claim = alice.Machine.ClaimRequestFor(Room, {bob.Device(), carol.Device()});
    Expect(claim["@bob:x"].StringOr("B") == "signed_curve25519" && claim["@carol:x"].StringOr("C") == "signed_curve25519",
        "one-time keys are claimed for both devices");
    TToDevice keys = alice.Machine.ShareRoomKey(Room, {bob.Device(), carol.Device()}, Claim({&bob, &carol}));
    Expect(Deliver(keys, "@alice:x", bob) && Deliver(keys, "@alice:x", carol), "both get the room key");
    const TJson first = Encrypt(alice.Machine, "первое");
    Expect(BodyOf(bob.Machine, first) == "первое" && BodyOf(carol.Machine, first) == "первое", "both read the first message");

    Expect(!alice.Machine.PrepareRoomKey(Room, {bob.Device(), carol.Device()}, rotation, Start + 1000), "nothing is due yet");
    const TJson second = Encrypt(alice.Machine, "второе");
    Expect(second["content"].StringOr("session_id") == first["content"].StringOr("session_id"), "the same session goes on");

    // Carol leaves: what comes next must not be readable with the key she has
    Expect(alice.Machine.PrepareRoomKey(Room, {bob.Device()}, rotation, Start + 2000), "a member who left gets a new session");
    Expect(alice.Machine.ClaimRequestFor(Room, {bob.Device()}).AsObject().empty(), "the Olm session with Bob is reused");
    keys = alice.Machine.ShareRoomKey(Room, {bob.Device()}, TJson(TJson::TObject{}));
    Expect(keys.size() == 1 && Deliver(keys, "@alice:x", bob), "only Bob gets the new key");
    const TJson third = Encrypt(alice.Machine, "третье");
    Expect(third["content"].StringOr("session_id") != first["content"].StringOr("session_id"), "a new session id");
    Expect(BodyOf(bob.Machine, third) == "третье", "Bob reads the new session");
    Expect(BodyOf(carol.Machine, third).empty(), "Carol cannot");
    Expect(BodyOf(alice.Machine, first) == "первое", "the sender still reads her messages of the old session");

    // A new device of a member does not need a new session: it just gets the current key
    TPeer bobPhone("@bob:x", "B2");
    Expect(!alice.Machine.PrepareRoomKey(Room, {bob.Device(), bobPhone.Device()}, rotation, Start + 3000), "an added device keeps the session");

    // After the message limit
    const TRotation small{rotation.PeriodMs, 3};
    Expect(!alice.Machine.PrepareRoomKey(Room, {bob.Device()}, small, Start + 4000), "one message of three");
    Encrypt(alice.Machine, "4");
    Encrypt(alice.Machine, "5");
    Expect(alice.Machine.PrepareRoomKey(Room, {bob.Device()}, small, Start + 5000), "three messages make it due");

    // Verified devices: only with the key that was verified
    alice.Machine.MarkVerified(bob.Device());
    TDevice replaced = bob.Device();
    replaced.Ed25519 = carol.Device().Ed25519;
    Expect(alice.Machine.IsVerified(bob.Device()) && !alice.Machine.IsVerified(replaced) && !alice.Machine.IsVerified(carol.Device()),
        "a device is verified with its key only");

    // After the period, also across a save and load
    const std::int64_t created = Start + 5000;
    const auto loaded = TMachine::Load(alice.Machine.Save());
    Expect(loaded.has_value(), "machine loads");
    if (loaded) {
        TMachine machine = *loaded;
        Expect(machine.IsVerified(bob.Device()) && !machine.IsVerified(carol.Device()), "verified devices survive a reload");
        Expect(!machine.PrepareRoomKey(Room, {}, rotation, created + rotation.PeriodMs - 1), "a week is not over");
        Expect(machine.PrepareRoomKey(Room, {}, rotation, created + rotation.PeriodMs), "a week is over");
    }

    if (Failures == 0) {
        std::printf("e2ee_test: all passed\n");
    }
    return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
