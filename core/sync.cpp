#include "sync.h"

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

void ApplyTimelineEvent(TRoom& room, const TJson& event) {
    if (event["state_key"].IsString()) {
        ApplyStateEvent(room, event);
        return;
    }
    if (event.StringOr("type") != "m.room.message") {
        return;
    }
    const TJson& content = event["content"];
    if (!content["body"].IsString()) {
        return;  // a redacted message has empty content
    }
    TMessage message;
    message.EventId = event.StringOr("event_id");
    message.Sender = event.StringOr("sender");
    message.MsgType = content.StringOr("msgtype");
    message.Body = content["body"].AsString();
    if (event["origin_server_ts"].IsInt()) {
        message.OriginServerTs = event["origin_server_ts"].AsInt();
    }
    room.Timeline.push_back(std::move(message));
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
        TRoom& room = Rooms[roomId];
        room.Id = roomId;
        const TJson& state = data["state"]["events"];
        if (state.IsArray()) {
            for (const auto& event : state.AsArray()) {
                ApplyStateEvent(room, event);
            }
        }
        const TJson& timeline = data["timeline"];
        if (timeline["limited"].IsBool() && timeline["limited"].AsBool()) {
            // The gap is not filled yet, so messages before it are dropped rather than shown as if adjacent
            room.Timeline.clear();
            room.Limited = true;
            room.PrevBatch = timeline.StringOr("prev_batch");
        }
        const TJson& events = timeline["events"];
        if (events.IsArray()) {
            for (const auto& event : events.AsArray()) {
                ApplyTimelineEvent(room, event);
            }
        }
    }
}

} // namespace NHoshi
