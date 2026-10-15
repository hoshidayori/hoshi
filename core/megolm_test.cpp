// Megolm: decrypts what vodozemac (the library behind Element) produced, and checks the ratchet arithmetic.
// The opposite direction, vodozemac reading Hoshi's output, is interop/megolm_vectors.py check.
#include "megolm.h"
#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace NHoshi;
using namespace NHoshi::NMegolm;

namespace {

int Failures = 0;

void Expect(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++Failures;
    }
}

TJson ReadJson(const std::string& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return TJson::Parse(buffer.str());
}

TRatchet SampleRatchet() {
    TRatchet ratchet;
    for (size_t i = 0; i < ratchet.Data.size(); ++i) {
        ratchet.Data[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    return ratchet;
}

void TestRatchetArithmetic() {
    // AdvanceTo must agree with stepping one by one, across the 2^8 and 2^16 boundaries
    for (std::uint32_t target : {1u, 255u, 256u, 257u, 300u, 65535u, 65536u, 65537u, 70000u}) {
        TRatchet stepped = SampleRatchet();
        while (stepped.Index < target) {
            stepped.Advance();
        }
        TRatchet jumped = SampleRatchet();
        jumped.AdvanceTo(target);
        Expect(jumped.Index == target && jumped.Data == stepped.Data, "AdvanceTo " + std::to_string(target));
    }
    // Jumping in two hops gives the same as one hop
    TRatchet oneHop = SampleRatchet();
    oneHop.AdvanceTo(0x01020304);
    TRatchet twoHops = SampleRatchet();
    twoHops.AdvanceTo(0x00FFFFFF);
    twoHops.AdvanceTo(0x01020304);
    Expect(oneHop.Data == twoHops.Data, "AdvanceTo across 2^24 in two hops");
}

void TestVodozemacVectors(const std::string& path) {
    const TJson vectors = ReadJson(path);
    const auto inbound = TInboundSession::FromSessionKey(vectors.StringOr("session_key"));
    Expect(inbound.has_value(), "vodozemac session key accepted");
    if (!inbound) {
        return;
    }
    Expect(inbound->SessionId() == vectors.StringOr("session_id"), "session id matches vodozemac");
    for (const auto& m : vectors["messages"].AsArray()) {
        const auto decrypted = inbound->Decrypt(m.StringOr("ciphertext"));
        const std::string at = "vodozemac message " + std::to_string(m["index"].AsInt());
        Expect(decrypted && decrypted->Plaintext == m.StringOr("plaintext"), at + " plaintext");
        Expect(decrypted && decrypted->MessageIndex == static_cast<std::uint32_t>(m["index"].AsInt()), at + " index");
    }

    const auto exported = TInboundSession::FromExport(vectors.StringOr("export_256"));
    Expect(exported && exported->FirstKnownIndex() == 256, "vodozemac export accepted at 256");
    if (exported) {
        for (const auto& m : vectors["messages"].AsArray()) {
            const auto decrypted = exported->Decrypt(m.StringOr("ciphertext"));
            if (m["index"].AsInt() < 256) {
                Expect(!decrypted, "export at 256 cannot read message " + std::to_string(m["index"].AsInt()));
            } else {
                Expect(decrypted && decrypted->Plaintext == m.StringOr("plaintext"), "export reads message " + std::to_string(m["index"].AsInt()));
            }
        }
        Expect(inbound->Export(256) == vectors.StringOr("export_256"), "our export at 256 is byte-identical to vodozemac's");
    }
}

void TestOwnSessions() {
    TOutboundSession outbound;
    const std::string key = outbound.SessionKey();
    const std::string first = outbound.Encrypt("раз");
    const std::string second = outbound.Encrypt("два");
    const auto inbound = TInboundSession::FromSessionKey(key);
    Expect(inbound && inbound->SessionId() == outbound.SessionId(), "own session key");
    Expect(inbound && inbound->Decrypt(second)->Plaintext == "два" && inbound->Decrypt(first)->Plaintext == "раз",
        "decrypts out of order");

    // A key shared later does not open earlier messages
    const auto late = TInboundSession::FromSessionKey(outbound.SessionKey());
    Expect(late && late->FirstKnownIndex() == 2 && !late->Decrypt(first), "later key cannot read earlier messages");

    // Tampering: a flipped byte in the ciphertext breaks the signature
    auto bytes = *NCrypto::Base64Decode(second);
    bytes[5] ^= 1;
    Expect(!inbound->Decrypt(NCrypto::Base64Encode(bytes)), "tampered message rejected");

    // Another session's message is rejected
    TOutboundSession other;
    Expect(!inbound->Decrypt(other.Encrypt("чужое")), "message of another session rejected");

    // A session key with a broken signature is rejected
    auto keyBytes = *NCrypto::Base64Decode(key);
    keyBytes[10] ^= 1;
    Expect(!TInboundSession::FromSessionKey(NCrypto::Base64Encode(keyBytes)), "forged session key rejected");
    Expect(!TInboundSession::FromSessionKey("garbage") && !inbound->Decrypt("AwgA"), "garbage rejected");
}

} // namespace

int main() {
    TestRatchetArithmetic();
    TestVodozemacVectors(HOSHI_TESTDATA "/megolm_vodozemac.json");
    TestOwnSessions();
    if (Failures == 0) {
        std::printf("megolm_test: all passed\n");
    }
    return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
