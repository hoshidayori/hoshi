#include "sync.h"

#include <cstdio>
#include <cstdlib>

using namespace NHoshi;

namespace {

int Failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++Failures;
    }
}

// Shaped like the /sync example in the client-server API specification, cut to what the client reads
const char* FirstSync = R"({
  "next_batch": "s72595_4483_1934",
  "rooms": {
    "join": {
      "!726s6s6q:example.com": {
        "state": {"events": [
          {"type": "m.room.name", "state_key": "", "sender": "@alice:example.com",
           "event_id": "$name1", "origin_server_ts": 1432735824653, "content": {"name": "Огоньки"}}
        ]},
        "timeline": {
          "limited": true,
          "prev_batch": "t34-23535_0_0",
          "events": [
            {"type": "m.room.member", "state_key": "@alice:example.com", "sender": "@alice:example.com",
             "event_id": "$member1", "origin_server_ts": 1432735824653, "content": {"membership": "join"}},
            {"type": "m.room.message", "sender": "@alice:example.com", "event_id": "$143273582443PhrSn",
             "origin_server_ts": 1432735824653, "content": {"msgtype": "m.text", "body": "Привет, мир"}},
            {"type": "m.room.message", "sender": "@bob:example.com", "event_id": "$redacted",
             "origin_server_ts": 1432735824700, "content": {}},
            {"type": "m.reaction", "sender": "@bob:example.com", "event_id": "$react",
             "content": {"m.relates_to": {"rel_type": "m.annotation", "event_id": "$143273582443PhrSn", "key": "👍"}}}
          ]
        }
      },
      "!secret:example.com": {
        "timeline": {"events": [
          {"type": "m.room.encryption", "state_key": "", "sender": "@alice:example.com", "event_id": "$enc",
           "content": {"algorithm": "m.megolm.v1.aes-sha2"}},
          {"type": "m.room.encrypted", "sender": "@alice:example.com", "event_id": "$cipher",
           "content": {"algorithm": "m.megolm.v1.aes-sha2", "ciphertext": "AwgA..."}}
        ]}
      }
    }
  }
})";

const char* SecondSync = R"({
  "next_batch": "s72596_4483_1934",
  "rooms": {"join": {"!726s6s6q:example.com": {"timeline": {"limited": false, "events": [
    {"type": "m.room.message", "sender": "@bob:example.com", "event_id": "$second",
     "origin_server_ts": 1432735825000, "content": {"msgtype": "m.text", "body": "и тебе привет"}},
    {"type": "m.room.name", "state_key": "", "sender": "@bob:example.com", "event_id": "$name2",
     "content": {"name": "Звёзды"}}
  ]}}}}
})";

} // namespace

int main() {
    TSyncState state;
    state.Apply(TJson::Parse(FirstSync));

    Expect(state.NextBatch == "s72595_4483_1934", "next_batch");
    Expect(state.Rooms.size() == 2, "two rooms");

    const TRoom& room = state.Rooms.at("!726s6s6q:example.com");
    Expect(room.Name == "Огоньки", "room name from state");
    Expect(!room.Encrypted, "plain room");
    Expect(room.Limited && room.PrevBatch == "t34-23535_0_0", "limited timeline keeps prev_batch");
    Expect(room.Timeline.size() == 1, "only the message with a body is kept");
    Expect(room.Timeline.at(0).Body == "Привет, мир", "message body");
    Expect(room.Timeline.at(0).Sender == "@alice:example.com", "message sender");
    Expect(room.Timeline.at(0).MsgType == "m.text", "msgtype");
    Expect(room.Timeline.at(0).OriginServerTs == 1432735824653, "timestamp");

    const TRoom& secret = state.Rooms.at("!secret:example.com");
    Expect(secret.Encrypted, "encryption seen in timeline state event");
    Expect(secret.Timeline.empty(), "encrypted events are not shown as messages yet");

    state.Apply(TJson::Parse(SecondSync));
    const TRoom& after = state.Rooms.at("!726s6s6q:example.com");
    Expect(state.NextBatch == "s72596_4483_1934", "next_batch advances");
    Expect(after.Timeline.size() == 2, "messages append across syncs");
    Expect(after.Timeline.at(1).Body == "и тебе привет", "second message");
    Expect(after.Name == "Звёзды", "rename in timeline");

    state.Apply(TJson::Parse(R"({"next_batch": "s3", "rooms": {"leave": {"!secret:example.com": {"timeline": {"events": []}}}}})"));
    Expect(state.Rooms.count("!secret:example.com") == 0, "left room is dropped");
    Expect(state.Rooms.count("!726s6s6q:example.com") == 1, "other rooms stay");

    // Garbage of the wrong shape must not throw
    state.Apply(TJson::Parse(R"({"rooms": {"join": {"!x:y": {"timeline": {"events": [1, "a", null, {"type": 5}]}}}}})"));
    Expect(state.Rooms.at("!x:y").Timeline.empty(), "malformed events are ignored");

    if (Failures == 0) {
        std::printf("sync_test: all passed\n");
    }
    return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
