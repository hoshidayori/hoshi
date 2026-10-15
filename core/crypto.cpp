#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <memory>
#include <stdexcept>

namespace NHoshi::NCrypto {

namespace {

[[noreturn]] void Fail(const char* what) {
    throw std::runtime_error(std::string("crypto: ") + what);
}

void Check(int ok, const char* what) {
    if (ok != 1) {
        Fail(what);
    }
}

struct TPkeyDeleter {
    void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};
struct TPkeyCtxDeleter {
    void operator()(EVP_PKEY_CTX* ctx) const { EVP_PKEY_CTX_free(ctx); }
};
struct TCipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const { EVP_CIPHER_CTX_free(ctx); }
};
struct TMdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const { EVP_MD_CTX_free(ctx); }
};
using TPkey = std::unique_ptr<EVP_PKEY, TPkeyDeleter>;
using TPkeyCtx = std::unique_ptr<EVP_PKEY_CTX, TPkeyCtxDeleter>;

TPkey PrivateKey(int type, const TKey32& raw) {
    TPkey key(EVP_PKEY_new_raw_private_key(type, nullptr, raw.data(), raw.size()));
    if (!key) {
        Fail("bad private key");
    }
    return key;
}

TPkey PublicKey(int type, const TKey32& raw) {
    TPkey key(EVP_PKEY_new_raw_public_key(type, nullptr, raw.data(), raw.size()));
    if (!key) {
        Fail("bad public key");
    }
    return key;
}

TKey32 RawPublic(EVP_PKEY* key) {
    TKey32 out{};
    size_t size = out.size();
    Check(EVP_PKEY_get_raw_public_key(key, out.data(), &size), "raw public key");
    if (size != out.size()) {
        Fail("public key size");
    }
    return out;
}

TKey32 RandomKey() {
    TKey32 key{};
    Check(RAND_bytes(key.data(), static_cast<int>(key.size())), "random");
    return key;
}

} // namespace

TBytes Random(size_t size) {
    TBytes out(size);
    if (size > 0) {
        Check(RAND_bytes(out.data(), static_cast<int>(size)), "random");
    }
    return out;
}

TKey32 Sha256(const TBytes& data) {
    TKey32 out{};
    SHA256(data.data(), data.size(), out.data());
    return out;
}

TKey32 HmacSha256(const TBytes& key, const TBytes& data) {
    TKey32 out{};
    unsigned int size = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), data.data(), data.size(), out.data(), &size) || size != out.size()) {
        Fail("hmac");
    }
    return out;
}

TBytes HkdfSha256(const TBytes& salt, const TBytes& ikm, const TBytes& info, size_t length) {
    TPkeyCtx ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr));
    if (!ctx) {
        Fail("hkdf context");
    }
    Check(EVP_PKEY_derive_init(ctx.get()), "hkdf init");
    Check(EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha256()), "hkdf md");
    // An empty salt means a zero-filled one per RFC 5869; OpenSSL needs a non-null pointer
    static const std::uint8_t noSalt = 0;
    Check(EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), salt.empty() ? &noSalt : salt.data(), static_cast<int>(salt.size())), "hkdf salt");
    Check(EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), ikm.data(), static_cast<int>(ikm.size())), "hkdf key");
    if (!info.empty()) {
        Check(EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), info.data(), static_cast<int>(info.size())), "hkdf info");
    }
    TBytes out(length);
    size_t size = length;
    Check(EVP_PKEY_derive(ctx.get(), out.data(), &size), "hkdf derive");
    if (size != length) {
        Fail("hkdf length");
    }
    return out;
}

TBytes Aes256CbcEncrypt(const TKey32& key, const std::array<std::uint8_t, 16>& iv, const TBytes& plaintext) {
    std::unique_ptr<EVP_CIPHER_CTX, TCipherCtxDeleter> ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        Fail("cipher context");
    }
    Check(EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, key.data(), iv.data()), "aes init");
    TBytes out(plaintext.size() + 16);
    int size = 0;
    int last = 0;
    Check(EVP_EncryptUpdate(ctx.get(), out.data(), &size, plaintext.data(), static_cast<int>(plaintext.size())), "aes update");
    Check(EVP_EncryptFinal_ex(ctx.get(), out.data() + size, &last), "aes final");
    out.resize(static_cast<size_t>(size + last));
    return out;
}

std::optional<TBytes> Aes256CbcDecrypt(const TKey32& key, const std::array<std::uint8_t, 16>& iv, const TBytes& ciphertext) {
    if (ciphertext.empty() || ciphertext.size() % 16 != 0) {
        return std::nullopt;
    }
    std::unique_ptr<EVP_CIPHER_CTX, TCipherCtxDeleter> ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        Fail("cipher context");
    }
    Check(EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, key.data(), iv.data()), "aes init");
    TBytes out(ciphertext.size() + 16);
    int size = 0;
    int last = 0;
    Check(EVP_DecryptUpdate(ctx.get(), out.data(), &size, ciphertext.data(), static_cast<int>(ciphertext.size())), "aes update");
    if (EVP_DecryptFinal_ex(ctx.get(), out.data() + size, &last) != 1) {
        return std::nullopt;  // bad padding: wrong key or corrupted data
    }
    out.resize(static_cast<size_t>(size + last));
    return out;
}

TCurve25519KeyPair TCurve25519KeyPair::Generate() {
    return FromPrivate(RandomKey());
}

TCurve25519KeyPair TCurve25519KeyPair::FromPrivate(const TKey32& privateKey) {
    TPkey key = PrivateKey(EVP_PKEY_X25519, privateKey);
    return {privateKey, RawPublic(key.get())};
}

TKey32 TCurve25519KeyPair::SharedSecret(const TKey32& theirPublic) const {
    TPkey ours = PrivateKey(EVP_PKEY_X25519, Private);
    TPkey theirs = PublicKey(EVP_PKEY_X25519, theirPublic);
    TPkeyCtx ctx(EVP_PKEY_CTX_new(ours.get(), nullptr));
    if (!ctx) {
        Fail("x25519 context");
    }
    Check(EVP_PKEY_derive_init(ctx.get()), "x25519 init");
    Check(EVP_PKEY_derive_set_peer(ctx.get(), theirs.get()), "x25519 peer");
    TKey32 out{};
    size_t size = out.size();
    // Fails for a low-order public key, whose shared secret would be all zeros
    Check(EVP_PKEY_derive(ctx.get(), out.data(), &size), "x25519 derive");
    return out;
}

TEd25519KeyPair TEd25519KeyPair::Generate() {
    return FromSeed(RandomKey());
}

TEd25519KeyPair TEd25519KeyPair::FromSeed(const TKey32& seed) {
    TPkey key = PrivateKey(EVP_PKEY_ED25519, seed);
    return {seed, RawPublic(key.get())};
}

TSignature TEd25519KeyPair::Sign(const TBytes& message) const {
    TPkey key = PrivateKey(EVP_PKEY_ED25519, Seed);
    std::unique_ptr<EVP_MD_CTX, TMdCtxDeleter> ctx(EVP_MD_CTX_new());
    if (!ctx) {
        Fail("sign context");
    }
    Check(EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()), "sign init");
    TSignature out{};
    size_t size = out.size();
    Check(EVP_DigestSign(ctx.get(), out.data(), &size, message.data(), message.size()), "sign");
    return out;
}

bool Ed25519Verify(const TKey32& publicKey, const TBytes& message, const TSignature& signature) {
    TPkey key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, publicKey.data(), publicKey.size()));
    if (!key) {
        return false;
    }
    std::unique_ptr<EVP_MD_CTX, TMdCtxDeleter> ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        return false;
    }
    return EVP_DigestVerify(ctx.get(), signature.data(), signature.size(), message.data(), message.size()) == 1;
}

std::string Base64Encode(const TBytes& data) {
    static const char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += Alphabet[(v >> 18) & 63];
        out += Alphabet[(v >> 12) & 63];
        out += Alphabet[(v >> 6) & 63];
        out += Alphabet[v & 63];
    }
    if (data.size() - i == 1) {
        const std::uint32_t v = data[i] << 16;
        out += Alphabet[(v >> 18) & 63];
        out += Alphabet[(v >> 12) & 63];
    } else if (data.size() - i == 2) {
        const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += Alphabet[(v >> 18) & 63];
        out += Alphabet[(v >> 12) & 63];
        out += Alphabet[(v >> 6) & 63];
    }
    return out;
}

std::optional<TBytes> Base64Decode(std::string_view text) {
    // Accept padded input too: some implementations still send it
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    TBytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : text) {
        const int v = value(c);
        if (v < 0) {
            return std::nullopt;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
    }
    // Leftover bits must be zero for a canonical encoding
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) {
        return std::nullopt;
    }
    return out;
}

TBytes ToBytes(std::string_view text) {
    return TBytes(text.begin(), text.end());
}

} // namespace NHoshi::NCrypto
