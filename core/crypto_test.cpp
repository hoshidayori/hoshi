// Known-answer tests for the primitive wrappers, with vectors from the RFCs and NIST publications named below
#include "crypto.h"

#include <cstdio>
#include <cstdlib>

using namespace NHoshi::NCrypto;

namespace {

int Failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++Failures;
    }
}

TBytes Hex(const char* hex) {
    TBytes out;
    for (const char* p = hex; p[0] && p[1]; p += 2) {
        char byte[3] = {p[0], p[1], 0};
        out.push_back(static_cast<std::uint8_t>(std::strtoul(byte, nullptr, 16)));
    }
    return out;
}

template <size_t N>
std::array<std::uint8_t, N> HexArray(const char* hex) {
    const TBytes bytes = Hex(hex);
    std::array<std::uint8_t, N> out{};
    for (size_t i = 0; i < N && i < bytes.size(); ++i) {
        out[i] = bytes[i];
    }
    return out;
}

} // namespace

int main() {
    // FIPS 180-2, "abc"
    Expect(ToBytes(Sha256(ToBytes("abc"))) == Hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256");

    // RFC 4231, test case 2
    Expect(ToBytes(HmacSha256(ToBytes("Jefe"), ToBytes("what do ya want for nothing?"))) ==
        Hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "hmac-sha256");

    // RFC 5869, test case 1
    Expect(HkdfSha256(Hex("000102030405060708090a0b0c"), Hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"),
        Hex("f0f1f2f3f4f5f6f7f8f9"), 42) ==
        Hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"), "hkdf");

    // NIST SP 800-38A F.2.5, first block; PKCS#7 then adds a full padding block
    const TKey32 aesKey = HexArray<32>("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
    const auto iv = HexArray<16>("000102030405060708090a0b0c0d0e0f");
    const TBytes block = Hex("6bc1bee22e409f96e93d7e117393172a");
    const TBytes encrypted = Aes256CbcEncrypt(aesKey, iv, block);
    Expect(encrypted.size() == 32, "aes adds a padding block");
    Expect(TBytes(encrypted.begin(), encrypted.begin() + 16) == Hex("f58c4c04d6e5f1ba779eabfb5f7bfbd6"), "aes-256-cbc");
    Expect(Aes256CbcDecrypt(aesKey, iv, encrypted) == block, "aes round trip");
    TBytes corrupted = encrypted;
    corrupted.back() ^= 1;
    Expect(Aes256CbcDecrypt(aesKey, iv, corrupted) != block, "corrupted ciphertext does not decrypt to the plaintext");
    Expect(!Aes256CbcDecrypt(aesKey, iv, TBytes(15)), "partial block rejected");

    // RFC 7748 section 6.1
    const auto alice = TCurve25519KeyPair::FromPrivate(HexArray<32>("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"));
    const auto bob = TCurve25519KeyPair::FromPrivate(HexArray<32>("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"));
    Expect(ToBytes(alice.Public) == Hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"), "x25519 alice public");
    Expect(ToBytes(bob.Public) == Hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"), "x25519 bob public");
    const TBytes shared = Hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    Expect(ToBytes(alice.SharedSecret(bob.Public)) == shared, "x25519 shared secret (alice)");
    Expect(ToBytes(bob.SharedSecret(alice.Public)) == shared, "x25519 shared secret (bob)");
    const auto x = TCurve25519KeyPair::Generate();
    const auto y = TCurve25519KeyPair::Generate();
    Expect(x.SharedSecret(y.Public) == y.SharedSecret(x.Public), "x25519 generated keys agree");

    // RFC 8032 section 7.1, test 1 (empty message)
    const auto ed = TEd25519KeyPair::FromSeed(HexArray<32>("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"));
    Expect(ToBytes(ed.Public) == Hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"), "ed25519 public");
    const TSignature signature = ed.Sign({});
    Expect(ToBytes(signature) == Hex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"), "ed25519 signature");
    Expect(Ed25519Verify(ed.Public, {}, signature), "ed25519 verify");
    Expect(!Ed25519Verify(ed.Public, ToBytes("x"), signature), "ed25519 rejects another message");
    TSignature broken = signature;
    broken[0] ^= 1;
    Expect(!Ed25519Verify(ed.Public, {}, broken), "ed25519 rejects a changed signature");

    // RFC 4648 section 10, without padding as Matrix uses it
    Expect(Base64Encode(ToBytes("")).empty() && Base64Encode(ToBytes("f")) == "Zg" && Base64Encode(ToBytes("fo")) == "Zm8" &&
        Base64Encode(ToBytes("foo")) == "Zm9v" && Base64Encode(ToBytes("foob")) == "Zm9vYg" && Base64Encode(ToBytes("foobar")) == "Zm9vYmFy",
        "unpadded base64 encode");
    Expect(Base64Decode("Zm9vYg") == ToBytes("foob") && Base64Decode("Zm9vYg==") == ToBytes("foob"), "base64 decode, padded or not");
    Expect(!Base64Decode("Zm9vY") && !Base64Decode("Zm9v!") && !Base64Decode("Zh"), "invalid base64 rejected");
    const TBytes random = Random(37);
    Expect(Base64Decode(Base64Encode(random)) == random, "base64 round trip");

    if (Failures == 0) {
        std::printf("crypto_test: all passed\n");
    }
    return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
