#pragma once

// Interactive device verification over to-device messages with m.sas.v1, as in the Matrix client-server API
// ("Key verification framework" and "Short Authentication String (SAS) verification").
//
// One TVerification is one transaction with one device of another user or of our own user. It does no I/O:
// incoming to-device events go to Receive, user decisions to the other methods, and the messages it wants to
// send to the other device are taken with TakeOutgoing.
//
// The flow: request -> ready -> start -> accept -> key, key -> both users compare the emoji -> mac, mac ->
// done, done.
// Whoever sends start is the "starter"; the other side commits to its key in accept before it sees the
// starter's key, so neither can pick a key after seeing the other's to steer the emoji.

#include "e2ee.h"
#include "json.h"
#include "sas.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace NHoshi::NVerification {

// This device
struct TIdentity {
    std::string UserId;
    std::string DeviceId;
    std::string Ed25519;
};

struct TOutgoing {
    std::string Type;  // m.key.verification.*
    TJson Content;
};

enum class EState {
    Requested,         // they asked; our user decides
    WaitingForReady,   // we asked
    Ready,             // both can do SAS; either side may start
    WaitingForAccept,  // we sent start
    WaitingForKey,     // the methods are agreed; keys are being exchanged
    ShowingSas,        // our user compares the emoji with the other screen
    WaitingForMac,     // our user confirmed; the other side has not yet
    WaitingForDone,    // their MAC is right; the other side may still reject ours
    Done,              // the other device's key is verified, and that side verified ours
    Cancelled,
};

class TVerification {
public:
    // A request they sent; nullopt if it is malformed, not from that device, or older than 10 minutes or more
    // than 5 minutes in the future (the spec's limits)
    static std::optional<TVerification> FromRequest(const TIdentity& us, const NE2ee::TDevice& them, const TJson& content,
        std::int64_t nowMs);
    // Our request to the device
    static TVerification Request(const TIdentity& us, const NE2ee::TDevice& them, std::string transactionId,
        std::int64_t nowMs);

    // The cross-signing master keys of both users, from /keys/query, if they have them. Our MAC then covers our
    // master key, so the other side can trust it, and theirs is checked if their MAC covers it.
    void SetMasterKeys(std::string ours, std::string theirs);

    void Receive(const std::string& type, const TJson& content, const std::string& sender);
    void AcceptRequest();  // Requested -> Ready
    void StartSas();       // Ready -> WaitingForAccept
    void ConfirmSas();     // ShowingSas -> WaitingForMac, or Done if their MAC is already here and correct
    // The emoji differ, or the user gives up
    void Cancel(const std::string& code, const std::string& reason);

    std::vector<TOutgoing> TakeOutgoing();

    EState State() const { return State_; }
    const std::string& TransactionId() const { return TransactionId_; }
    const NE2ee::TDevice& Them() const { return Them_; }
    // When ShowingSas or later
    std::array<NSas::TEmoji, 7> Emoji() const;
    std::array<int, 3> Decimals() const;
    // When Cancelled: the m.key.verification.cancel code, ours or theirs
    const std::string& CancelCode() const { return CancelCode_; }
    const std::string& CancelReason() const { return CancelReason_; }
    // Their master key, when their MAC covered it and was right; empty otherwise. Meaningful once Done.
    const std::string& VerifiedMasterKey() const { return VerifiedMasterKey_; }

private:
    TVerification(const TIdentity& us, const NE2ee::TDevice& them, std::string transactionId, EState state);

    void Send(const std::string& type, TJson::TObject content);
    void OnReady(const TJson& content);
    void OnStart(const TJson& content);
    void OnAccept(const TJson& content);
    void OnKey(const TJson& content);
    void OnMac(const TJson& content);
    void CheckMac();
    TJson StartContent() const;
    std::string SasInfo() const;
    std::string MacInfo(const std::string& fromUser, const std::string& fromDevice, const std::string& toUser,
        const std::string& toDevice, const std::string& keyId) const;

    TIdentity Us;
    NE2ee::TDevice Them_;
    std::string TransactionId_;
    EState State_;
    std::vector<TOutgoing> Outgoing;

    bool WeStarted = false;
    TJson Start;                 // the start content as sent or received: the commitment covers it
    std::string Commitment;      // from their accept, when we started
    NSas::TSas Sas;
    std::string TheirKey;
    NCrypto::TBytes SasBytes;
    bool Confirmed = false;
    std::optional<TJson> TheirMac;
    std::string CancelCode_;
    std::string CancelReason_;
    std::string OurMasterKey;
    std::string TheirMasterKey;
    std::string VerifiedMasterKey_;
};

} // namespace NHoshi::NVerification
