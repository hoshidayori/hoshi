#pragma once

#include "json.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace NHoshi {

// A message from a room timeline, the part of an m.room.message event that the client shows
struct TMessage {
    std::string EventId;
    std::string Sender;
    std::string MsgType;  // m.text, m.notice, m.emote, m.image, ...
    std::string Body;
    std::int64_t OriginServerTs = 0;
};

struct TRoom {
    std::string Id;
    std::string Name;      // from m.room.name, empty if the room has none
    bool Encrypted = false;  // m.room.encryption was seen; the timeline then has m.room.encrypted events
    std::vector<TMessage> Timeline;
    bool Limited = false;    // the server skipped older events; prev_batch fetches them
    std::string PrevBatch;   // token for /messages?dir=b to fetch what comes before Timeline.front()
    bool HistoryComplete = false;  // /messages reached the start of the room
};

// The part of a /sync response the client keeps. Rooms are keyed by room id.
struct TSyncState {
    std::string NextBatch;
    std::map<std::string, TRoom> Rooms;

    // Applies one /sync response: drops left rooms, updates names and encryption from state, appends new
    // timeline messages.
    // Events that are not messages and messages without a body are skipped; their shape is not trusted.
    void Apply(const TJson& response);

    // Applies a /messages?dir=b response for the room: older messages go before the timeline, and the
    // next PrevBatch is its "end". No "end" or an empty chunk means the start of the room was reached.
    void PrependHistory(const std::string& roomId, const TJson& response);
};

} // namespace NHoshi
