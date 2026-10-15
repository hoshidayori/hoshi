#pragma once

// End-to-end encryption on top of Olm and Megolm: device keys, key sharing over to-device messages and
// m.room.encrypted events, as in the Matrix client-server API ("End-to-end encryption" module).

#include "json.h"
#include "megolm.h"
#include "olm.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace NHoshi::NE2ee {

inline constexpr const char* OlmAlgorithm = "m.olm.v1.curve25519-aes-sha2";
inline constexpr const char* MegolmAlgorithm = "m.megolm.v1.aes-sha2";

// Signs a JSON object as Matrix does: Ed25519 over the canonical JSON without "signatures" and "unsigned",
// stored under signatures[userId]["ed25519:" + deviceId]
TJson SignJson(const TJson& object, const std::string& userId, const std::string& deviceId, const NOlm::TAccount& account);
bool VerifyJson(const TJson& object, const std::string& userId, const std::string& deviceId, const std::string& ed25519Key);

// A device of another user, from /keys/query, after its self-signature was checked
struct TDevice {
    std::string UserId;
    std::string DeviceId;
    std::string Curve25519;
    std::string Ed25519;
};

// Devices with valid self-signatures from a /keys/query response; others are dropped
std::vector<TDevice> DevicesFromQuery(const TJson& response);

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

    // Olm-encrypted m.room_key for each device, using one-time keys from a /keys/claim response.
    // Devices without a valid signed one-time key are skipped. Creates the room's outbound session if needed.
    TToDevice ShareRoomKey(const std::string& roomId, const std::vector<TDevice>& devices, const TJson& claimResponse);
    // The content of an m.room.encrypted event for the room's current outbound session
    TJson EncryptRoomEvent(const std::string& roomId, const std::string& type, const TJson& content);

    // Handles one to-device event from /sync; returns true if it delivered a room key
    bool ReceiveToDevice(const TJson& event);
    // Decrypts an m.room.encrypted timeline event: the plaintext event {"type", "content", "room_id"}, or nullopt
    std::optional<TJson> DecryptRoomEvent(const std::string& roomId, const TJson& event) const;

private:
    std::optional<std::string> DecryptOlm(const std::string& senderKey, const NOlm::TOlmMessage& message);

    std::string UserId_;
    std::string DeviceId_;
    NOlm::TAccount Account_;
    // Olm sessions by the other device's Curve25519 key, newest first
    std::map<std::string, std::vector<NOlm::TSession>> OlmSessions;
    // Megolm sessions for sending, by room
    std::map<std::string, NMegolm::TOutboundSession> Outbound;
    // Megolm sessions for receiving, by (room, session id)
    std::map<std::pair<std::string, std::string>, NMegolm::TInboundSession> Inbound;
};

} // namespace NHoshi::NE2ee
