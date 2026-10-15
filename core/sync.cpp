#include "sync.h"

#include <optional>
#include <unordered_set>

namespace NHoshi {

namespace {

// State events can come both in the state section and in the timeline, so both go through this
void ApplyStateEvent(TRoom& room, const TJson& event) {
    const std::string type = event.StringOr("type");
    if (type == "m.room.name" && event["state_key"].IsString() && event["state_key"].AsString().empty()) {
        room.Name = event["content"].StringOr("name");
    } else if (type == "m.room.encryption") {
        room.Encrypted = true;
    }
}

// The message the client shows for an event, if the event is a message with a body
std::optional<TMessage> ParseMessage(const TJson& event) {
    if (event["state_key"].IsString() || event.StringOr("type") != "m.room.message") {
        return std::nullopt;
    }
    const TJson& content = event["content"];
    if (!content["body"].IsString()) {
        return std::nullopt;  // a redacted message has empty content
    }
    TMessage message;
    message.EventId = event.StringOr("event_id");
    message.Sender = event.StringOr("sender");
    message.MsgType = content.StringOr("msgtype");
    message.Body = content["body"].AsString();
    if (event["origin_server_ts"].IsInt()) {
        message.OriginServerTs = event["origin_server_ts"].AsInt();
    }
    return message;
}

void ApplyTimelineEvent(TRoom& room, const TJson& event) {
    if (event["state_key"].IsString()) {
        ApplyStateEvent(room, event);
        return;
    }
    if (auto message = ParseMessage(event)) {
        room.Timeline.push_back(std::move(*message));
    }
}

} // namespace

void TSyncState::Apply(const TJson& response) {
    if (response["next_batch"].IsString()) {
        NextBatch = response["next_batch"].AsString();
    }
    // Rooms the user left or was removed from are dropped from the list
    const TJson& left = response["rooms"]["leave"];
    if (left.IsObject()) {
        for (const auto& [roomId, data] : left.AsObject()) {
            Rooms.erase(roomId);
        }
    }
    const TJson& joined = response["rooms"]["join"];
    if (!joined.IsObject()) {
        return;
    }
    for (const auto& [roomId, data] : joined.AsObject()) {
        const bool seenBefore = Rooms.count(roomId) != 0;
        TRoom& room = Rooms[roomId];
        room.Id = roomId;
        const TJson& state = data["state"]["events"];
        if (state.IsArray()) {
            for (const auto& event : state.AsArray()) {
                ApplyStateEvent(room, event);
            }
        }
        const TJson& timeline = data["timeline"];
        const bool limited = timeline["limited"].IsBool() && timeline["limited"].AsBool();
        if (limited) {
            // The gap is not filled yet, so messages before it are dropped rather than shown as if adjacent
            room.Timeline.clear();
            room.Limited = true;
            room.HistoryComplete = false;
        }
        if (limited || !seenBefore) {
            // History before the first timeline chunk we hold starts at its prev_batch
            room.PrevBatch = timeline.StringOr("prev_batch");
            if (room.PrevBatch.empty()) {
                room.HistoryComplete = true;
            }
        }
        const TJson& events = timeline["events"];
        if (events.IsArray()) {
            for (const auto& event : events.AsArray()) {
                ApplyTimelineEvent(room, event);
            }
        }
    }
}

void TSyncState::PrependHistory(const std::string& roomId, const TJson& response) {
    const auto it = Rooms.find(roomId);
    if (it == Rooms.end()) {
        return;  // left while the request was in flight
    }
    TRoom& room = it->second;

    std::unordered_set<std::string> known;
    for (const auto& message : room.Timeline) {
        known.insert(message.EventId);
    }
    // dir=b returns the newest event first; state events in history are old and must not replace current state
    std::vector<TMessage> older;
    const TJson& chunk = response["chunk"];
    if (chunk.IsArray()) {
        for (const auto& event : chunk.AsArray()) {
            if (auto message = ParseMessage(event); message && !known.count(message->EventId)) {
                older.push_back(std::move(*message));
            }
        }
    }
    room.Timeline.insert(room.Timeline.begin(), older.rbegin(), older.rend());

    const bool emptyChunk = !chunk.IsArray() || chunk.AsArray().empty();
    room.PrevBatch = response.StringOr("end");
    if (room.PrevBatch.empty() || emptyChunk) {
        room.PrevBatch.clear();
        room.HistoryComplete = true;
    }
}

} // namespace NHoshi
