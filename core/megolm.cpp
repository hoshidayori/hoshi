#include "megolm.h"

#include <algorithm>
#include <cstring>

namespace NHoshi::NMegolm {

namespace {

using namespace NCrypto;

constexpr std::uint8_t MessageVersion = 3;
constexpr std::uint8_t SessionKeyVersion = 2;
constexpr std::uint8_t ExportVersion = 1;
constexpr size_t MacSize = 8;  // the HMAC is truncated to 8 bytes
constexpr size_t SignatureSize = 64;
constexpr std::uint8_t IndexTag = 0x08;       // field 1, varint
constexpr std::uint8_t CiphertextTag = 0x12;  // field 2, length-delimited

// R(i,to) = HMAC-SHA-256(key = R(i,from), message = the single byte `to`)
void RehashPart(std::array<std::uint8_t, 128>& data, int from, int to) {
    const TBytes key(data.begin() + from * 32, data.begin() + from * 32 + 32);
    const TKey32 hashed = HmacSha256(key, TBytes{static_cast<std::uint8_t>(to)});
    std::copy(hashed.begin(), hashed.end(), data.begin() + to * 32);
}

struct TMessageKeys {
    TKey32 Aes;
    TKey32 Mac;
    std::array<std::uint8_t, 16> Iv;
};

// HKDF-SHA-256 with an empty salt and info "MEGOLM_KEYS" over the whole ratchet gives 80 bytes:
// AES key, HMAC key, IV
TMessageKeys DeriveKeys(const TRatchet& ratchet) {
    const TBytes okm = HkdfSha256({}, TBytes(ratchet.Data.begin(), ratchet.Data.end()), ToBytes("MEGOLM_KEYS"), 80);
    TMessageKeys keys{};
    std::copy(okm.begin(), okm.begin() + 32, keys.Aes.begin());
    std::copy(okm.begin() + 32, okm.begin() + 64, keys.Mac.begin());
    std::copy(okm.begin() + 64, okm.begin() + 80, keys.Iv.begin());
    return keys;
}

void PutVarint(TBytes& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

std::optional<std::uint64_t> GetVarint(const TBytes& in, size_t& pos, size_t end) {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64 && pos < end; shift += 7) {
        const std::uint8_t byte = in[pos++];
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) {
            return value;
        }
    }
    return std::nullopt;
}

void PutIndex(TBytes& out, std::uint32_t index) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(index >> shift));
    }
}

std::uint32_t GetIndex(const TBytes& in, size_t pos) {
    return (std::uint32_t(in[pos]) << 24) | (std::uint32_t(in[pos + 1]) << 16) | (std::uint32_t(in[pos + 2]) << 8) | in[pos + 3];
}

// A parsed group message
struct TParsed {
    std::uint32_t Index = 0;
    TBytes Ciphertext;
    TBytes Mac;        // over everything before it
    TBytes Signed;     // everything before the signature
    TSignature Signature{};
};

std::optional<TParsed> Parse(const TBytes& message) {
    if (message.size() < 1 + MacSize + SignatureSize || message[0] != MessageVersion) {
        return std::nullopt;
    }
    TParsed parsed;
    const size_t bodyEnd = message.size() - MacSize - SignatureSize;
    bool haveIndex = false;
    bool haveCiphertext = false;
    size_t pos = 1;
    while (pos < bodyEnd) {
        const auto tag = GetVarint(message, pos, bodyEnd);
        if (!tag) {
            return std::nullopt;
        }
        const std::uint64_t wireType = *tag & 7;
        if (*tag == IndexTag) {
            const auto index = GetVarint(message, pos, bodyEnd);
            if (!index || *index > 0xFFFFFFFFu) {
                return std::nullopt;
            }
            parsed.Index = static_cast<std::uint32_t>(*index);
            haveIndex = true;
        } else if (*tag == CiphertextTag) {
            const auto length = GetVarint(message, pos, bodyEnd);
            if (!length || *length > bodyEnd - pos) {
                return std::nullopt;
            }
            parsed.Ciphertext.assign(message.begin() + pos, message.begin() + pos + *length);
            pos += *length;
            haveCiphertext = true;
        } else if (wireType == 0) {
            // Unknown fields are skipped, as libolm does, for forward compatibility
            if (!GetVarint(message, pos, bodyEnd)) {
                return std::nullopt;
            }
        } else if (wireType == 2) {
            const auto length = GetVarint(message, pos, bodyEnd);
            if (!length || *length > bodyEnd - pos) {
                return std::nullopt;
            }
            pos += *length;
        } else {
            return std::nullopt;
        }
    }
    if (!haveIndex || !haveCiphertext) {
        return std::nullopt;
    }
    parsed.Mac.assign(message.begin() + bodyEnd, message.begin() + bodyEnd + MacSize);
    parsed.Signed.assign(message.begin(), message.end() - SignatureSize);
    std::copy(message.end() - SignatureSize, message.end(), parsed.Signature.begin());
    return parsed;
}

} // namespace

void TRatchet::Advance() {
    ++Index;
    // h is the highest part that changes: part 0 every 2^24 steps, part 1 every 2^16, part 2 every 2^8, part 3 always
    std::uint32_t mask = 0x00FFFFFF;
    int h = 0;
    while (h < 4) {
        if (!(Index & mask)) {
            break;
        }
        ++h;
        mask >>= 8;
    }
    // From the last part down, so that part h is rehashed from its old value last
    for (int i = 3; i >= h; --i) {
        RehashPart(Data, h, i);
    }
}

void TRatchet::AdvanceTo(std::uint32_t target) {
    for (int j = 0; j < 4; ++j) {
        const unsigned shift = static_cast<unsigned>(3 - j) * 8;
        const std::uint32_t mask = ~std::uint32_t(0) << shift;
        unsigned steps = ((target >> shift) - (Index >> shift)) & 0xFF;
        if (steps == 0) {
            // Same byte at this level: nothing to do, unless the target is behind and we must wrap around
            if (target < Index) {
                steps = 0x100;
            } else {
                continue;
            }
        }
        // All but the last step only advance part j itself
        while (steps > 1) {
            RehashPart(Data, j, j);
            --steps;
        }
        // The last step also reseeds the parts below it
        for (int k = 3; k >= j; --k) {
            RehashPart(Data, j, k);
        }
        Index = target & mask;
    }
}

TBytes EncryptMessage(const TRatchet& ratchet, const TEd25519KeyPair& signing, const TBytes& plaintext) {
    const TMessageKeys keys = DeriveKeys(ratchet);
    const TBytes ciphertext = Aes256CbcEncrypt(keys.Aes, keys.Iv, plaintext);
    TBytes message{MessageVersion};
    message.push_back(IndexTag);
    PutVarint(message, ratchet.Index);
    message.push_back(CiphertextTag);
    PutVarint(message, ciphertext.size());
    message.insert(message.end(), ciphertext.begin(), ciphertext.end());
    const TKey32 mac = HmacSha256(ToBytes(keys.Mac), message);
    message.insert(message.end(), mac.begin(), mac.begin() + MacSize);
    const TSignature signature = signing.Sign(message);
    message.insert(message.end(), signature.begin(), signature.end());
    return message;
}

TOutboundSession::TOutboundSession()
    : Signing(TEd25519KeyPair::Generate())
{
    const TBytes random = Random(Ratchet.Data.size());
    std::copy(random.begin(), random.end(), Ratchet.Data.begin());
}

std::string TOutboundSession::Encrypt(const std::string& plaintext) {
    const TBytes message = EncryptMessage(Ratchet, Signing, ToBytes(plaintext));
    Ratchet.Advance();
    return Base64Encode(message);
}

std::string TOutboundSession::SessionId() const {
    return Base64Encode(ToBytes(Signing.Public));
}

std::string TOutboundSession::SessionKey() const {
    TBytes key{SessionKeyVersion};
    PutIndex(key, Ratchet.Index);
    key.insert(key.end(), Ratchet.Data.begin(), Ratchet.Data.end());
    key.insert(key.end(), Signing.Public.begin(), Signing.Public.end());
    const TSignature signature = Signing.Sign(key);
    key.insert(key.end(), signature.begin(), signature.end());
    return Base64Encode(key);
}

std::optional<TInboundSession> TInboundSession::FromSessionKey(const std::string& sessionKey) {
    const auto key = Base64Decode(sessionKey);
    constexpr size_t Size = 1 + 4 + 128 + 32 + SignatureSize;
    if (!key || key->size() != Size || (*key)[0] != SessionKeyVersion) {
        return std::nullopt;
    }
    TInboundSession session;
    session.Initial.Index = GetIndex(*key, 1);
    std::copy(key->begin() + 5, key->begin() + 133, session.Initial.Data.begin());
    std::copy(key->begin() + 133, key->begin() + 165, session.SigningKey.begin());
    TSignature signature{};
    std::copy(key->begin() + 165, key->end(), signature.begin());
    if (!Ed25519Verify(session.SigningKey, TBytes(key->begin(), key->begin() + 165), signature)) {
        return std::nullopt;
    }
    return session;
}

std::optional<TInboundSession> TInboundSession::FromExport(const std::string& exported) {
    const auto key = Base64Decode(exported);
    constexpr size_t Size = 1 + 4 + 128 + 32;
    if (!key || key->size() != Size || (*key)[0] != ExportVersion) {
        return std::nullopt;
    }
    TInboundSession session;
    session.Initial.Index = GetIndex(*key, 1);
    std::copy(key->begin() + 5, key->begin() + 133, session.Initial.Data.begin());
    std::copy(key->begin() + 133, key->end(), session.SigningKey.begin());
    return session;
}

std::optional<TDecrypted> TInboundSession::Decrypt(const std::string& ciphertext) const {
    const auto message = Base64Decode(ciphertext);
    if (!message) {
        return std::nullopt;
    }
    const auto parsed = Parse(*message);
    if (!parsed || !Ed25519Verify(SigningKey, parsed->Signed, parsed->Signature)) {
        return std::nullopt;
    }
    if (parsed->Index < Initial.Index) {
        return std::nullopt;  // the key we were given starts later; the earlier messages stay unreadable
    }
    TRatchet ratchet = Initial;
    ratchet.AdvanceTo(parsed->Index);
    const TMessageKeys keys = DeriveKeys(ratchet);
    const TBytes macInput(parsed->Signed.begin(), parsed->Signed.end() - MacSize);
    const TKey32 mac = HmacSha256(ToBytes(keys.Mac), macInput);
    if (!std::equal(parsed->Mac.begin(), parsed->Mac.end(), mac.begin())) {
        return std::nullopt;
    }
    const auto plaintext = Aes256CbcDecrypt(keys.Aes, keys.Iv, parsed->Ciphertext);
    if (!plaintext) {
        return std::nullopt;
    }
    return TDecrypted{std::string(plaintext->begin(), plaintext->end()), parsed->Index};
}

std::string TInboundSession::SessionId() const {
    return Base64Encode(ToBytes(SigningKey));
}

std::optional<std::string> TInboundSession::Export(std::uint32_t index) const {
    if (index < Initial.Index) {
        return std::nullopt;
    }
    TRatchet ratchet = Initial;
    ratchet.AdvanceTo(index);
    TBytes key{ExportVersion};
    PutIndex(key, ratchet.Index);
    key.insert(key.end(), ratchet.Data.begin(), ratchet.Data.end());
    key.insert(key.end(), SigningKey.begin(), SigningKey.end());
    return Base64Encode(key);
}

TJson TOutboundSession::Save() const {
    return TJson(TJson::TObject{
        {"ratchet", Base64Encode(TBytes(Ratchet.Data.begin(), Ratchet.Data.end()))},
        {"index", static_cast<std::int64_t>(Ratchet.Index)},
        {"signing", Base64Encode(ToBytes(Signing.Seed))},
    });
}

std::optional<TOutboundSession> TOutboundSession::Load(const TJson& saved) {
    const auto ratchet = saved["ratchet"].IsString() ? Base64Decode(saved["ratchet"].AsString()) : std::nullopt;
    const auto seed = saved["signing"].IsString() ? Base64Decode(saved["signing"].AsString()) : std::nullopt;
    if (!ratchet || ratchet->size() != 128 || !seed || seed->size() != 32 || !saved["index"].IsInt()) {
        return std::nullopt;
    }
    TRatchet r;
    std::copy(ratchet->begin(), ratchet->end(), r.Data.begin());
    r.Index = static_cast<std::uint32_t>(saved["index"].AsInt());
    TKey32 s{};
    std::copy(seed->begin(), seed->end(), s.begin());
    return TOutboundSession(r, TEd25519KeyPair::FromSeed(s));
}

TJson TInboundSession::Save() const {
    return TJson(TJson::TObject{{"export", *Export(Initial.Index)}});
}

std::optional<TInboundSession> TInboundSession::Load(const TJson& saved) {
    return saved["export"].IsString() ? FromExport(saved["export"].AsString()) : std::nullopt;
}

} // namespace NHoshi::NMegolm
