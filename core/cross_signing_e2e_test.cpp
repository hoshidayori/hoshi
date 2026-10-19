// Cross-signing on the local test Synapse: Alice uploads a master, self-signing and user-signing key, signs her
// new device with the self-signing key, and both she and Bob see the chain through /keys/query in the form the
// server returns it.
// Run from synapse/data like e2e_test.
#include "client.h"
#include "e2ee.h"
#include "unix_http.h"

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

std::string Public(const NCrypto::TEd25519KeyPair& key) {
    return NCrypto::Base64Encode(NCrypto::ToBytes(key.Public));
}

TJson KeyObject(const std::string& userId, const NCrypto::TEd25519KeyPair& key, const char* usage) {
    return TJson(TJson::TObject{
        {"user_id", userId},
        {"usage", TJson::TArray{usage}},
        {"keys", TJson::TObject{{"ed25519:" + Public(key), Public(key)}}},
    });
}

const TDevice* Find(const std::vector<TDevice>& devices, const std::string& deviceId) {
    for (const auto& device : devices) {
        if (device.DeviceId == deviceId) {
            return &device;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    auto http = std::make_shared<TUnixHttp>("hs.sock");
    TClient alice(http, "http://localhost");
    TClient bob(http, "http://localhost");
    try {
        alice.Login("alice", "alice-test-pw", "hoshi cross-signing test");
        bob.Login("bob", "bob-test-pw", "hoshi cross-signing test");
        TMachine machine(alice.UserId(), alice.DeviceId(), NOlm::TAccount::Create());
        const TJson upload = machine.KeysUploadBody(1, true);
        alice.UploadKeys(upload);
        machine.MarkKeysAsPublished();

        // New keys each run: a second upload replaces the old ones, after the password
        const auto master = NCrypto::TEd25519KeyPair::Generate();
        const auto selfSigning = NCrypto::TEd25519KeyPair::Generate();
        const auto userSigning = NCrypto::TEd25519KeyPair::Generate();
        alice.UploadCrossSigningKeys(TJson(TJson::TObject{
            {"master_key", KeyObject(alice.UserId(), master, "master")},
            {"self_signing_key", SignJsonWithKey(KeyObject(alice.UserId(), selfSigning, "self_signing"), alice.UserId(), master)},
            {"user_signing_key", SignJsonWithKey(KeyObject(alice.UserId(), userSigning, "user_signing"), alice.UserId(), master)},
        }), "alice-test-pw");

        // Bob sees Alice's master and self-signing keys, but not her user-signing key, and the device is not yet
        // cross-signed
        const auto before = DevicesFromQuery(bob.QueryKeys({alice.UserId()}));
        const TDevice* unsignedDevice = Find(before, alice.DeviceId());
        Expect(unsignedDevice && unsignedDevice->CrossSignedBy.empty(), "the device is not cross-signed before the signature");

        // The device signed with the self-signing key
        const TJson signedDevice = SignJsonWithKey(upload["device_keys"], alice.UserId(), selfSigning);
        const TJson failures = alice.UploadSignatures(TJson(TJson::TObject{
            {alice.UserId(), TJson::TObject{{alice.DeviceId(), signedDevice}}},
        }))["failures"];
        Expect(!failures.IsObject() || failures.AsObject().empty(), "the server took the signature");

        const TJson bobsView = bob.QueryKeys({alice.UserId()});
        const auto identities = CrossSigningFromQuery(bobsView);
        Expect(identities.count(alice.UserId()) == 1, "Bob sees Alice's cross-signing keys");
        if (identities.count(alice.UserId())) {
            const auto& keys = identities.at(alice.UserId());
            Expect(keys.Master == Public(master), "the master key Bob sees");
            Expect(keys.SelfSigning == Public(selfSigning), "the self-signing key Bob sees, signed by the master key");
            Expect(keys.UserSigning.empty(), "Bob does not get Alice's user-signing key");
        }
        const auto bobsDevices = DevicesFromQuery(bobsView);
        const TDevice* device = Find(bobsDevices, alice.DeviceId());
        Expect(device && device->CrossSignedBy == Public(master), "Bob sees the device cross-signed under Alice's master key");

        TMachine bobMachine(bob.UserId(), bob.DeviceId(), NOlm::TAccount::Create());
        Expect(device && !bobMachine.IsTrusted(*device), "not trusted before Bob verifies Alice's master key");
        bobMachine.TrustMasterKey(alice.UserId(), Public(master));
        Expect(device && bobMachine.IsTrusted(*device), "trusted after");

        const auto ownView = CrossSigningFromQuery(alice.QueryKeys({alice.UserId()}));
        Expect(ownView.count(alice.UserId()) && ownView.at(alice.UserId()).UserSigning == Public(userSigning),
            "Alice sees her own user-signing key");
    } catch (const TMatrixError& e) {
        std::printf("FAIL: %s (status %d, %s)\n", e.what(), e.Status, e.ErrCode.c_str());
        return EXIT_FAILURE;
    }
    if (Failures) {
        std::printf("%d failures\n", Failures);
        return EXIT_FAILURE;
    }
    std::printf("cross_signing_e2e_test: ok\n");
    return EXIT_SUCCESS;
}
