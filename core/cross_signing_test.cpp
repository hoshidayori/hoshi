// Cross-signing keys from /keys/query: the chain master -> self-signing -> device, forged and malformed keys,
// and trust in a master key
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

const std::string User = "@alice:x";
const std::string DeviceId = "PHONE";

struct TKey {
    NCrypto::TEd25519KeyPair Pair = NCrypto::TEd25519KeyPair::Generate();

    std::string Public() const {
        return NCrypto::Base64Encode(NCrypto::ToBytes(Pair.Public));
    }

    // Adds a signature as Matrix does, named "ed25519:" + keyName
    TJson Sign(const TJson& object, const std::string& keyName) const {
        TJson::TObject part = object.AsObject();
        part.erase("signatures");
        part.erase("unsigned");
        const auto signature = Pair.Sign(NCrypto::ToBytes(TJson(part).Dump()));
        TJson::TObject result = object.AsObject();
        TJson::TObject signatures = object["signatures"].IsObject() ? object["signatures"].AsObject() : TJson::TObject{};
        TJson::TObject mine = signatures[User].IsObject() ? signatures[User].AsObject() : TJson::TObject{};
        mine["ed25519:" + keyName] = NCrypto::Base64Encode(NCrypto::ToBytes(signature));
        signatures[User] = TJson(std::move(mine));
        result["signatures"] = TJson(std::move(signatures));
        return TJson(std::move(result));
    }
};

TJson CrossSigningKeyObject(const TKey& key, const char* usage) {
    return TJson(TJson::TObject{
        {"user_id", User},
        {"usage", TJson::TArray{usage}},
        {"keys", TJson::TObject{{"ed25519:" + key.Public(), key.Public()}}},
    });
}

struct TAccount {
    TKey Master;
    TKey SelfSigning;
    TKey UserSigning;
    TKey Device;

    TJson DeviceKeys() const {
        const TJson keys(TJson::TObject{
            {"user_id", User},
            {"device_id", DeviceId},
            {"algorithms", TJson::TArray{OlmAlgorithm, MegolmAlgorithm}},
            {"keys", TJson::TObject{
                {"curve25519:" + DeviceId, "curve"},
                {"ed25519:" + DeviceId, Device.Public()},
            }},
        });
        return Device.Sign(keys, DeviceId);
    }

    // A /keys/query response as a server returns it for an account with cross-signing
    TJson Query() const {
        const TJson master = Master.Sign(CrossSigningKeyObject(Master, "master"), DeviceId);
        const TJson selfSigning = Master.Sign(CrossSigningKeyObject(SelfSigning, "self_signing"), Master.Public());
        const TJson userSigning = Master.Sign(CrossSigningKeyObject(UserSigning, "user_signing"), Master.Public());
        return TJson(TJson::TObject{
            {"device_keys", TJson::TObject{{User, TJson::TObject{
                {DeviceId, SelfSigning.Sign(DeviceKeys(), SelfSigning.Public())},
            }}}},
            {"master_keys", TJson::TObject{{User, master}}},
            {"self_signing_keys", TJson::TObject{{User, selfSigning}}},
            {"user_signing_keys", TJson::TObject{{User, userSigning}}},
        });
    }
};

// The response with one field of the user replaced
TJson Replace(const TJson& response, const std::string& field, const TJson& value) {
    TJson::TObject result = response.AsObject();
    result[field] = TJson(TJson::TObject{{User, value}});
    return TJson(std::move(result));
}

std::string CrossSignedBy(const TJson& response) {
    const auto devices = DevicesFromQuery(response);
    return devices.size() == 1 ? devices[0].CrossSignedBy : "<no device>";
}

void TestChain() {
    const TAccount account;
    const TJson response = account.Query();
    const auto identities = CrossSigningFromQuery(response);
    Expect(identities.count(User) == 1, "the user has cross-signing keys");
    const TCrossSigning& keys = identities.at(User);
    Expect(keys.Master == account.Master.Public(), "the master key");
    Expect(keys.SelfSigning == account.SelfSigning.Public(), "the self-signing key");
    Expect(keys.UserSigning == account.UserSigning.Public(), "the user-signing key");
    Expect(CrossSignedBy(response) == account.Master.Public(), "the device is cross-signed under the master key");

    // Without cross-signing keys the device is still there, just not cross-signed
    TJson::TObject plain = response.AsObject();
    plain.erase("master_keys");
    Expect(CrossSignedBy(TJson(plain)).empty(), "no master key: not cross-signed");
}

void TestForgeries() {
    const TAccount account;
    const TJson response = account.Query();
    const TKey stranger;

    // A self-signing key the master key did not sign
    const TJson unsignedSelf = stranger.Sign(CrossSigningKeyObject(account.SelfSigning, "self_signing"), stranger.Public());
    const TJson noSelf = Replace(response, "self_signing_keys", unsignedSelf);
    Expect(CrossSigningFromQuery(noSelf).at(User).SelfSigning.empty(), "self-signing key not signed by the master key is ignored");
    Expect(CrossSignedBy(noSelf).empty(), "and devices under it are not cross-signed");

    // The right key signed by the master key, but declared for another usage
    const TJson wrongUsage = account.Master.Sign(CrossSigningKeyObject(account.SelfSigning, "user_signing"), account.Master.Public());
    Expect(CrossSignedBy(Replace(response, "self_signing_keys", wrongUsage)).empty(), "a key with the wrong usage does not cross-sign");

    // A master key object that claims another user
    TJson::TObject otherUser = CrossSigningKeyObject(account.Master, "master").AsObject();
    otherUser["user_id"] = "@mallory:x";
    Expect(CrossSigningFromQuery(Replace(response, "master_keys", TJson(otherUser))).empty(), "a master key of another user is ignored");

    // A key object with two keys is not a cross-signing key
    TJson::TObject twoKeys = CrossSigningKeyObject(account.Master, "master").AsObject();
    twoKeys["keys"] = TJson::TObject{
        {"ed25519:" + account.Master.Public(), account.Master.Public()},
        {"ed25519:" + stranger.Public(), stranger.Public()},
    };
    Expect(CrossSigningFromQuery(Replace(response, "master_keys", TJson(twoKeys))).empty(), "a key object with two keys is ignored");

    // The key id must name the key itself
    TJson::TObject misnamed = CrossSigningKeyObject(account.Master, "master").AsObject();
    misnamed["keys"] = TJson::TObject{{"ed25519:" + stranger.Public(), account.Master.Public()}};
    Expect(CrossSigningFromQuery(Replace(response, "master_keys", TJson(misnamed))).empty(), "a misnamed key is ignored");

    // A device signed by someone else's key, presented as the self-signing signature
    TJson::TObject devices = response["device_keys"].AsObject();
    const TJson forgedDevice = stranger.Sign(account.DeviceKeys(), account.SelfSigning.Public());
    devices[User] = TJson(TJson::TObject{{DeviceId, forgedDevice}});
    TJson::TObject forged = response.AsObject();
    forged["device_keys"] = TJson(devices);
    Expect(CrossSignedBy(TJson(forged)).empty(), "a forged self-signing signature does not cross-sign");

    // A server swapping in its own master key and self-signing key gets a cross-signed device, but under its own
    // master key, which our user has not verified
    TAccount swapped;
    swapped.Device = account.Device;
    Expect(CrossSignedBy(swapped.Query()) == swapped.Master.Public(), "a swapped identity cross-signs under the swapped master key");
}

void TestTrust() {
    const TAccount account;
    const auto devices = DevicesFromQuery(account.Query());
    Expect(devices.size() == 1, "one device");
    TMachine machine("@bob:x", "LAPTOP", NOlm::TAccount::Create());
    Expect(!machine.IsTrusted(devices[0]), "not trusted before anything is verified");

    TKey other;
    machine.TrustMasterKey(User, other.Public());
    Expect(!machine.IsTrusted(devices[0]), "trusting another master key does not trust the device");

    machine.TrustMasterKey(User, account.Master.Public());
    Expect(machine.IsTrusted(devices[0]), "trusted once its master key is trusted");
    Expect(!machine.IsVerified(devices[0]), "but not verified directly");

    TDevice notCrossSigned = devices[0];
    notCrossSigned.CrossSignedBy.clear();
    Expect(!machine.IsTrusted(notCrossSigned), "a device that is not cross-signed is not trusted through the master key");

    const auto loaded = TMachine::Load(machine.Save());
    Expect(loaded && loaded->IsTrusted(devices[0]), "the trusted master key is saved");
}

} // namespace

int main() {
    TestChain();
    TestForgeries();
    TestTrust();
    if (Failures) {
        std::printf("%d failures\n", Failures);
        return EXIT_FAILURE;
    }
    std::printf("cross_signing_test: ok\n");
    return EXIT_SUCCESS;
}
