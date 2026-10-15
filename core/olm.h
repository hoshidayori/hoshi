#pragma once

// Olm, the pairwise double ratchet Matrix uses between devices (m.olm.v1.curve25519-aes-sha2), following
// libolm's docs/olm.md. Wire formats are byte-compatible with libolm and vodozemac.

#include "crypto.h"
#include "json.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace NHoshi::NOlm {

using NCrypto::TBytes;
using NCrypto::TKey32;

// The identity of a device and its one-time keys
class TAccount {
public:
    static TAccount Create();

    std::string IdentityKey() const;     // Curve25519, base64: "curve25519:DEVICE" in device keys
    std::string FingerprintKey() const;  // Ed25519, base64: "ed25519:DEVICE"
    std::string Sign(const std::string& message) const;  // base64 Ed25519 signature

    // New one-time keys; key ids are base64 of a counter, as in libolm
    void GenerateOneTimeKeys(size_t count);
    // Keys not yet published: key id -> base64 Curve25519 public key
    std::map<std::string, std::string> UnpublishedOneTimeKeys() const;
    void MarkKeysAsPublished();

    TJson Save() const;
    static std::optional<TAccount> Load(const TJson& saved);

private:
    friend class TSession;
    struct TOneTimeKey {
        std::uint32_t Id = 0;
        NCrypto::TCurve25519KeyPair Key;
        bool Published = false;
    };

    // Finds the one-time key with this public key and forgets it: a one-time key is used once
    std::optional<NCrypto::TCurve25519KeyPair> TakeOneTimeKey(const TKey32& publicKey);

    NCrypto::TCurve25519KeyPair Identity{};
    NCrypto::TEd25519KeyPair Fingerprint{};
    std::vector<TOneTimeKey> OneTimeKeys;
    std::uint32_t NextKeyId = 0;
};

struct TOlmMessage {
    int Type = 0;  // 0 = pre-key message, 1 = normal message
    std::string Body;  // base64
};

class TSession {
public:
    // Alice: from Bob's identity key and one of his one-time keys
    static TSession CreateOutbound(const TAccount& account, const std::string& theirIdentityKey, const std::string& theirOneTimeKey);
    // Bob: from Alice's first pre-key message; uses up the one-time key it names. The plaintext of that
    // message comes back too, since the session is only worth keeping if it decrypts.
    struct TInbound;
    static std::optional<TInbound> CreateInbound(TAccount& account, const std::string& preKeyBody);

    // Whether a pre-key message belongs to this session (same base key, one-time key and sender identity)
    bool MatchesInbound(const std::string& preKeyBody) const;

    TOlmMessage Encrypt(const std::string& plaintext);
    // Nullopt for a message this session cannot decrypt; the session is unchanged then
    std::optional<std::string> Decrypt(const TOlmMessage& message);

    std::string SessionId() const;
    TJson Save() const;
    static std::optional<TSession> Load(const TJson& saved);

private:
    struct TChain {
        TKey32 RatchetKey{};  // the sender's ratchet public key for this chain
        TKey32 ChainKey{};
        std::uint32_t Index = 0;
    };
    struct TSkippedKey {
        TKey32 RatchetKey{};
        std::uint32_t Index = 0;
        TKey32 MessageKey{};
    };

    std::optional<std::string> DecryptNormal(const TBytes& message);

    // Pre-key fields, kept until the other side answers so every message we send can carry them
    bool ReceivedMessage = false;
    TKey32 AliceIdentityKey{};
    TKey32 AliceBaseKey{};
    TKey32 BobOneTimeKey{};

    TKey32 RootKey{};
    std::optional<NCrypto::TCurve25519KeyPair> OurRatchet;  // our current sending ratchet key
    std::optional<TChain> SenderChain;
    std::vector<TChain> ReceiverChains;   // newest first, at most a few
    std::vector<TSkippedKey> SkippedKeys; // for messages that arrive out of order
};

struct TSession::TInbound {
    TSession Session;
    std::string Plaintext;
};

} // namespace NHoshi::NOlm
