#include "verification.h"

#include <algorithm>
#include <utility>

namespace NHoshi::NVerification {

namespace {

constexpr const char* SasMethod = "m.sas.v1";
constexpr const char* KeyAgreement = "curve25519-hkdf-sha256";
constexpr const char* Hash = "sha256";
constexpr const char* MacMethod = "hkdf-hmac-sha256.v2";

constexpr std::int64_t MaxAgeMs = 10 * 60 * 1000;
constexpr std::int64_t MaxFutureMs = 5 * 60 * 1000;

bool Contains(const TJson& list, const std::string& value) {
    if (!list.IsArray()) {
        return false;
    }
    return std::any_of(list.AsArray().begin(), list.AsArray().end(),
        [&](const TJson& item) { return item.IsString() && item.AsString() == value; });
}

std::string Commit(const std::string& key, const TJson& start) {
    return NCrypto::Base64Encode(NCrypto::ToBytes(NCrypto::Sha256(NCrypto::ToBytes(key + start.Dump()))));
}

} // namespace

TVerification::TVerification(const TIdentity& us, const NE2ee::TDevice& them, std::string transactionId, EState state)
    : Us(us)
    , Them_(them)
    , TransactionId_(std::move(transactionId))
    , State_(state)
{}

std::optional<TVerification> TVerification::FromRequest(const TIdentity& us, const NE2ee::TDevice& them,
    const TJson& content, std::int64_t nowMs)
{
    const std::string transactionId = content.StringOr("transaction_id");
    if (transactionId.empty() || content.StringOr("from_device") != them.DeviceId || !content["timestamp"].IsInt()) {
        return std::nullopt;
    }
    const std::int64_t timestamp = content["timestamp"].AsInt();
    if (nowMs - timestamp > MaxAgeMs || timestamp - nowMs > MaxFutureMs) {
        return std::nullopt;
    }
    TVerification verification(us, them, transactionId, EState::Requested);
    if (!Contains(content["methods"], SasMethod)) {
        verification.Cancel("m.unknown_method", "Only emoji verification (m.sas.v1) is supported");
    }
    return verification;
}

TVerification TVerification::Request(const TIdentity& us, const NE2ee::TDevice& them, std::string transactionId,
    std::int64_t nowMs)
{
    TVerification verification(us, them, std::move(transactionId), EState::WaitingForReady);
    verification.Send("m.key.verification.request", {
        {"from_device", us.DeviceId},
        {"methods", TJson::TArray{SasMethod}},
        {"timestamp", nowMs},
    });
    return verification;
}

void TVerification::Send(const std::string& type, TJson::TObject content) {
    content["transaction_id"] = TransactionId_;
    Outgoing.push_back({type, TJson(std::move(content))});
}

std::vector<TOutgoing> TVerification::TakeOutgoing() {
    return std::exchange(Outgoing, {});
}

void TVerification::Cancel(const std::string& code, const std::string& reason) {
    if (State_ == EState::Done || State_ == EState::Cancelled) {
        return;
    }
    Send("m.key.verification.cancel", {{"code", code}, {"reason", reason}});
    State_ = EState::Cancelled;
    CancelCode_ = code;
    CancelReason_ = reason;
}

void TVerification::AcceptRequest() {
    if (State_ != EState::Requested) {
        return;
    }
    Send("m.key.verification.ready", {{"from_device", Us.DeviceId}, {"methods", TJson::TArray{SasMethod}}});
    State_ = EState::Ready;
}

TJson TVerification::StartContent() const {
    return TJson(TJson::TObject{
        {"from_device", Us.DeviceId},
        {"method", SasMethod},
        {"key_agreement_protocols", TJson::TArray{KeyAgreement}},
        {"hashes", TJson::TArray{Hash}},
        {"message_authentication_codes", TJson::TArray{MacMethod}},
        {"short_authentication_string", TJson::TArray{"decimal", "emoji"}},
        {"transaction_id", TransactionId_},
    });
}

void TVerification::StartSas() {
    if (State_ != EState::Ready) {
        return;
    }
    Start = StartContent();
    WeStarted = true;
    Outgoing.push_back({"m.key.verification.start", Start});
    State_ = EState::WaitingForAccept;
}

void TVerification::Receive(const std::string& type, const TJson& content, const std::string& sender) {
    // Another transaction, or someone else: not ours to answer
    if (sender != Them_.UserId || content.StringOr("transaction_id") != TransactionId_) {
        return;
    }
    if (State_ == EState::Done || State_ == EState::Cancelled) {
        return;
    }
    if (type == "m.key.verification.cancel") {
        State_ = EState::Cancelled;
        CancelCode_ = content.StringOr("code", "m.user");
        CancelReason_ = content.StringOr("reason");
    } else if (type == "m.key.verification.ready" && State_ == EState::WaitingForReady) {
        OnReady(content);
    } else if (type == "m.key.verification.start" && (State_ == EState::Ready || State_ == EState::WaitingForAccept)) {
        OnStart(content);
    } else if (type == "m.key.verification.accept" && State_ == EState::WaitingForAccept) {
        OnAccept(content);
    } else if (type == "m.key.verification.key" && State_ == EState::WaitingForKey && TheirKey.empty()) {
        OnKey(content);
    } else if (type == "m.key.verification.mac" && (State_ == EState::ShowingSas || State_ == EState::WaitingForMac) &&
        !TheirMac)
    {
        OnMac(content);
    } else if (type == "m.key.verification.done" && State_ == EState::WaitingForDone) {
        State_ = EState::Done;
    } else if (type == "m.key.verification.done" && State_ == EState::WaitingForMac) {
        // Their done can overtake their MAC only if they are broken; the MAC check decides
    } else {
        Cancel("m.unexpected_message", "Unexpected " + type);
    }
}

void TVerification::OnReady(const TJson& content) {
    if (content.StringOr("from_device") != Them_.DeviceId) {
        Cancel("m.invalid_message", "ready from another device");
        return;
    }
    if (!Contains(content["methods"], SasMethod)) {
        Cancel("m.unknown_method", "Only emoji verification (m.sas.v1) is supported");
        return;
    }
    State_ = EState::Ready;
}

void TVerification::OnStart(const TJson& content) {
    if (State_ == EState::WaitingForAccept) {
        // Both sides started at once: the start of the lower user id, then device id, goes on (the spec's rule)
        const bool theirsWins = Them_.UserId != Us.UserId ? Them_.UserId < Us.UserId : Them_.DeviceId < Us.DeviceId;
        if (!theirsWins) {
            return;
        }
        WeStarted = false;
    }
    if (content.StringOr("from_device") != Them_.DeviceId) {
        Cancel("m.invalid_message", "start from another device");
        return;
    }
    if (content.StringOr("method") != SasMethod || !Contains(content["key_agreement_protocols"], KeyAgreement) ||
        !Contains(content["hashes"], Hash) || !Contains(content["message_authentication_codes"], MacMethod) ||
        !Contains(content["short_authentication_string"], "emoji"))
    {
        Cancel("m.unknown_method", "No common SAS methods");
        return;
    }
    Start = content;
    Send("m.key.verification.accept", {
        {"method", SasMethod},
        {"key_agreement_protocol", KeyAgreement},
        {"hash", Hash},
        {"message_authentication_code", MacMethod},
        {"short_authentication_string", TJson::TArray{"decimal", "emoji"}},
        {"commitment", Commit(Sas.PublicKey(), Start)},
    });
    State_ = EState::WaitingForKey;
}

void TVerification::OnAccept(const TJson& content) {
    if (content.StringOr("key_agreement_protocol") != KeyAgreement || content.StringOr("hash") != Hash ||
        content.StringOr("message_authentication_code") != MacMethod || !Contains(content["short_authentication_string"], "emoji"))
    {
        Cancel("m.unknown_method", "The accepted methods were not offered");
        return;
    }
    Commitment = content.StringOr("commitment");
    if (Commitment.empty()) {
        Cancel("m.invalid_message", "accept without a commitment");
        return;
    }
    // The starter shows its key first; the other side is already bound to its key by the commitment
    Send("m.key.verification.key", {{"key", Sas.PublicKey()}});
    State_ = EState::WaitingForKey;
}

void TVerification::OnKey(const TJson& content) {
    const std::string key = content.StringOr("key");
    if (WeStarted && Commit(key, Start) != Commitment) {
        Cancel("m.mismatched_commitment", "The key does not match the commitment");
        return;
    }
    if (!Sas.SetTheirKey(key)) {
        Cancel("m.invalid_message", "Invalid key");
        return;
    }
    TheirKey = key;
    if (!WeStarted) {
        Send("m.key.verification.key", {{"key", Sas.PublicKey()}});
    }
    SasBytes = Sas.Bytes(SasInfo(), 6);
    State_ = EState::ShowingSas;
}

std::string TVerification::SasInfo() const {
    // The starter's user, device and key first, then the other side's
    const std::string ourPart = Us.UserId + "|" + Us.DeviceId + "|" + Sas.PublicKey();
    const std::string theirPart = Them_.UserId + "|" + Them_.DeviceId + "|" + TheirKey;
    return std::string("MATRIX_KEY_VERIFICATION_SAS|") + (WeStarted ? ourPart + "|" + theirPart : theirPart + "|" + ourPart) +
        "|" + TransactionId_;
}

std::string TVerification::MacInfo(const std::string& fromUser, const std::string& fromDevice, const std::string& toUser,
    const std::string& toDevice, const std::string& keyId) const
{
    return "MATRIX_KEY_VERIFICATION_MAC" + fromUser + fromDevice + toUser + toDevice + TransactionId_ + keyId;
}

std::array<NSas::TEmoji, 7> TVerification::Emoji() const {
    return NSas::EmojiFromBytes(SasBytes);
}

std::array<int, 3> TVerification::Decimals() const {
    return NSas::DecimalsFromBytes(SasBytes);
}

void TVerification::ConfirmSas() {
    if (State_ != EState::ShowingSas) {
        return;
    }
    Confirmed = true;
    // Our device key, MACed so that the other side can check it is the key it has for us
    const std::string keyId = "ed25519:" + Us.DeviceId;
    Send("m.key.verification.mac", {
        {"mac", TJson::TObject{{keyId, Sas.Mac(Us.Ed25519, MacInfo(Us.UserId, Us.DeviceId, Them_.UserId, Them_.DeviceId, keyId))}}},
        {"keys", Sas.Mac(keyId, MacInfo(Us.UserId, Us.DeviceId, Them_.UserId, Them_.DeviceId, "KEY_IDS"))},
    });
    State_ = EState::WaitingForMac;
    if (TheirMac) {
        CheckMac();
    }
}

void TVerification::OnMac(const TJson& content) {
    TheirMac = content;
    // Before our user has compared the emoji, a correct MAC proves nothing to us yet
    if (Confirmed) {
        CheckMac();
    }
}

void TVerification::CheckMac() {
    const TJson& macs = (*TheirMac)["mac"];
    if (!macs.IsObject() || macs.AsObject().empty()) {
        Cancel("m.invalid_message", "mac without keys");
        return;
    }
    std::vector<std::string> keyIds;
    for (const auto& [keyId, mac] : macs.AsObject()) {
        keyIds.push_back(keyId);
    }
    std::sort(keyIds.begin(), keyIds.end());
    std::string joined;
    for (const auto& keyId : keyIds) {
        joined += (joined.empty() ? "" : ",") + keyId;
    }
    // The list of keys is MACed too, so that no one can drop a key from the message
    if ((*TheirMac).StringOr("keys") != Sas.Mac(joined, MacInfo(Them_.UserId, Them_.DeviceId, Us.UserId, Us.DeviceId, "KEY_IDS"))) {
        Cancel("m.key_mismatch", "The key list does not match");
        return;
    }
    const std::string deviceKeyId = "ed25519:" + Them_.DeviceId;
    bool deviceKeyChecked = false;
    for (const auto& [keyId, mac] : macs.AsObject()) {
        // Keys we do not hold yet, such as a cross-signing master key, cannot be checked, so they prove nothing
        if (keyId != deviceKeyId) {
            continue;
        }
        if (!mac.IsString() || mac.AsString() != Sas.Mac(Them_.Ed25519, MacInfo(Them_.UserId, Them_.DeviceId, Us.UserId, Us.DeviceId, keyId))) {
            Cancel("m.key_mismatch", "The device key does not match");
            return;
        }
        deviceKeyChecked = true;
    }
    if (!deviceKeyChecked) {
        Cancel("m.key_mismatch", "The device key is missing");
        return;
    }
    Send("m.key.verification.done", {});
    State_ = EState::WaitingForDone;
}

} // namespace NHoshi::NVerification
