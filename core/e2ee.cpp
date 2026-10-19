#include "e2ee.h"

#include <algorithm>

namespace NHoshi::NE2ee {

namespace {

// The object without "signatures" and "unsigned", the part a signature covers
TJson::TObject SignedPart(const TJson& object) {
    TJson::TObject copy = object.AsObject();
    copy.erase("signatures");
    copy.erase("unsigned");
    return copy;
}

std::string DeviceKeyName(const char* algorithm, const std::string& deviceId) {
    return std::string(algorithm) + ":" + deviceId;
}

// The public key of a cross-signing key object of the user with the usage, or empty if the object is not one.
// A cross-signing key is named after itself: "keys": {"ed25519:<key>": "<key>"}.
std::string CrossSigningKey(const TJson& object, const std::string& userId, const std::string& usage) {
    if (!object.IsObject() || object.StringOr("user_id") != userId || !object["usage"].IsArray()) {
        return {};
    }
    const auto& usages = object["usage"].AsArray();
    if (std::none_of(usages.begin(), usages.end(), [&](const TJson& u) { return u.IsString() && u.AsString() == usage; })) {
        return {};
    }
    const TJson& keys = object["keys"];
    if (!keys.IsObject() || keys.AsObject().size() != 1) {
        return {};
    }
    const auto& [keyId, key] = *keys.AsObject().begin();
    if (!key.IsString() || key.AsString().empty() || keyId != DeviceKeyName("ed25519", key.AsString())) {
        return {};
    }
    return key.AsString();
}

} // namespace

std::map<std::string, TCrossSigning> CrossSigningFromQuery(const TJson& response) {
    std::map<std::string, TCrossSigning> result;
    const TJson& masters = response["master_keys"];
    if (!masters.IsObject()) {
        return result;
    }
    for (const auto& [userId, masterObject] : masters.AsObject()) {
        TCrossSigning keys;
        keys.Master = CrossSigningKey(masterObject, userId, "master");
        if (keys.Master.empty()) {
            continue;
        }
        // The other keys are the master key's to vouch for: a signature by the master key, named after it
        const auto signedByMaster = [&](const char* field, const char* usage) -> std::string {
            const TJson& object = response[field][userId];
            const std::string key = CrossSigningKey(object, userId, usage);
            return !key.empty() && VerifyJson(object, userId, keys.Master, keys.Master) ? key : std::string();
        };
        keys.SelfSigning = signedByMaster("self_signing_keys", "self_signing");
        keys.UserSigning = signedByMaster("user_signing_keys", "user_signing");
        result.emplace(userId, std::move(keys));
    }
    return result;
}

TJson SignJson(const TJson& object, const std::string& userId, const std::string& deviceId, const NOlm::TAccount& account) {
    const std::string canonical = TJson(SignedPart(object)).Dump();
    TJson::TObject result = object.AsObject();
    TJson::TObject signatures = object["signatures"].IsObject() ? object["signatures"].AsObject() : TJson::TObject{};
    TJson::TObject mine = signatures[userId].IsObject() ? signatures[userId].AsObject() : TJson::TObject{};
    mine[DeviceKeyName("ed25519", deviceId)] = account.Sign(canonical);
    signatures[userId] = TJson(std::move(mine));
    result["signatures"] = TJson(std::move(signatures));
    return TJson(std::move(result));
}

TJson SignJsonWithKey(const TJson& object, const std::string& userId, const NCrypto::TEd25519KeyPair& key) {
    const std::string canonical = TJson(SignedPart(object)).Dump();
    const std::string publicKey = NCrypto::Base64Encode(NCrypto::ToBytes(key.Public));
    TJson::TObject result = object.AsObject();
    TJson::TObject signatures = object["signatures"].IsObject() ? object["signatures"].AsObject() : TJson::TObject{};
    TJson::TObject mine = signatures[userId].IsObject() ? signatures[userId].AsObject() : TJson::TObject{};
    mine[DeviceKeyName("ed25519", publicKey)] = NCrypto::Base64Encode(NCrypto::ToBytes(key.Sign(NCrypto::ToBytes(canonical))));
    signatures[userId] = TJson(std::move(mine));
    result["signatures"] = TJson(std::move(signatures));
    return TJson(std::move(result));
}

bool VerifyJson(const TJson& object, const std::string& userId, const std::string& deviceId, const std::string& ed25519Key) {
    if (!object.IsObject()) {
        return false;
    }
    const TJson& signature = object["signatures"][userId][DeviceKeyName("ed25519", deviceId)];
    const auto key = NCrypto::Base64Decode(ed25519Key);
    const auto sig = signature.IsString() ? NCrypto::Base64Decode(signature.AsString()) : std::nullopt;
    if (!key || key->size() != 32 || !sig || sig->size() != 64) {
        return false;
    }
    NCrypto::TKey32 publicKey{};
    std::copy(key->begin(), key->end(), publicKey.begin());
    NCrypto::TSignature raw{};
    std::copy(sig->begin(), sig->end(), raw.begin());
    return NCrypto::Ed25519Verify(publicKey, NCrypto::ToBytes(TJson(SignedPart(object)).Dump()), raw);
}

std::vector<TDevice> DevicesFromQuery(const TJson& response) {
    std::vector<TDevice> devices;
    const TJson& users = response["device_keys"];
    if (!users.IsObject()) {
        return devices;
    }
    const auto crossSigning = CrossSigningFromQuery(response);
    for (const auto& [userId, userDevices] : users.AsObject()) {
        if (!userDevices.IsObject()) {
            continue;
        }
        for (const auto& [deviceId, keys] : userDevices.AsObject()) {
            // The server could lie about whose keys these are, so the ids inside must match and the
            // device must have signed its own keys
            if (keys.StringOr("user_id") != userId || keys.StringOr("device_id") != deviceId) {
                continue;
            }
            const std::string ed25519 = keys["keys"].StringOr(DeviceKeyName("ed25519", deviceId));
            const std::string curve25519 = keys["keys"].StringOr(DeviceKeyName("curve25519", deviceId));
            if (ed25519.empty() || curve25519.empty() || !VerifyJson(keys, userId, deviceId, ed25519)) {
                continue;
            }
            std::string crossSignedBy;
            if (const auto it = crossSigning.find(userId); it != crossSigning.end() && !it->second.SelfSigning.empty() &&
                VerifyJson(keys, userId, it->second.SelfSigning, it->second.SelfSigning))
            {
                crossSignedBy = it->second.Master;
            }
            devices.push_back({userId, deviceId, curve25519, ed25519, std::move(crossSignedBy)});
        }
    }
    return devices;
}

TMachine::TMachine(std::string userId, std::string deviceId, NOlm::TAccount account)
    : UserId_(std::move(userId))
    , DeviceId_(std::move(deviceId))
    , Account_(std::move(account))
{}

TJson TMachine::KeysUploadBody(size_t newOneTimeKeys, bool withDeviceKeys) {
    TJson::TObject body;
    if (withDeviceKeys) {
        const TJson deviceKeys(TJson::TObject{
            {"user_id", UserId_},
            {"device_id", DeviceId_},
            {"algorithms", TJson::TArray{OlmAlgorithm, MegolmAlgorithm}},
            {"keys", TJson::TObject{
                {DeviceKeyName("curve25519", DeviceId_), Account_.IdentityKey()},
                {DeviceKeyName("ed25519", DeviceId_), Account_.FingerprintKey()},
            }},
        });
        body["device_keys"] = SignJson(deviceKeys, UserId_, DeviceId_, Account_);
    }
    Account_.GenerateOneTimeKeys(newOneTimeKeys);
    TJson::TObject oneTime;
    for (const auto& [keyId, key] : Account_.UnpublishedOneTimeKeys()) {
        oneTime["signed_curve25519:" + keyId] = SignJson(TJson(TJson::TObject{{"key", key}}), UserId_, DeviceId_, Account_);
    }
    if (!oneTime.empty()) {
        body["one_time_keys"] = TJson(std::move(oneTime));
    }
    return TJson(std::move(body));
}

bool TMachine::PrepareRoomKey(const std::string& roomId, const std::vector<TDevice>& devices, const TRotation& rotation,
    std::int64_t nowMs)
{
    const auto outbound = Outbound.find(roomId);
    bool rotate = outbound == Outbound.end();
    if (!rotate) {
        // A session saved before creation times were kept counts from now
        const std::int64_t created = OutboundCreatedMs.try_emplace(roomId, nowMs).first->second;
        rotate = nowMs - created >= rotation.PeriodMs ||
            static_cast<std::int64_t>(outbound->second.MessageIndex()) >= rotation.PeriodMsgs;
    }
    if (!rotate) {
        std::set<std::string> current;
        for (const auto& device : devices) {
            current.insert(device.UserId + "|" + device.DeviceId);
        }
        for (const auto& device : SharedWith[roomId]) {
            if (!current.count(device)) {
                rotate = true;
                break;
            }
        }
    }
    if (!rotate) {
        return false;
    }
    // Our inbound copy of the old session stays, so the messages already sent can still be read
    Outbound.insert_or_assign(roomId, NMegolm::TOutboundSession());
    OutboundCreatedMs[roomId] = nowMs;
    SharedWith.erase(roomId);
    return true;
}

TToDevice TMachine::ShareRoomKey(const std::string& roomId, const std::vector<TDevice>& devices, const TJson& claimResponse) {
    auto outbound = Outbound.try_emplace(roomId).first;
    const NMegolm::TOutboundSession& session = outbound->second;
    // Keep our own copy too, so we can read our messages coming back in the timeline
    const auto key = std::make_pair(roomId, session.SessionId());
    if (!Inbound.count(key)) {
        if (auto inbound = NMegolm::TInboundSession::FromSessionKey(session.SessionKey())) {
            Inbound.emplace(key, std::move(*inbound));
        }
    }

    TToDevice messages;
    auto& shared = SharedWith[roomId];
    for (const auto& device : devices) {
        if ((device.UserId == UserId_ && device.DeviceId == DeviceId_) || shared.count(device.UserId + "|" + device.DeviceId)) {
            continue;
        }
        // Use an existing Olm session with the device, or start one with a claimed, signed one-time key
        auto& sessions = OlmSessions[device.Curve25519];
        if (sessions.empty()) {
            const TJson& claimed = claimResponse["one_time_keys"][device.UserId][device.DeviceId];
            if (!claimed.IsObject()) {
                continue;
            }
            std::string oneTimeKey;
            for (const auto& [keyId, value] : claimed.AsObject()) {
                if (keyId.rfind("signed_curve25519:", 0) == 0 && VerifyJson(value, device.UserId, device.DeviceId, device.Ed25519)) {
                    oneTimeKey = value.StringOr("key");
                    break;
                }
            }
            if (oneTimeKey.empty()) {
                continue;
            }
            sessions.insert(sessions.begin(), NOlm::TSession::CreateOutbound(Account_, device.Curve25519, oneTimeKey));
        }
        // The plaintext names sender and recipient keys, so a relayed copy cannot be passed off as from someone else
        const TJson payload(TJson::TObject{
            {"type", "m.room_key"},
            {"content", TJson::TObject{
                {"algorithm", MegolmAlgorithm},
                {"room_id", roomId},
                {"session_id", session.SessionId()},
                {"session_key", session.SessionKey()},
            }},
            {"sender", UserId_},
            {"sender_device", DeviceId_},
            {"keys", TJson::TObject{{"ed25519", Account_.FingerprintKey()}}},
            {"recipient", device.UserId},
            {"recipient_keys", TJson::TObject{{"ed25519", device.Ed25519}}},
        });
        const NOlm::TOlmMessage encrypted = sessions.front().Encrypt(payload.Dump());
        messages[device.UserId][device.DeviceId] = TJson(TJson::TObject{
            {"algorithm", OlmAlgorithm},
            {"sender_key", Account_.IdentityKey()},
            {"ciphertext", TJson::TObject{
                {device.Curve25519, TJson::TObject{{"type", static_cast<std::int64_t>(encrypted.Type)}, {"body", encrypted.Body}}},
            }},
        });
        shared.insert(device.UserId + "|" + device.DeviceId);
    }
    return messages;
}

TJson TMachine::ClaimRequestFor(const std::string& roomId, const std::vector<TDevice>& devices) const {
    const auto sharedIt = SharedWith.find(roomId);
    TJson::TObject users;
    for (const auto& device : devices) {
        if ((device.UserId == UserId_ && device.DeviceId == DeviceId_) ||
            (sharedIt != SharedWith.end() && sharedIt->second.count(device.UserId + "|" + device.DeviceId))) {
            continue;
        }
        const auto sessions = OlmSessions.find(device.Curve25519);
        if (sessions != OlmSessions.end() && !sessions->second.empty()) {
            continue;
        }
        TJson::TObject perDevice = users[device.UserId].IsObject() ? users[device.UserId].AsObject() : TJson::TObject{};
        perDevice[device.DeviceId] = "signed_curve25519";
        users[device.UserId] = TJson(std::move(perDevice));
    }
    return TJson(std::move(users));
}

void TMachine::MarkVerified(const TDevice& device) {
    Verified[device.UserId + "|" + device.DeviceId] = device.Ed25519;
}

bool TMachine::IsVerified(const TDevice& device) const {
    const auto it = Verified.find(device.UserId + "|" + device.DeviceId);
    return it != Verified.end() && it->second == device.Ed25519;
}

void TMachine::TrustMasterKey(const std::string& userId, const std::string& masterKey) {
    TrustedMasters[userId] = masterKey;
}

bool TMachine::IsMasterKeyTrusted(const std::string& userId, const std::string& masterKey) const {
    const auto it = TrustedMasters.find(userId);
    return !masterKey.empty() && it != TrustedMasters.end() && it->second == masterKey;
}

bool TMachine::IsTrusted(const TDevice& device) const {
    return IsVerified(device) || IsMasterKeyTrusted(device.UserId, device.CrossSignedBy);
}

TJson TMachine::Save() const {
    TJson::TObject olm;
    for (const auto& [key, sessions] : OlmSessions) {
        TJson::TArray saved;
        for (const auto& session : sessions) {
            saved.push_back(session.Save());
        }
        olm[key] = TJson(std::move(saved));
    }
    TJson::TObject outbound;
    for (const auto& [room, session] : Outbound) {
        TJson::TArray shared;
        if (const auto it = SharedWith.find(room); it != SharedWith.end()) {
            shared.assign(it->second.begin(), it->second.end());
        }
        TJson::TObject saved{{"session", session.Save()}, {"shared_with", TJson(std::move(shared))}};
        if (const auto it = OutboundCreatedMs.find(room); it != OutboundCreatedMs.end()) {
            saved["created_ms"] = it->second;
        }
        outbound[room] = TJson(std::move(saved));
    }
    TJson::TArray inbound;
    for (const auto& [id, session] : Inbound) {
        inbound.push_back(TJson::TObject{{"room_id", id.first}, {"session", session.Save()}});
    }
    return TJson(TJson::TObject{
        {"user_id", UserId_},
        {"device_id", DeviceId_},
        {"account", Account_.Save()},
        {"olm", TJson(std::move(olm))},
        {"outbound", TJson(std::move(outbound))},
        {"inbound", TJson(std::move(inbound))},
        {"verified", TJson(TJson::TObject(Verified.begin(), Verified.end()))},
        {"trusted_masters", TJson(TJson::TObject(TrustedMasters.begin(), TrustedMasters.end()))},
    });
}

std::optional<TMachine> TMachine::Load(const TJson& saved) {
    auto account = NOlm::TAccount::Load(saved["account"]);
    if (!account || !saved["user_id"].IsString() || !saved["device_id"].IsString()) {
        return std::nullopt;
    }
    TMachine machine(saved["user_id"].AsString(), saved["device_id"].AsString(), std::move(*account));
    if (saved["olm"].IsObject()) {
        for (const auto& [key, sessions] : saved["olm"].AsObject()) {
            if (!sessions.IsArray()) {
                continue;
            }
            for (const auto& json : sessions.AsArray()) {
                if (auto session = NOlm::TSession::Load(json)) {
                    machine.OlmSessions[key].push_back(std::move(*session));
                }
            }
        }
    }
    if (saved["outbound"].IsObject()) {
        for (const auto& [room, json] : saved["outbound"].AsObject()) {
            if (auto session = NMegolm::TOutboundSession::Load(json["session"])) {
                machine.Outbound.insert_or_assign(room, std::move(*session));
                if (json["created_ms"].IsInt()) {
                    machine.OutboundCreatedMs[room] = json["created_ms"].AsInt();
                }
                if (json["shared_with"].IsArray()) {
                    for (const auto& device : json["shared_with"].AsArray()) {
                        if (device.IsString()) {
                            machine.SharedWith[room].insert(device.AsString());
                        }
                    }
                }
            }
        }
    }
    if (saved["inbound"].IsArray()) {
        for (const auto& json : saved["inbound"].AsArray()) {
            if (auto session = NMegolm::TInboundSession::Load(json["session"])) {
                machine.Inbound.insert_or_assign(std::make_pair(json.StringOr("room_id"), session->SessionId()), std::move(*session));
            }
        }
    }
    if (saved["verified"].IsObject()) {
        for (const auto& [device, key] : saved["verified"].AsObject()) {
            if (key.IsString()) {
                machine.Verified[device] = key.AsString();
            }
        }
    }
    if (saved["trusted_masters"].IsObject()) {
        for (const auto& [user, key] : saved["trusted_masters"].AsObject()) {
            if (key.IsString()) {
                machine.TrustedMasters[user] = key.AsString();
            }
        }
    }
    return machine;
}

TJson TMachine::EncryptRoomEvent(const std::string& roomId, const std::string& type, const TJson& content) {
    auto& session = Outbound.try_emplace(roomId).first->second;
    const TJson payload(TJson::TObject{{"type", type}, {"content", content}, {"room_id", roomId}});
    return TJson(TJson::TObject{
        {"algorithm", MegolmAlgorithm},
        {"ciphertext", session.Encrypt(payload.Dump())},
        {"session_id", session.SessionId()},
        {"sender_key", Account_.IdentityKey()},  // deprecated, but older clients still read it
        {"device_id", DeviceId_},
    });
}

std::optional<std::string> TMachine::DecryptOlm(const std::string& senderKey, const NOlm::TOlmMessage& message) {
    auto& sessions = OlmSessions[senderKey];
    for (auto& session : sessions) {
        if (message.Type == 0 && !session.MatchesInbound(message.Body)) {
            continue;
        }
        if (auto plaintext = session.Decrypt(message)) {
            return plaintext;
        }
    }
    // A pre-key message no session knows starts a new one
    if (message.Type == 0) {
        if (auto inbound = NOlm::TSession::CreateInbound(Account_, message.Body)) {
            sessions.insert(sessions.begin(), std::move(inbound->Session));
            return inbound->Plaintext;
        }
    }
    return std::nullopt;
}

bool TMachine::ReceiveToDevice(const TJson& event) {
    if (event.StringOr("type") != "m.room.encrypted") {
        return false;
    }
    const TJson& content = event["content"];
    if (content.StringOr("algorithm") != OlmAlgorithm) {
        return false;
    }
    const std::string senderKey = content.StringOr("sender_key");
    const TJson& mine = content["ciphertext"][Account_.IdentityKey()];
    if (senderKey.empty() || !mine["type"].IsInt() || !mine["body"].IsString()) {
        return false;
    }
    const auto plaintext = DecryptOlm(senderKey, {static_cast<int>(mine["type"].AsInt()), mine["body"].AsString()});
    if (!plaintext) {
        return false;
    }
    TJson payload;
    try {
        payload = TJson::Parse(*plaintext);
    } catch (const std::runtime_error&) {
        return false;
    }
    // The claims inside the encrypted payload must agree with the event and with us
    if (payload.StringOr("sender") != event.StringOr("sender") || payload.StringOr("recipient") != UserId_ ||
        payload["recipient_keys"].StringOr("ed25519") != Account_.FingerprintKey()) {
        return false;
    }
    if (payload.StringOr("type") != "m.room_key") {
        return false;
    }
    const TJson& key = payload["content"];
    if (key.StringOr("algorithm") != MegolmAlgorithm) {
        return false;
    }
    auto inbound = NMegolm::TInboundSession::FromSessionKey(key.StringOr("session_key"));
    if (!inbound || inbound->SessionId() != key.StringOr("session_id")) {
        return false;
    }
    const auto id = std::make_pair(key.StringOr("room_id"), inbound->SessionId());
    const auto existing = Inbound.find(id);
    // Keep the copy that starts earlier: it can read more of the history
    if (existing == Inbound.end() || inbound->FirstKnownIndex() < existing->second.FirstKnownIndex()) {
        Inbound.insert_or_assign(id, std::move(*inbound));
    }
    return true;
}

std::optional<TJson> TMachine::DecryptRoomEvent(const std::string& roomId, const TJson& event) const {
    const TJson& content = event["content"];
    if (event.StringOr("type") != "m.room.encrypted" || content.StringOr("algorithm") != MegolmAlgorithm) {
        return std::nullopt;
    }
    const auto session = Inbound.find({roomId, content.StringOr("session_id")});
    if (session == Inbound.end()) {
        return std::nullopt;  // the key has not arrived (yet)
    }
    const auto decrypted = session->second.Decrypt(content.StringOr("ciphertext"));
    if (!decrypted) {
        return std::nullopt;
    }
    TJson payload;
    try {
        payload = TJson::Parse(decrypted->Plaintext);
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
    // A message decrypted with this room's key must also say it belongs to this room
    if (payload.StringOr("room_id") != roomId || !payload["content"].IsObject()) {
        return std::nullopt;
    }
    return payload;
}

bool DecryptWaiting(TSyncState& state, const TMachine& machine) {
    bool changed = false;
    for (auto& [roomId, room] : state.Rooms) {
        for (auto& message : room.Timeline) {
            if (!message.IsWaitingForKey()) {
                continue;
            }
            std::optional<TJson> payload;
            try {
                payload = machine.DecryptRoomEvent(roomId, TJson::Parse(message.EncryptedEvent));
            } catch (const std::runtime_error&) {
            }
            if (!payload) {
                continue;
            }
            const TJson& content = (*payload)["content"];
            if (payload->StringOr("type") == "m.room.message" && content["body"].IsString()) {
                message.MsgType = content.StringOr("msgtype", "m.text");
                message.Body = content["body"].AsString();
                message.FormattedBody = FormattedBodyOf(content);
            } else {
                message.MsgType = payload->StringOr("type", "unknown");  // removed below
            }
            changed = true;
        }
        std::erase_if(room.Timeline, [](const TMessage& message) {
            return !message.EncryptedEvent.empty() && !message.MsgType.empty() && message.Body.empty();
        });
    }
    return changed;
}

} // namespace NHoshi::NE2ee
