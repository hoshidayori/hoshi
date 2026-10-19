#pragma once

// End-to-end encryption on top of Olm and Megolm: device keys, key sharing over to-device messages and
// m.room.encrypted events, as in the Matrix client-server API ("End-to-end encryption" module).

#include "json.h"
#include "megolm.h"
#include "olm.h"
#include "sync.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace NHoshi::NE2ee {

inline constexpr const char* OlmAlgorithm = "m.olm.v1.curve25519-aes-sha2";
inline constexpr const char* MegolmAlgorithm = "m.megolm.v1.aes-sha2";

// Signs a JSON object as Matrix does: Ed25519 over the canonical JSON without "signatures" and "unsigned",
// stored under signatures[userId]["ed25519:" + deviceId]
TJson SignJson(const TJson& object, const std::string& userId, const std::string& deviceId, const NOlm::TAccount& account);
// The same with a cross-signing key, named after its public key: signatures[userId]["ed25519:" + public key]
TJson SignJsonWithKey(const TJson& object, const std::string& userId, const NCrypto::TEd25519KeyPair& key);
bool VerifyJson(const TJson& object, const std::string& userId, const std::string& deviceId, const std::string& ed25519Key);

// A user's cross-signing keys from /keys/query (base64 Ed25519 public keys), as in the spec's "Cross-signing"
// section. The self-signing and user-signing keys count only if the master key signed them; otherwise they are
// empty. The user-signing key is returned only for our own user.
struct TCrossSigning {
    std::string Master;
    std::string SelfSigning;
    std::string UserSigning;
};
std::map<std::string, TCrossSigning> CrossSigningFromQuery(const TJson& response);

// A device of another user, from /keys/query, after its self-signature was checked
struct TDevice {
    std::string UserId;
    std::string DeviceId;
    std::string Curve25519;
    std::string Ed25519;
    // The master key under which the device is cross-signed: its user's self-signing key, signed by this master
    // key, signed the device's keys. Empty if the device is not cross-signed.
    std::string CrossSignedBy = {};
};

// Devices with valid self-signatures from a /keys/query response; others are dropped. Cross-signing is checked
// against the keys of the same response.
std::vector<TDevice> DevicesFromQuery(const TJson& response);

// When a room's outbound Megolm session is replaced, from m.room.encryption; the defaults are the spec's
struct TRotation {
    std::int64_t PeriodMs = 7 * 24 * 60 * 60 * 1000LL;  // rotation_period_ms
    std::int64_t PeriodMsgs = 100;                     // rotation_period_msgs
};

// To-device messages for /sendToDevice: user -> device -> content
using TToDevice = std::map<std::string, std::map<std::string, TJson>>;

class TMachine {
public:
    TMachine(std::string userId, std::string deviceId, NOlm::TAccount account);

    const std::string& UserId() const { return UserId_; }
    const std::string& DeviceId() const { return DeviceId_; }
    const NOlm::TAccount& Account() const { return Account_; }

    // The body for /keys/upload: signed device keys (first time) and new signed one-time keys
    TJson KeysUploadBody(size_t newOneTimeKeys, bool withDeviceKeys);
    // After the upload succeeded
    void MarkKeysAsPublished() { Account_.MarkKeysAsPublished(); }

    // Before sending: replaces the room's outbound session with a new one when the old one is too old, has
    // encrypted too many messages, or its key went to a device that is not among the room's devices any more
    // (a member left, or a device was removed), so that device cannot read what comes next. Starts the session
    // if the room has none. Returns true if a new session was started.
    bool PrepareRoomKey(const std::string& roomId, const std::vector<TDevice>& devices, const TRotation& rotation,
        std::int64_t nowMs);
    // Olm-encrypted m.room_key for each device, using one-time keys from a /keys/claim response.
    // Devices without a valid signed one-time key are skipped. Creates the room's outbound session if needed.
    // Devices that already got the room's current session key are skipped too.
    TToDevice ShareRoomKey(const std::string& roomId, const std::vector<TDevice>& devices, const TJson& claimResponse);
    // Devices of the list that still need the room's current session key and have no Olm session to send it over:
    // the body for /keys/claim (user -> device -> "signed_curve25519"), empty if none
    TJson ClaimRequestFor(const std::string& roomId, const std::vector<TDevice>& devices) const;
    // The content of an m.room.encrypted event for the room's current outbound session
    TJson EncryptRoomEvent(const std::string& roomId, const std::string& type, const TJson& content);

    // Handles one to-device event from /sync; returns true if it delivered a room key
    bool ReceiveToDevice(const TJson& event);
    // Decrypts an m.room.encrypted timeline event: the plaintext event {"type", "content", "room_id"}, or nullopt
    std::optional<TJson> DecryptRoomEvent(const std::string& roomId, const TJson& event) const;

    // Devices whose Ed25519 key a user confirmed by verification. A device counts as verified only while
    // /keys/query returns the same key for it.
    void MarkVerified(const TDevice& device);
    bool IsVerified(const TDevice& device) const;
    // Master keys our user confirmed by verification: the other side's MAC covered the key
    void TrustMasterKey(const std::string& userId, const std::string& masterKey);
    bool IsMasterKeyTrusted(const std::string& userId, const std::string& masterKey) const;
    // Verified directly, or cross-signed under a master key we trust
    bool IsTrusted(const TDevice& device) const;

    // Everything, private keys included: the caller stores it protected (Keychain or a protected file)
    TJson Save() const;
    static std::optional<TMachine> Load(const TJson& saved);

private:
    std::optional<std::string> DecryptOlm(const std::string& senderKey, const NOlm::TOlmMessage& message);

    std::string UserId_;
    std::string DeviceId_;
    NOlm::TAccount Account_;
    // Olm sessions by the other device's Curve25519 key, newest first
    std::map<std::string, std::vector<NOlm::TSession>> OlmSessions;
    // Megolm sessions for sending, by room, and the devices ("user|device") that got each one's key
    std::map<std::string, NMegolm::TOutboundSession> Outbound;
    std::map<std::string, std::set<std::string>> SharedWith;
    std::map<std::string, std::int64_t> OutboundCreatedMs;
    std::map<std::string, std::string> Verified;  // "user|device" -> Ed25519 key
    std::map<std::string, std::string> TrustedMasters;  // user -> master key
    // Megolm sessions for receiving, by (room, session id)
    std::map<std::pair<std::string, std::string>, NMegolm::TInboundSession> Inbound;
};

// Decrypts the timeline messages that are waiting for their key. Decrypted events that are not messages
// (reactions and the like) leave the timeline. Returns whether anything changed.
bool DecryptWaiting(TSyncState& state, const TMachine& machine);

} // namespace NHoshi::NE2ee
