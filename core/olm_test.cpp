// Olm between two Hoshi devices, and the cases that must fail. Compatibility with vodozemac is checked by
// interop/olm_interop.py (ctest -L interop).
#include "olm.h"

#include <cstdio>
#include <cstdlib>

using namespace NHoshi;
using namespace NHoshi::NOlm;

namespace {

int Failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++Failures;
    }
}

} // namespace

int main() {
    TAccount alice = TAccount::Create();
    TAccount bob = TAccount::Create();
    bob.GenerateOneTimeKeys(2);
    const auto keys = bob.UnpublishedOneTimeKeys();
    Expect(keys.size() == 2, "two one-time keys");
    Expect(keys.begin()->first == "AAAAAQ", "libolm-style key ids");
    bob.MarkKeysAsPublished();
    Expect(bob.UnpublishedOneTimeKeys().empty(), "published keys are not offered again");
    const std::string oneTimeKey = keys.begin()->second;

    TSession aliceSession = TSession::CreateOutbound(alice, bob.IdentityKey(), oneTimeKey);
    const TOlmMessage hello = aliceSession.Encrypt("привет");
    Expect(hello.Type == 0, "first message is a pre-key message");

    auto inbound = TSession::CreateInbound(bob, hello.Body);
    Expect(inbound && inbound->Plaintext == "привет", "inbound session from the pre-key message");
    if (!inbound) {
        return EXIT_FAILURE;
    }
    TSession bobSession = std::move(inbound->Session);
    Expect(bobSession.SessionId() == aliceSession.SessionId(), "both sides agree on the session id");
    Expect(bobSession.MatchesInbound(hello.Body), "pre-key message matches its session");

    // The one-time key is used up: the same pre-key message cannot open a second session
    Expect(!TSession::CreateInbound(bob, hello.Body), "one-time key used only once");

    const TOlmMessage reply = bobSession.Encrypt("и тебе");
    Expect(reply.Type == 1 && aliceSession.Decrypt(reply) == "и тебе", "reply");
    const TOlmMessage next = aliceSession.Encrypt("уже обычное");
    Expect(next.Type == 1 && bobSession.Decrypt(next) == "уже обычное", "normal messages after the reply");

    // Tampering with any byte fails, and leaves the session usable
    TOlmMessage tampered = bobSession.Encrypt("целое");
    auto bytes = *NCrypto::Base64Decode(tampered.Body);
    bytes[bytes.size() / 2] ^= 1;
    const TOlmMessage broken{tampered.Type, NCrypto::Base64Encode(bytes)};
    Expect(!aliceSession.Decrypt(broken), "tampered message rejected");
    Expect(aliceSession.Decrypt(tampered) == "целое", "the original still decrypts after a failed attempt");

    // A pre-key message of another session does not match
    TAccount carol = TAccount::Create();
    TSession carolSession = TSession::CreateOutbound(carol, bob.IdentityKey(), std::next(keys.begin())->second);
    const TOlmMessage fromCarol = carolSession.Encrypt("я Кэрол");
    Expect(!bobSession.MatchesInbound(fromCarol.Body) && !bobSession.Decrypt(fromCarol), "foreign pre-key message rejected");

    // Saved and loaded sessions and accounts keep working
    auto reloaded = TSession::Load(TJson::Parse(bobSession.Save().Dump()));
    const TOlmMessage afterReload = aliceSession.Encrypt("после сохранения");
    Expect(reloaded && reloaded->Decrypt(afterReload) == "после сохранения", "session survives save and load");
    auto reloadedBob = TAccount::Load(TJson::Parse(bob.Save().Dump()));
    Expect(reloadedBob && reloadedBob->IdentityKey() == bob.IdentityKey(), "account survives save and load");
    Expect(reloadedBob && TSession::CreateInbound(*reloadedBob, fromCarol.Body)->Plaintext == "я Кэрол",
        "a loaded account still has its unused one-time key");

    Expect(!TSession::CreateInbound(bob, "AwoA") && !aliceSession.Decrypt({1, "garbage"}), "garbage rejected");

    if (Failures == 0) {
        std::printf("olm_test: all passed\n");
    }
    return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
