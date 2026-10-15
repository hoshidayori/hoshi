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
    Expect(secret.Timeline.size() == 1 && secret.Timeline[0].IsWaitingForKey() && secret.Timeline[0].EventId == "$cipher",
        "an encrypted event waits in the timeline for its key");

    state.Apply(TJson::Parse(SecondSync));
    const TRoom& after = state.Rooms.at("!726s6s6q:example.com");
    Expect(state.NextBatch == "s72596_4483_1934", "next_batch advances");
    Expect(after.Timeline.size() == 2, "messages append across syncs");
    Expect(after.Timeline.at(1).Body == "и тебе привет", "second message");
    Expect(after.Name == "Звёзды", "rename in timeline");

    // History before the limited timeline: dir=b gives the newest first, state events must not win
    state.PrependHistory("!726s6s6q:example.com", TJson::Parse(R"({"start": "t34-23535_0_0", "end": "t20-1_0_0", "chunk": [
        {"type": "m.room.message", "sender": "@bob:example.com", "event_id": "$old2", "content": {"msgtype": "m.text", "body": "второе старое"}},
        {"type": "m.room.name", "state_key": "", "sender": "@bob:example.com", "event_id": "$oldname", "content": {"name": "Старое имя"}},
        {"type": "m.room.message", "sender": "@bob:example.com", "event_id": "$old1", "content": {"msgtype": "m.text", "body": "первое старое"}},
        {"type": "m.room.message", "sender": "@alice:example.com", "event_id": "$143273582443PhrSn", "content": {"msgtype": "m.text", "body": "Привет, мир"}}
    ]})"));
    const TRoom& withHistory = state.Rooms.at("!726s6s6q:example.com");
    Expect(withHistory.Timeline.size() == 4, "two older messages prepended, the known one skipped");
    Expect(withHistory.Timeline.at(0).Body == "первое старое" && withHistory.Timeline.at(1).Body == "второе старое", "history in chronological order");
    Expect(withHistory.Timeline.at(2).Body == "Привет, мир", "existing timeline follows history");
    Expect(withHistory.Name == "Звёзды", "old name in history does not replace the current one");
    Expect(withHistory.PrevBatch == "t20-1_0_0" && !withHistory.HistoryComplete, "next history token");

    state.PrependHistory("!726s6s6q:example.com", TJson::Parse(R"({"start": "t20-1_0_0", "chunk": []})"));
    Expect(state.Rooms.at("!726s6s6q:example.com").HistoryComplete, "empty chunk without end completes the history");

    // Local echo: pending until /sync returns the event with our transaction id
    state.AddPending("!726s6s6q:example.com", "txnA", "@alice:example.com", "уже пишу", 1432735830000);
    state.AddPending("!726s6s6q:example.com", "txnB", "@alice:example.com", "и это", 1432735830001);
    Expect(state.Rooms.at("!726s6s6q:example.com").Pending.size() == 2, "two pending");
    state.MarkPendingFailed("!726s6s6q:example.com", "txnB");
    Expect(state.Rooms.at("!726s6s6q:example.com").Pending.at(1).SendState == TMessage::ESendState::Failed, "failed send");
    const TMessage* retry = state.RetryPending("!726s6s6q:example.com", "txnB");
    Expect(retry && retry->Body == "и это" && retry->SendState == TMessage::ESendState::Sending, "retry keeps the text");
    Expect(state.RetryPending("!726s6s6q:example.com", "txnB") == nullptr, "a sending message is not retried twice");
    state.MarkPendingSent("!726s6s6q:example.com", "txnB", "$echoB");
    state.Apply(TJson::Parse(R"({"next_batch": "s2b", "rooms": {"join": {"!726s6s6q:example.com": {"timeline": {"events": [
        {"type": "m.room.message", "sender": "@alice:example.com", "event_id": "$echoA", "content": {"msgtype": "m.text", "body": "уже пишу"},
         "unsigned": {"transaction_id": "txnA"}},
        {"type": "m.room.message", "sender": "@alice:example.com", "event_id": "$echoB", "content": {"msgtype": "m.text", "body": "и это"}}
    ]}}}}})"));
    const TRoom& echoed = state.Rooms.at("!726s6s6q:example.com");
    Expect(echoed.Pending.empty(), "echo by transaction id and by event id clears pending");
    Expect(echoed.Timeline.back().Body == "и это" && echoed.Timeline.back().SendState == TMessage::ESendState::Sent, "echo is in the timeline");

    // Display names of unnamed rooms
    state.Apply(TJson::Parse(R"({"rooms": {"join": {"!dm:example.com": {
        "summary": {"m.heroes": ["@alice:example.com"]},
        "state": {"events": [
            {"type": "m.room.member", "state_key": "@alice:example.com", "sender": "@alice:example.com", "content": {"membership": "join", "displayname": "Алиса"}},
            {"type": "m.room.member", "state_key": "@me:example.com", "sender": "@me:example.com", "content": {"membership": "join"}}
        ]}, "timeline": {"events": []}}}}})"));
    Expect(state.Rooms.at("!dm:example.com").DisplayName("@me:example.com") == "Алиса", "direct chat is named after the other person");
    TRoom group;
    group.Members = {"@me:x", "@a:x", "@b:x", "@c:x", "@d:x"};
    Expect(group.DisplayName("@me:x") == "@a:x, @b:x, @c:x and 1 other", "group of members");
    group.CanonicalAlias = "#звёзды:x";
    Expect(group.DisplayName("@me:x") == "#звёзды:x", "alias before members");
    Expect(TRoom{}.DisplayName("@me:x") == "Empty room", "no one else");

    // Invites from stripped state, gone once joined
    state.Apply(TJson::Parse(R"({"rooms": {"invite": {"!inv:example.com": {"invite_state": {"events": [
        {"type": "m.room.name", "state_key": "", "sender": "@carol:example.com", "content": {"name": "Приглашение"}},
        {"type": "m.room.encryption", "state_key": "", "sender": "@carol:example.com", "content": {"algorithm": "m.megolm.v1.aes-sha2"}},
        {"type": "m.room.member", "state_key": "@alice:example.com", "sender": "@carol:example.com", "content": {"membership": "invite"}}
    ]}}}}})"));
    const auto& invite = state.Invites.at("!inv:example.com");
    Expect(invite.Name == "Приглашение" && invite.Inviter == "@carol:example.com" && invite.Encrypted, "invite from stripped state");
    state.Apply(TJson::Parse(R"({"rooms": {"join": {"!inv:example.com": {"timeline": {"events": []}}}}})"));
    Expect(state.Invites.empty() && state.Rooms.count("!inv:example.com"), "joining moves the invite to the rooms");

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
