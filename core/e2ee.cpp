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

} // namespace

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
            devices.push_back({userId, deviceId, curve25519, ed25519});
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
        outbound[room] = TJson(TJson::TObject{{"session", session.Save()}, {"shared_with", TJson(std::move(shared))}});
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
