#pragma once

// The cryptographic primitives Olm and Megolm are built from, on top of OpenSSL. Nothing here implements
// a primitive itself: every operation is one OpenSSL call, so the risky part stays in a reviewed library.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NHoshi::NCrypto {

using TBytes = std::vector<std::uint8_t>;
using TKey32 = std::array<std::uint8_t, 32>;
using TSignature = std::array<std::uint8_t, 64>;

TBytes Random(size_t size);

TKey32 Sha256(const TBytes& data);
TKey32 HmacSha256(const TBytes& key, const TBytes& data);
// HKDF-SHA-256 (RFC 5869): extract with salt, expand with info to length bytes
TBytes HkdfSha256(const TBytes& salt, const TBytes& ikm, const TBytes& info, size_t length);

// AES-256-CBC with PKCS#7 padding, as Olm and Megolm use it. Decrypt returns nullopt on bad padding.
TBytes Aes256CbcEncrypt(const TKey32& key, const std::array<std::uint8_t, 16>& iv, const TBytes& plaintext);
std::optional<TBytes> Aes256CbcDecrypt(const TKey32& key, const std::array<std::uint8_t, 16>& iv, const TBytes& ciphertext);

// X25519 Diffie-Hellman
struct TCurve25519KeyPair {
    TKey32 Private;
    TKey32 Public;

    static TCurve25519KeyPair Generate();
    static TCurve25519KeyPair FromPrivate(const TKey32& privateKey);
    TKey32 SharedSecret(const TKey32& theirPublic) const;
};

// Ed25519 signatures
struct TEd25519KeyPair {
    TKey32 Seed;  // the RFC 8032 private key
    TKey32 Public;

    static TEd25519KeyPair Generate();
    static TEd25519KeyPair FromSeed(const TKey32& seed);
    TSignature Sign(const TBytes& message) const;
};
bool Ed25519Verify(const TKey32& publicKey, const TBytes& message, const TSignature& signature);

// Matrix uses standard base64 without padding
std::string Base64Encode(const TBytes& data);
std::optional<TBytes> Base64Decode(std::string_view text);

TBytes ToBytes(std::string_view text);
template <size_t N>
TBytes ToBytes(const std::array<std::uint8_t, N>& array) {
    return TBytes(array.begin(), array.end());
}

} // namespace NHoshi::NCrypto
