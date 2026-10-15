#pragma once

// Short authentication strings for m.sas.v1 device verification, as in the Matrix client-server API
// ("Short Authentication String (SAS) verification"): key agreement curve25519-hkdf-sha256 and MAC method
// hkdf-hmac-sha256.v2. Both sides show the same emoji or numbers only if no one in between replaced the keys.

#include "crypto.h"

#include <array>
#include <optional>
#include <string>

namespace NHoshi::NSas {

class TSas {
public:
    TSas();

    // Base64 of the ephemeral Curve25519 key for m.key.verification.key
    std::string PublicKey() const;
    // Takes the other side's key; false if it is not a valid key. Needed before Bytes and Mac.
    bool SetTheirKey(const std::string& base64);
    bool HasSharedSecret() const { return SharedSecret.has_value(); }

    // HKDF of the shared secret with the info string, count bytes
    NCrypto::TBytes Bytes(const std::string& info, size_t count) const;
    // hkdf-hmac-sha256.v2: HMAC-SHA-256 of input with a key derived for info, in base64
    std::string Mac(const std::string& input, const std::string& info) const;

private:
    NCrypto::TCurve25519KeyPair Ephemeral;
    std::optional<NCrypto::TKey32> SharedSecret;
};

struct TEmoji {
    const char* Symbol;
    const char* Description;  // the spec's English name
};

// The 7 emoji from the first 6 SAS bytes: 42 bits, 6 bits each
std::array<TEmoji, 7> EmojiFromBytes(const NCrypto::TBytes& bytes);
std::array<int, 7> EmojiIndices(const NCrypto::TBytes& bytes);
// The 3 numbers from the first 5 SAS bytes: 39 bits, 13 bits each, plus 1000
std::array<int, 3> DecimalsFromBytes(const NCrypto::TBytes& bytes);

} // namespace NHoshi::NSas
