#include "olm.h"

#include <algorithm>

namespace NHoshi::NOlm {

namespace {

using namespace NCrypto;

constexpr std::uint8_t Version = 3;
constexpr size_t MacSize = 8;
constexpr size_t MaxReceiverChains = 5;   // as in libolm
constexpr size_t MaxSkippedKeys = 40;     // as in libolm
constexpr std::uint32_t MaxMessageGap = 2000;  // as in libolm

// Normal message fields
constexpr std::uint8_t RatchetKeyTag = 0x0A;  // field 1, bytes
constexpr std::uint8_t CounterTag = 0x10;     // field 2, varint
constexpr std::uint8_t CiphertextTag = 0x22;  // field 4, bytes
// Pre-key message fields
constexpr std::uint8_t OneTimeKeyTag = 0x0A;  // field 1
constexpr std::uint8_t BaseKeyTag = 0x12;     // field 2
constexpr std::uint8_t IdentityKeyTag = 0x1A; // field 3
constexpr std::uint8_t MessageTag = 0x22;     // field 4

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

void PutBytes(TBytes& out, std::uint8_t tag, const TBytes& bytes) {
    out.push_back(tag);
    PutVarint(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// The fields of a protobuf-like message: varints by tag and byte strings by tag; unknown fields are skipped
struct TFields {
    std::map<std::uint64_t, std::uint64_t> Varints;
    std::map<std::uint64_t, TBytes> Bytes;
};

std::optional<TFields> ParseFields(const TBytes& in, size_t begin, size_t end) {
    TFields fields;
    size_t pos = begin;
    while (pos < end) {
        const auto tag = GetVarint(in, pos, end);
        if (!tag) {
            return std::nullopt;
        }
        if ((*tag & 7) == 0) {
            const auto value = GetVarint(in, pos, end);
            if (!value) {
                return std::nullopt;
            }
            fields.Varints[*tag] = *value;
        } else if ((*tag & 7) == 2) {
            const auto length = GetVarint(in, pos, end);
            if (!length || *length > end - pos) {
                return std::nullopt;
            }
            fields.Bytes[*tag] = TBytes(in.begin() + pos, in.begin() + pos + *length);
            pos += *length;
        } else {
            return std::nullopt;
        }
    }
    return fields;
}

std::optional<TKey32> Key32(const TBytes& bytes) {
    if (bytes.size() != 32) {
        return std::nullopt;
    }
    TKey32 key{};
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

std::optional<TKey32> Key32FromBase64(const std::string& text) {
    const auto bytes = Base64Decode(text);
    return bytes ? Key32(*bytes) : std::nullopt;
}

std::optional<TKey32> Key32FromJson(const TJson& json, const char* name) {
    return json[name].IsString() ? Key32FromBase64(json[name].AsString()) : std::nullopt;
}

std::string B64(const TKey32& key) {
    return Base64Encode(ToBytes(key));
}

TKey32 Concat3Hkdf(const TKey32& a, const TKey32& b, const TKey32& c, TKey32& chainKey) {
    TBytes secret;
    for (const auto* part : {&a, &b, &c}) {
        secret.insert(secret.end(), part->begin(), part->end());
    }
    const TBytes derived = HkdfSha256({}, secret, ToBytes("OLM_ROOT"), 64);
    TKey32 root{};
    std::copy(derived.begin(), derived.begin() + 32, root.begin());
    std::copy(derived.begin() + 32, derived.end(), chainKey.begin());
    return root;
}

// A ratchet step: R(i) || C(i,0) = HKDF(salt = R(i-1), ECDH(ours, theirs), "OLM_RATCHET")
void RatchetStep(const TKey32& rootKey, const TCurve25519KeyPair& ours, const TKey32& theirs, TKey32& newRoot, TKey32& chainKey) {
    const TKey32 secret = ours.SharedSecret(theirs);
    const TBytes derived = HkdfSha256(ToBytes(rootKey), ToBytes(secret), ToBytes("OLM_RATCHET"), 64);
    std::copy(derived.begin(), derived.begin() + 32, newRoot.begin());
    std::copy(derived.begin() + 32, derived.end(), chainKey.begin());
}

TKey32 MessageKey(const TKey32& chainKey) {
    return HmacSha256(ToBytes(chainKey), TBytes{0x01});
}

TKey32 NextChainKey(const TKey32& chainKey) {
    return HmacSha256(ToBytes(chainKey), TBytes{0x02});
}

struct TCipherKeys {
    TKey32 Aes;
    TKey32 Mac;
    std::array<std::uint8_t, 16> Iv;
};

TCipherKeys CipherKeys(const TKey32& messageKey) {
    const TBytes okm = HkdfSha256({}, ToBytes(messageKey), ToBytes("OLM_KEYS"), 80);
    TCipherKeys keys{};
    std::copy(okm.begin(), okm.begin() + 32, keys.Aes.begin());
    std::copy(okm.begin() + 32, okm.begin() + 64, keys.Mac.begin());
    std::copy(okm.begin() + 64, okm.end(), keys.Iv.begin());
    return keys;
}

TBytes EncodeNormal(const TKey32& ratchetKey, std::uint32_t counter, const TKey32& messageKey, const std::string& plaintext) {
    const TCipherKeys keys = CipherKeys(messageKey);
    TBytes message{Version};
    PutBytes(message, RatchetKeyTag, ToBytes(ratchetKey));
    message.push_back(CounterTag);
    PutVarint(message, counter);
    PutBytes(message, CiphertextTag, Aes256CbcEncrypt(keys.Aes, keys.Iv, ToBytes(plaintext)));
    const TKey32 mac = HmacSha256(ToBytes(keys.Mac), message);
    message.insert(message.end(), mac.begin(), mac.begin() + MacSize);
    return message;
}

// Checks the MAC and decrypts a normal message with one message key
std::optional<std::string> DecryptWithKey(const TBytes& message, const TBytes& ciphertext, const TKey32& messageKey) {
    const TCipherKeys keys = CipherKeys(messageKey);
    const TBytes macInput(message.begin(), message.end() - MacSize);
    const TKey32 mac = HmacSha256(ToBytes(keys.Mac), macInput);
    if (!std::equal(message.end() - MacSize, message.end(), mac.begin())) {
        return std::nullopt;
    }
    const auto plaintext = Aes256CbcDecrypt(keys.Aes, keys.Iv, ciphertext);
    if (!plaintext) {
        return std::nullopt;
    }
    return std::string(plaintext->begin(), plaintext->end());
}

struct TParsedNormal {
    TKey32 RatchetKey{};
    std::uint32_t Counter = 0;
    TBytes Ciphertext;
};

std::optional<TParsedNormal> ParseNormal(const TBytes& message) {
    if (message.size() < 1 + MacSize || message[0] != Version) {
        return std::nullopt;
    }
    const auto fields = ParseFields(message, 1, message.size() - MacSize);
    if (!fields || !fields->Bytes.count(RatchetKeyTag) || !fields->Varints.count(CounterTag) || !fields->Bytes.count(CiphertextTag)) {
        return std::nullopt;
    }
    const auto ratchet = Key32(fields->Bytes.at(RatchetKeyTag));
    if (!ratchet || fields->Varints.at(CounterTag) > 0xFFFFFFFFu) {
        return std::nullopt;
    }
    return TParsedNormal{*ratchet, static_cast<std::uint32_t>(fields->Varints.at(CounterTag)), fields->Bytes.at(CiphertextTag)};
}

struct TParsedPreKey {
    TKey32 OneTimeKey{};
    TKey32 BaseKey{};
    TKey32 IdentityKey{};
    TBytes Message;
};

std::optional<TParsedPreKey> ParsePreKey(const std::string& body) {
    const auto bytes = Base64Decode(body);
    if (!bytes || bytes->empty() || (*bytes)[0] != Version) {
        return std::nullopt;
    }
    const auto fields = ParseFields(*bytes, 1, bytes->size());
    if (!fields || !fields->Bytes.count(OneTimeKeyTag) || !fields->Bytes.count(BaseKeyTag) ||
        !fields->Bytes.count(IdentityKeyTag) || !fields->Bytes.count(MessageTag)) {
        return std::nullopt;
    }
    const auto oneTime = Key32(fields->Bytes.at(OneTimeKeyTag));
    const auto base = Key32(fields->Bytes.at(BaseKeyTag));
    const auto identity = Key32(fields->Bytes.at(IdentityKeyTag));
    if (!oneTime || !base || !identity) {
        return std::nullopt;
    }
    return TParsedPreKey{*oneTime, *base, *identity, fields->Bytes.at(MessageTag)};
}

std::string KeyId(std::uint32_t id) {
    // libolm's key ids are the base64 of the 32-bit counter, big-endian
    return Base64Encode(TBytes{static_cast<std::uint8_t>(id >> 24), static_cast<std::uint8_t>(id >> 16),
        static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id)});
}

} // namespace

// Account

TAccount TAccount::Create() {
    TAccount account;
    account.Identity = TCurve25519KeyPair::Generate();
    account.Fingerprint = TEd25519KeyPair::Generate();
    return account;
}

std::string TAccount::IdentityKey() const {
    return B64(Identity.Public);
}

std::string TAccount::FingerprintKey() const {
    return B64(Fingerprint.Public);
}

std::string TAccount::Sign(const std::string& message) const {
    return Base64Encode(ToBytes(Fingerprint.Sign(ToBytes(message))));
}

void TAccount::GenerateOneTimeKeys(size_t count) {
    for (size_t i = 0; i < count; ++i) {
        OneTimeKeys.push_back({++NextKeyId, TCurve25519KeyPair::Generate(), false});
    }
}

std::map<std::string, std::string> TAccount::UnpublishedOneTimeKeys() const {
    std::map<std::string, std::string> keys;
    for (const auto& key : OneTimeKeys) {
        if (!key.Published) {
            keys[KeyId(key.Id)] = B64(key.Key.Public);
        }
    }
    return keys;
}

void TAccount::MarkKeysAsPublished() {
    for (auto& key : OneTimeKeys) {
        key.Published = true;
    }
}

std::optional<TCurve25519KeyPair> TAccount::TakeOneTimeKey(const TKey32& publicKey) {
    const auto it = std::find_if(OneTimeKeys.begin(), OneTimeKeys.end(), [&](const TOneTimeKey& key) {
        return key.Key.Public == publicKey;
    });
    if (it == OneTimeKeys.end()) {
        return std::nullopt;
    }
    TCurve25519KeyPair key = it->Key;
    OneTimeKeys.erase(it);
    return key;
}

TJson TAccount::Save() const {
    TJson::TArray keys;
    for (const auto& key : OneTimeKeys) {
        keys.push_back(TJson::TObject{{"id", static_cast<std::int64_t>(key.Id)}, {"private", B64(key.Key.Private)}, {"published", key.Published}});
    }
    return TJson(TJson::TObject{
        {"identity", B64(Identity.Private)},
        {"fingerprint", B64(Fingerprint.Seed)},
        {"one_time_keys", std::move(keys)},
        {"next_key_id", static_cast<std::int64_t>(NextKeyId)},
    });
}

std::optional<TAccount> TAccount::Load(const TJson& saved) {
    const auto identity = Key32FromJson(saved, "identity");
    const auto fingerprint = Key32FromJson(saved, "fingerprint");
    if (!identity || !fingerprint || !saved["next_key_id"].IsInt() || !saved["one_time_keys"].IsArray()) {
        return std::nullopt;
    }
    TAccount account;
    account.Identity = TCurve25519KeyPair::FromPrivate(*identity);
    account.Fingerprint = TEd25519KeyPair::FromSeed(*fingerprint);
    account.NextKeyId = static_cast<std::uint32_t>(saved["next_key_id"].AsInt());
    for (const auto& key : saved["one_time_keys"].AsArray()) {
        const auto priv = Key32FromJson(key, "private");
        if (!priv || !key["id"].IsInt()) {
            return std::nullopt;
        }
        account.OneTimeKeys.push_back({static_cast<std::uint32_t>(key["id"].AsInt()), TCurve25519KeyPair::FromPrivate(*priv),
            key["published"].IsBool() && key["published"].AsBool()});
    }
    return account;
}

// Session

TSession TSession::CreateOutbound(const TAccount& account, const std::string& theirIdentityKey, const std::string& theirOneTimeKey) {
    const auto bobIdentity = Key32FromBase64(theirIdentityKey);
    const auto bobOneTime = Key32FromBase64(theirOneTimeKey);
    if (!bobIdentity || !bobOneTime) {
        throw std::runtime_error("olm: bad identity or one-time key");
    }
    const auto baseKey = TCurve25519KeyPair::Generate();
    TSession session;
    // S = ECDH(I_A, E_B) || ECDH(E_A, I_B) || ECDH(E_A, E_B)
    TKey32 chainKey{};
    session.RootKey = Concat3Hkdf(account.Identity.SharedSecret(*bobOneTime), baseKey.SharedSecret(*bobIdentity),
        baseKey.SharedSecret(*bobOneTime), chainKey);
    session.OurRatchet = TCurve25519KeyPair::Generate();
    session.SenderChain = TChain{session.OurRatchet->Public, chainKey, 0};
    session.AliceIdentityKey = account.Identity.Public;
    session.AliceBaseKey = baseKey.Public;
    session.BobOneTimeKey = *bobOneTime;
    return session;
}

std::optional<TSession::TInbound> TSession::CreateInbound(TAccount& account, const std::string& preKeyBody) {
    const auto preKey = ParsePreKey(preKeyBody);
    if (!preKey) {
        return std::nullopt;
    }
    const auto inner = ParseNormal(preKey->Message);
    if (!inner) {
        return std::nullopt;
    }
    // Look the one-time key up without taking it: it is only used up if the message decrypts
    const auto known = std::find_if(account.OneTimeKeys.begin(), account.OneTimeKeys.end(), [&](const TAccount::TOneTimeKey& key) {
        return key.Key.Public == preKey->OneTimeKey;
    });
    if (known == account.OneTimeKeys.end()) {
        return std::nullopt;
    }
    const TCurve25519KeyPair oneTime = known->Key;

    TSession session;
    // S = ECDH(E_B, I_A) || ECDH(I_B, E_A) || ECDH(E_B, E_A), the same secret seen from Bob's side
    TKey32 chainKey{};
    session.RootKey = Concat3Hkdf(oneTime.SharedSecret(preKey->IdentityKey), account.Identity.SharedSecret(preKey->BaseKey),
        oneTime.SharedSecret(preKey->BaseKey), chainKey);
    session.ReceiverChains.push_back(TChain{inner->RatchetKey, chainKey, 0});
    session.ReceivedMessage = true;
    session.AliceIdentityKey = preKey->IdentityKey;
    session.AliceBaseKey = preKey->BaseKey;
    session.BobOneTimeKey = preKey->OneTimeKey;

    auto plaintext = session.DecryptNormal(preKey->Message);
    if (!plaintext) {
        return std::nullopt;
    }
    account.TakeOneTimeKey(preKey->OneTimeKey);
    return TInbound{std::move(session), std::move(*plaintext)};
}

bool TSession::MatchesInbound(const std::string& preKeyBody) const {
    const auto preKey = ParsePreKey(preKeyBody);
    return preKey && preKey->BaseKey == AliceBaseKey && preKey->OneTimeKey == BobOneTimeKey && preKey->IdentityKey == AliceIdentityKey;
}

TOlmMessage TSession::Encrypt(const std::string& plaintext) {
    if (!SenderChain) {
        // We received a new ratchet key since we last sent: step the ratchet with a fresh key of ours
        OurRatchet = TCurve25519KeyPair::Generate();
        TChain chain{OurRatchet->Public, {}, 0};
        TKey32 newRoot{};
        RatchetStep(RootKey, *OurRatchet, ReceiverChains.front().RatchetKey, newRoot, chain.ChainKey);
        RootKey = newRoot;
        SenderChain = chain;
    }
    const TKey32 messageKey = MessageKey(SenderChain->ChainKey);
    const TBytes inner = EncodeNormal(SenderChain->RatchetKey, SenderChain->Index, messageKey, plaintext);
    SenderChain->ChainKey = NextChainKey(SenderChain->ChainKey);
    ++SenderChain->Index;

    if (ReceivedMessage) {
        return {1, Base64Encode(inner)};
    }
    // Until the other side answers, every message repeats what it needs to set up the session
    TBytes preKey{Version};
    PutBytes(preKey, OneTimeKeyTag, ToBytes(BobOneTimeKey));
    PutBytes(preKey, BaseKeyTag, ToBytes(AliceBaseKey));
    PutBytes(preKey, IdentityKeyTag, ToBytes(AliceIdentityKey));
    PutBytes(preKey, MessageTag, inner);
    return {0, Base64Encode(preKey)};
}

std::optional<std::string> TSession::Decrypt(const TOlmMessage& message) {
    if (message.Type == 0) {
        const auto preKey = ParsePreKey(message.Body);
        if (!preKey || !MatchesInbound(message.Body)) {
            return std::nullopt;
        }
        return DecryptNormal(preKey->Message);
    }
    if (message.Type != 1) {
        return std::nullopt;
    }
    const auto bytes = NCrypto::Base64Decode(message.Body);
    return bytes ? DecryptNormal(*bytes) : std::nullopt;
}

std::optional<std::string> TSession::DecryptNormal(const TBytes& message) {
    const auto parsed = ParseNormal(message);
    if (!parsed) {
        return std::nullopt;
    }

    const auto chainIt = std::find_if(ReceiverChains.begin(), ReceiverChains.end(), [&](const TChain& chain) {
        return chain.RatchetKey == parsed->RatchetKey;
    });

    // An earlier message of a known chain: only its saved key can open it
    if (chainIt != ReceiverChains.end() && parsed->Counter < chainIt->Index) {
        const auto skipped = std::find_if(SkippedKeys.begin(), SkippedKeys.end(), [&](const TSkippedKey& key) {
            return key.RatchetKey == parsed->RatchetKey && key.Index == parsed->Counter;
        });
        if (skipped == SkippedKeys.end()) {
            return std::nullopt;
        }
        auto plaintext = DecryptWithKey(message, parsed->Ciphertext, skipped->MessageKey);
        if (plaintext) {
            SkippedKeys.erase(skipped);
        }
        return plaintext;
    }

    // Work on copies, so that a message that fails to decrypt changes nothing
    TChain chain{};
    TKey32 newRoot = RootKey;
    const bool newChain = chainIt == ReceiverChains.end();
    if (newChain) {
        if (!OurRatchet) {
            return std::nullopt;  // Bob before sending anything knows only Alice's first ratchet key
        }
        chain.RatchetKey = parsed->RatchetKey;
        RatchetStep(RootKey, *OurRatchet, parsed->RatchetKey, newRoot, chain.ChainKey);
    } else {
        chain = *chainIt;
    }
    if (parsed->Counter - chain.Index > MaxMessageGap) {
        return std::nullopt;
    }
    std::vector<TSkippedKey> skipped;
    while (chain.Index < parsed->Counter) {
        skipped.push_back({chain.RatchetKey, chain.Index, MessageKey(chain.ChainKey)});
        chain.ChainKey = NextChainKey(chain.ChainKey);
        ++chain.Index;
    }
    auto plaintext = DecryptWithKey(message, parsed->Ciphertext, MessageKey(chain.ChainKey));
    if (!plaintext) {
        return std::nullopt;
    }

    // Commit
    chain.ChainKey = NextChainKey(chain.ChainKey);
    ++chain.Index;
    if (newChain) {
        RootKey = newRoot;
        ReceiverChains.insert(ReceiverChains.begin(), chain);
        if (ReceiverChains.size() > MaxReceiverChains) {
            ReceiverChains.pop_back();
        }
        SenderChain.reset();  // our next message steps the ratchet
    } else {
        *chainIt = chain;
    }
    SkippedKeys.insert(SkippedKeys.end(), skipped.begin(), skipped.end());
    if (SkippedKeys.size() > MaxSkippedKeys) {
        SkippedKeys.erase(SkippedKeys.begin(), SkippedKeys.end() - MaxSkippedKeys);
    }
    ReceivedMessage = true;
    return plaintext;
}

std::string TSession::SessionId() const {
    TBytes input;
    for (const auto* key : {&AliceIdentityKey, &AliceBaseKey, &BobOneTimeKey}) {
        input.insert(input.end(), key->begin(), key->end());
    }
    return B64(Sha256(input));
}

TJson TSession::Save() const {
    auto chainJson = [](const TChain& chain) {
        return TJson(TJson::TObject{{"ratchet_key", B64(chain.RatchetKey)}, {"chain_key", B64(chain.ChainKey)},
            {"index", static_cast<std::int64_t>(chain.Index)}});
    };
    TJson::TArray receivers;
    for (const auto& chain : ReceiverChains) {
        receivers.push_back(chainJson(chain));
    }
    TJson::TArray skipped;
    for (const auto& key : SkippedKeys) {
        skipped.push_back(TJson::TObject{{"ratchet_key", B64(key.RatchetKey)}, {"index", static_cast<std::int64_t>(key.Index)},
            {"message_key", B64(key.MessageKey)}});
    }
    TJson::TObject saved{
        {"received_message", ReceivedMessage},
        {"alice_identity_key", B64(AliceIdentityKey)},
        {"alice_base_key", B64(AliceBaseKey)},
        {"bob_one_time_key", B64(BobOneTimeKey)},
        {"root_key", B64(RootKey)},
        {"receiver_chains", std::move(receivers)},
        {"skipped_keys", std::move(skipped)},
    };
    if (OurRatchet) {
        saved["our_ratchet"] = B64(OurRatchet->Private);
    }
    if (SenderChain) {
        saved["sender_chain"] = chainJson(*SenderChain);
    }
    return TJson(std::move(saved));
}

std::optional<TSession> TSession::Load(const TJson& saved) {
    auto chainFrom = [](const TJson& json) -> std::optional<TChain> {
        const auto ratchet = Key32FromJson(json, "ratchet_key");
        const auto chainKey = Key32FromJson(json, "chain_key");
        if (!ratchet || !chainKey || !json["index"].IsInt()) {
            return std::nullopt;
        }
        return TChain{*ratchet, *chainKey, static_cast<std::uint32_t>(json["index"].AsInt())};
    };
    TSession session;
    const auto identity = Key32FromJson(saved, "alice_identity_key");
    const auto base = Key32FromJson(saved, "alice_base_key");
    const auto oneTime = Key32FromJson(saved, "bob_one_time_key");
    const auto root = Key32FromJson(saved, "root_key");
    if (!identity || !base || !oneTime || !root || !saved["receiver_chains"].IsArray() || !saved["skipped_keys"].IsArray()) {
        return std::nullopt;
    }
    session.ReceivedMessage = saved["received_message"].IsBool() && saved["received_message"].AsBool();
    session.AliceIdentityKey = *identity;
    session.AliceBaseKey = *base;
    session.BobOneTimeKey = *oneTime;
    session.RootKey = *root;
    if (const auto ours = Key32FromJson(saved, "our_ratchet")) {
        session.OurRatchet = TCurve25519KeyPair::FromPrivate(*ours);
    }
    if (saved["sender_chain"].IsObject()) {
        session.SenderChain = chainFrom(saved["sender_chain"]);
        if (!session.SenderChain) {
            return std::nullopt;
        }
    }
    for (const auto& json : saved["receiver_chains"].AsArray()) {
        const auto chain = chainFrom(json);
        if (!chain) {
            return std::nullopt;
        }
        session.ReceiverChains.push_back(*chain);
    }
    for (const auto& json : saved["skipped_keys"].AsArray()) {
        const auto ratchet = Key32FromJson(json, "ratchet_key");
        const auto key = Key32FromJson(json, "message_key");
        if (!ratchet || !key || !json["index"].IsInt()) {
            return std::nullopt;
        }
        session.SkippedKeys.push_back({*ratchet, static_cast<std::uint32_t>(json["index"].AsInt()), *key});
    }
    return session;
}

} // namespace NHoshi::NOlm
