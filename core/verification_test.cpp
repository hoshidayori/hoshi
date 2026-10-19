// SAS verification between two TVerification objects in memory, including a relay that swaps the keys
#include "verification.h"

#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace NHoshi;
using namespace NHoshi::NVerification;

namespace {

int Failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++Failures;
    }
}

const std::int64_t Now = 1760000000000;

struct TSide {
    NCrypto::TEd25519KeyPair Key = NCrypto::TEd25519KeyPair::Generate();
    TIdentity Identity;
    NE2ee::TDevice Device;

    TSide(const std::string& user, const std::string& device) {
        Identity = {user, device, NCrypto::Base64Encode(NCrypto::ToBytes(Key.Public))};
        Device = {user, device, "curve-" + device, Identity.Ed25519};
    }
};

// What the relay may change on the way: by default, nothing
using TTamper = std::function<void(const std::string& type, TJson& content)>;

// Moves the messages each side wants to send to the other until both are quiet
void Pump(TVerification& a, const TSide& sideA, TVerification& b, const TSide& sideB, const TTamper& tamper = {}) {
    for (bool moved = true; moved;) {
        moved = false;
        for (auto message : a.TakeOutgoing()) {
            if (tamper) {
                tamper(message.Type, message.Content);
            }
            b.Receive(message.Type, message.Content, sideA.Identity.UserId);
            moved = true;
        }
        for (auto message : b.TakeOutgoing()) {
            if (tamper) {
                tamper(message.Type, message.Content);
            }
            a.Receive(message.Type, message.Content, sideB.Identity.UserId);
            moved = true;
        }
    }
}

bool SameEmoji(const TVerification& a, const TVerification& b) {
    const auto x = a.Emoji();
    const auto y = b.Emoji();
    for (size_t i = 0; i < x.size(); ++i) {
        if (std::string(x[i].Symbol) != y[i].Symbol) {
            return false;
        }
    }
    return a.Decimals() == b.Decimals();
}

// A asks, B agrees; returns B's side of the transaction
std::optional<TVerification> Handshake(TVerification& a, const TSide& sideA, const TSide& sideB) {
    auto request = a.TakeOutgoing();
    if (request.size() != 1 || request[0].Type != "m.key.verification.request") {
        return std::nullopt;
    }
    auto b = TVerification::FromRequest(sideB.Identity, sideA.Device, request[0].Content, Now + 1000);
    if (b) {
        b->AcceptRequest();
    }
    return b;
}

void Verified(bool requesterStarts) {
    TSide alice("@alice:x", "PHONE");
    TSide bob("@bob:x", "LAPTOP");
    TVerification a = TVerification::Request(alice.Identity, bob.Device, "txn1", Now);
    auto b = Handshake(a, alice, bob);
    Expect(b && b->State() == EState::Ready, "the other side is ready");
    if (!b) {
        return;
    }
    Pump(a, alice, *b, bob);
    Expect(a.State() == EState::Ready, "the requester sees ready");
    (requesterStarts ? a : *b).StartSas();
    Pump(a, alice, *b, bob);
    Expect(a.State() == EState::ShowingSas && b->State() == EState::ShowingSas, "both show the emoji");
    Expect(SameEmoji(a, *b), "the same emoji and numbers on both screens");

    a.ConfirmSas();
    Pump(a, alice, *b, bob);
    Expect(a.State() == EState::WaitingForMac && b->State() == EState::ShowingSas, "one confirmed, the other still compares");
    b->ConfirmSas();
    Pump(a, alice, *b, bob);
    Expect(a.State() == EState::Done && b->State() == EState::Done, "both verified");
    Expect(a.VerifiedMasterKey().empty() && b->VerifiedMasterKey().empty(), "no master keys without cross-signing");
}

// Both users have cross-signing master keys: each MAC covers its master key. ourMaster is what Bob's side puts
// in its MAC, theirMaster what Alice's side expects; tamper may change the MACs on the way.
struct TMasterResult {
    EState Alice;
    std::string AliceVerifiedMaster;
    std::string AliceCancelReason;
};
TMasterResult VerifyWithMasterKeys(const std::string& bobSends, const std::string& aliceExpects, const TTamper& tamper = {}) {
    TSide alice("@alice:x", "PHONE");
    TSide bob("@bob:x", "LAPTOP");
    TVerification a = TVerification::Request(alice.Identity, bob.Device, "master", Now);
    auto b = Handshake(a, alice, bob);
    a.SetMasterKeys("alice-master", aliceExpects);
    b->SetMasterKeys(bobSends, "alice-master");
    Pump(a, alice, *b, bob);
    a.StartSas();
    Pump(a, alice, *b, bob);
    b->ConfirmSas();
    Pump(a, alice, *b, bob, tamper);
    a.ConfirmSas();
    Pump(a, alice, *b, bob, tamper);
    Expect(tamper || b->VerifiedMasterKey() == "alice-master", "Bob verified Alice's master key");
    return {a.State(), a.VerifiedMasterKey(), a.CancelReason()};
}

void MasterKeys() {
    const auto both = VerifyWithMasterKeys("bob-master", "bob-master");
    Expect(both.Alice == EState::Done && both.AliceVerifiedMaster == "bob-master", "Alice verified Bob's master key");

    // Bob's MAC covers a master key Alice does not have for him: it proves nothing, the device is still verified
    const auto unknown = VerifyWithMasterKeys("other-master", "bob-master");
    Expect(unknown.Alice == EState::Done && unknown.AliceVerifiedMaster.empty(), "an unknown master key is not verified");

    // Someone changes the MAC of the master key on the way
    const auto tampered = VerifyWithMasterKeys("bob-master", "bob-master", [](const std::string& type, TJson& content) {
        if (type != "m.key.verification.mac" || !content["mac"]["ed25519:bob-master"].IsString()) {
            return;
        }
        TJson::TObject macs = content["mac"].AsObject();
        macs["ed25519:bob-master"] = macs.at("ed25519:LAPTOP");
        TJson::TObject changed = content.AsObject();
        changed["mac"] = TJson(std::move(macs));
        content = TJson(std::move(changed));
    });
    Expect(tampered.Alice == EState::Cancelled && tampered.AliceCancelReason == "The master key does not match",
        "a wrong MAC of the master key cancels");

    // Someone drops the master key from the MAC: the key list no longer matches
    const auto dropped = VerifyWithMasterKeys("bob-master", "bob-master", [](const std::string& type, TJson& content) {
        if (type != "m.key.verification.mac" || !content["mac"]["ed25519:bob-master"].IsString()) {
            return;
        }
        TJson::TObject macs = content["mac"].AsObject();
        macs.erase("ed25519:bob-master");
        TJson::TObject changed = content.AsObject();
        changed["mac"] = TJson(std::move(macs));
        content = TJson(std::move(changed));
    });
    Expect(dropped.Alice == EState::Cancelled && dropped.AliceVerifiedMaster.empty(), "a dropped master key cancels");
}

} // namespace

int main() {
    Verified(true);
    Verified(false);
    MasterKeys();

    TSide alice("@alice:x", "PHONE");
    TSide bob("@bob:x", "LAPTOP");

    // Someone in the middle answers each side with its own key, also fixing up the commitment: then the two
    // sides get different emoji, and the users see it
    {
        TVerification a = TVerification::Request(alice.Identity, bob.Device, "mitm", Now);
        auto b = Handshake(a, alice, bob);
        Pump(a, alice, *b, bob);
        a.StartSas();
        NSas::TSas towardAlice;
        NSas::TSas towardBob;
        TJson start;
        int keys = 0;
        Pump(a, alice, *b, bob, [&](const std::string& type, TJson& content) {
            TJson::TObject changed = content.AsObject();
            if (type == "m.key.verification.start") {
                start = content;
            } else if (type == "m.key.verification.accept") {
                changed["commitment"] = NCrypto::Base64Encode(NCrypto::ToBytes(NCrypto::Sha256(
                    NCrypto::ToBytes(towardAlice.PublicKey() + start.Dump()))));
            } else if (type == "m.key.verification.key") {
                const bool fromAlice = keys++ == 0;  // the starter sends its key first
                (fromAlice ? towardAlice : towardBob).SetTheirKey(content.StringOr("key"));
                changed["key"] = fromAlice ? towardBob.PublicKey() : towardAlice.PublicKey();
            }
            content = TJson(std::move(changed));
        });
        Expect(a.State() == EState::ShowingSas && b->State() == EState::ShowingSas, "the relay got both to the emoji");
        Expect(!SameEmoji(a, *b), "but the emoji differ");
        a.Cancel("m.mismatched_sas", "The emoji did not match");
        Pump(a, alice, *b, bob);
        Expect(a.State() == EState::Cancelled && b->State() == EState::Cancelled && b->CancelCode() == "m.mismatched_sas",
            "a mismatch cancels both");
    }

    // Swapping only the key, without the commitment, is caught before any emoji are shown
    {
        TVerification a = TVerification::Request(alice.Identity, bob.Device, "swap", Now);
        auto b = Handshake(a, alice, bob);
        Pump(a, alice, *b, bob);
        a.StartSas();
        NSas::TSas mallory;
        Pump(a, alice, *b, bob, [&](const std::string& type, TJson& content) {
            if (type == "m.key.verification.key") {
                TJson::TObject changed = content.AsObject();
                changed["key"] = mallory.PublicKey();
                content = TJson(std::move(changed));
            }
        });
        Expect(a.State() == EState::Cancelled && a.CancelCode() == "m.mismatched_commitment", "a swapped key breaks the commitment");
    }

    // The server gave Bob a wrong key for Alice's device: the emoji match, but Alice's MAC shows the key is not hers
    {
        TSide impostor("@alice:x", "PHONE");
        TVerification a = TVerification::Request(alice.Identity, bob.Device, "wrongkey", Now);
        auto request = a.TakeOutgoing();
        auto b = TVerification::FromRequest(bob.Identity, impostor.Device, request[0].Content, Now);
        b->AcceptRequest();
        Pump(a, alice, *b, bob);
        a.StartSas();
        Pump(a, alice, *b, bob);
        Expect(SameEmoji(a, *b), "the channel itself is fine");
        a.ConfirmSas();
        b->ConfirmSas();
        Pump(a, alice, *b, bob);
        Expect(b->State() == EState::Cancelled && b->CancelCode() == "m.key_mismatch", "Bob refuses the key he was given");
        Expect(a.State() == EState::Cancelled, "and Alice hears about it");
    }

    // Both press start at once: the start of the lower user id goes on, and the transaction still completes
    {
        TVerification a = TVerification::Request(alice.Identity, bob.Device, "both", Now);
        auto b = Handshake(a, alice, bob);
        Pump(a, alice, *b, bob);
        a.StartSas();
        b->StartSas();
        Pump(a, alice, *b, bob);
        Expect(a.State() == EState::ShowingSas && b->State() == EState::ShowingSas && SameEmoji(a, *b), "one start wins");
    }

    // Requests from too long ago, or from another device than the one we hold keys for, are dropped
    {
        TVerification a = TVerification::Request(alice.Identity, bob.Device, "old", Now);
        const TJson request = a.TakeOutgoing()[0].Content;
        Expect(!TVerification::FromRequest(bob.Identity, alice.Device, request, Now + 11 * 60 * 1000), "a stale request");
        Expect(!TVerification::FromRequest(bob.Identity, bob.Device, request, Now), "a request from another device");
    }

    // Messages of other transactions do not touch this one
    {
        TVerification a = TVerification::Request(alice.Identity, bob.Device, "mine", Now);
        a.TakeOutgoing();
        a.Receive("m.key.verification.cancel", TJson(TJson::TObject{{"transaction_id", "other"}, {"code", "m.user"}}), "@bob:x");
        a.Receive("m.key.verification.cancel", TJson(TJson::TObject{{"transaction_id", "mine"}, {"code", "m.user"}}), "@eve:x");
        Expect(a.State() == EState::WaitingForReady, "only the other device's messages of this transaction count");
    }

    if (Failures == 0) {
        std::printf("verification_test: all passed\n");
    }
    return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
