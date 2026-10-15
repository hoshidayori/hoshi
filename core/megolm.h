#pragma once

// Megolm, the group ratchet Matrix uses for room messages (m.megolm.v1.aes-sha2), following the
// specification in libolm's docs/megolm.md. Wire formats are byte-compatible with libolm and vodozemac.

#include "crypto.h"
#include "json.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace NHoshi::NMegolm {

using NCrypto::TBytes;

// The ratchet R(i): four 32-byte parts and the 32-bit index i
struct TRatchet {
    std::array<std::uint8_t, 128> Data{};
    std::uint32_t Index = 0;

    // R(i) -> R(i+1)
    void Advance();
    // R(i) -> R(target) in at most 4 * 256 hashes; target below Index wraps around 2^32, as in libolm
    void AdvanceTo(std::uint32_t target);
};

// Builds the message for the ratchet's current index and signs it. Does not advance.
TBytes EncryptMessage(const TRatchet& ratchet, const NCrypto::TEd25519KeyPair& signing, const TBytes& plaintext);

class TOutboundSession {
public:
    TOutboundSession();

    // Base64 of the message for the content's "ciphertext"; advances the ratchet
    std::string Encrypt(const std::string& plaintext);
    // Base64 of the signing public key
    std::string SessionId() const;
    // The m.room_key "session_key": version 2, index, ratchet, signing key, signature; for the current index
    std::string SessionKey() const;
    std::uint32_t MessageIndex() const { return Ratchet.Index; }

    TJson Save() const;
    static std::optional<TOutboundSession> Load(const TJson& saved);

private:
    TOutboundSession(TRatchet ratchet, NCrypto::TEd25519KeyPair signing)
        : Ratchet(ratchet)
        , Signing(signing)
    {}

    TRatchet Ratchet;
    NCrypto::TEd25519KeyPair Signing;
};

struct TDecrypted {
    std::string Plaintext;
    std::uint32_t MessageIndex = 0;
};

class TInboundSession {
public:
    // From an m.room_key session_key (version 2, signature checked) or an exported key (version 1)
    static std::optional<TInboundSession> FromSessionKey(const std::string& sessionKey);
    static std::optional<TInboundSession> FromExport(const std::string& exported);

    // Nullopt for a message of another session, an index before the first known one, a bad MAC or signature
    std::optional<TDecrypted> Decrypt(const std::string& ciphertext) const;
    std::string SessionId() const;
    std::uint32_t FirstKnownIndex() const { return Initial.Index; }
    // Exported key (version 1) at the given index, at or after FirstKnownIndex()
    std::optional<std::string> Export(std::uint32_t index) const;

    // Saved as the export at the first known index
    TJson Save() const;
    static std::optional<TInboundSession> Load(const TJson& saved);

private:
    TRatchet Initial;
    NCrypto::TKey32 SigningKey{};
};

} // namespace NHoshi::NMegolm
