#pragma once

#include "json.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace NHoshi {

// A message from a room timeline, the part of an m.room.message event that the client shows
struct TMessage {
    enum class ESendState {
        Sent,     // came from the server
        Sending,  // written here, not confirmed by /sync yet
        Failed,   // the send request failed; can be retried with the same TxnId
    };

    std::string EventId;  // empty for a message still being sent
    std::string Sender;
    std::string MsgType;  // m.text, m.notice, m.emote, m.image, ...
    std::string Body;
    std::int64_t OriginServerTs = 0;
    std::string TxnId;  // for messages sent from this device: the transaction id of the send request
    ESendState SendState = ESendState::Sent;
    // For an m.room.encrypted event: the event as received. MsgType and Body stay empty until it is decrypted.
    std::string EncryptedEvent;
    bool IsWaitingForKey() const { return !EncryptedEvent.empty() && MsgType.empty(); }
};

struct TRoom {
    std::string Id;
    std::string Name;      // from m.room.name, empty if the room has none
    bool Encrypted = false;  // m.room.encryption was seen; the timeline then has m.room.encrypted events
    std::vector<TMessage> Timeline;
    bool Limited = false;    // the server skipped older events; prev_batch fetches them
    std::string PrevBatch;   // token for /messages?dir=b to fetch what comes before Timeline.front()
    bool HistoryComplete = false;  // /messages reached the start of the room
    // Messages written on this device that /sync has not returned yet, shown after the timeline in order
    std::vector<TMessage> Pending;
    // Joined and invited users, from m.room.member; in an encrypted room their devices get the room key
    std::set<std::string> Members;
    std::map<std::string, std::string> MemberNames;  // user id -> display name, when the member set one
    std::string CanonicalAlias;
    std::vector<std::string> Heroes;  // from the /sync room summary: the members to name an unnamed room after

    // The name to show, as the spec's "Calculating the display name for a room": the room name, else its
    // canonical alias, else the heroes or other members, e.g. a direct chat is called after the other person
    std::string DisplayName(const std::string& ownUserId) const;
};

// A room the user is invited to, from the stripped state the invite carries
struct TInvite {
    std::string RoomId;
    std::string Name;
    std::string Inviter;
    bool Encrypted = false;
};

// The part of a /sync response the client keeps. Rooms are keyed by room id.
struct TSyncState {
    std::string NextBatch;
    std::map<std::string, TRoom> Rooms;
    std::map<std::string, TInvite> Invites;  // an invite goes away once the room is joined or left

    // Applies one /sync response: drops left rooms, updates names and encryption from state, appends new
    // timeline messages.
    // Events that are not messages and messages without a body are skipped; their shape is not trusted.
    void Apply(const TJson& response);

    // Applies a /messages?dir=b response for the room: older messages go before the timeline, and the
    // next PrevBatch is its "end". No "end" or an empty chunk means the start of the room was reached.
    void PrependHistory(const std::string& roomId, const TJson& response);

    // Local echo. A pending message leaves Pending when /sync returns the event, matched by
    // unsigned.transaction_id or by the event id the send request returned.
    void AddPending(const std::string& roomId, const std::string& txnId, const std::string& sender, const std::string& body,
        std::int64_t nowMs);
    void MarkPendingSent(const std::string& roomId, const std::string& txnId, const std::string& eventId);
    void MarkPendingFailed(const std::string& roomId, const std::string& txnId);
    // Returns the body to send again and marks the message as sending; nullptr if there is no such failed message
    const TMessage* RetryPending(const std::string& roomId, const std::string& txnId);
};

} // namespace NHoshi
