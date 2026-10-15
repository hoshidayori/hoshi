#!/usr/bin/env python3
"""Olm compatibility with vodozemac, the library behind Element, in both directions.

Talks to Hoshi through build/olm_tool (one JSON command per line) and runs two conversations:
vodozemac starts the session with Hoshi, then Hoshi starts one with vodozemac. Each covers the pre-key
messages before the first answer, ratchet turns in both directions, messages out of order, and a save
and load of Hoshi's session in the middle.
Usage: olm_interop.py PATH_TO_OLM_TOOL
"""
import base64
import json
import subprocess
import sys

import vodozemac as v


class Hoshi:
    def __init__(self, tool):
        self.proc = subprocess.Popen([tool], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def __call__(self, **command):
        self.proc.stdin.write(json.dumps(command, ensure_ascii=False) + '\n')
        self.proc.stdin.flush()
        answer = json.loads(self.proc.stdout.readline())
        if 'error' in answer:
            raise AssertionError(f"hoshi {command['cmd']}: {answer['error']}")
        return answer

    def close(self):
        self.proc.stdin.close()
        self.proc.wait()


# Matrix carries Olm messages as unpadded base64; vodozemac's to_parts/from_parts use the raw bytes
def b64decode(text):
    return base64.b64decode(text + '=' * (-len(text) % 4))


def to_vodozemac(message):
    return v.AnyOlmMessage.from_parts(message['type'], b64decode(message['body']))


def hoshi_message(message):
    kind, raw = message.to_parts()
    return {'type': kind, 'body': base64.b64encode(raw).decode().rstrip('=')}


def check(condition, what):
    if not condition:
        raise AssertionError(what)
    print('ok:', what)


def vodozemac_starts(tool):
    hoshi = Hoshi(tool)
    keys = hoshi(cmd='account')
    alice = v.Account()
    session = alice.create_outbound_session(v.Curve25519PublicKey.from_base64(keys['identity']),
                                            v.Curve25519PublicKey.from_base64(keys['one_time_key']))
    first = hoshi_message(session.encrypt('привет от Element'.encode()))
    second = hoshi_message(session.encrypt('second pre-key message'.encode()))
    check(first['type'] == 0 and second['type'] == 0, 'vodozemac sends pre-key messages before an answer')

    answer = hoshi(cmd='inbound', body=first['body'])
    check(answer['plaintext'] == 'привет от Element', 'Hoshi creates the inbound session from a vodozemac pre-key message')
    check(hoshi(cmd='decrypt', **second)['plaintext'] == 'second pre-key message', 'Hoshi reads the second pre-key message')
    check(hoshi(cmd='session_id')['session_id'] == session.session_id, 'session ids match')

    reply = hoshi(cmd='encrypt', text='ответ от Hoshi')
    check(reply['type'] == 1, 'Hoshi answers with a normal message')
    check(session.decrypt(to_vodozemac(reply)).decode() == 'ответ от Hoshi', 'vodozemac reads Hoshi\'s answer')

    # A ratchet turn from vodozemac, with two messages delivered in reverse order
    m1 = hoshi_message(session.encrypt(b'turn 1 message 1'))
    m2 = hoshi_message(session.encrypt(b'turn 1 message 2'))
    hoshi(cmd='reload')
    check(hoshi(cmd='decrypt', **m2)['plaintext'] == 'turn 1 message 2', 'Hoshi reads a later message first, after a reload')
    check(hoshi(cmd='decrypt', **m1)['plaintext'] == 'turn 1 message 1', 'Hoshi reads the skipped message')

    for i in range(3):
        back = hoshi(cmd='encrypt', text=f'Hoshi turn {i}')
        check(session.decrypt(to_vodozemac(back)).decode() == f'Hoshi turn {i}', f'vodozemac reads Hoshi turn {i}')
        forth = hoshi_message(session.encrypt(f'vodozemac turn {i}'.encode()))
        check(hoshi(cmd='decrypt', **forth)['plaintext'] == f'vodozemac turn {i}', f'Hoshi reads vodozemac turn {i}')
    hoshi.close()


def hoshi_starts(tool):
    hoshi = Hoshi(tool)
    hoshi(cmd='account')
    bob = v.Account()
    bob.generate_one_time_keys(1)
    one_time_key = next(iter(bob.one_time_keys.values()))
    bob.mark_keys_as_published()
    hoshi(cmd='outbound', identity=bob.curve25519_key.to_base64(), one_time_key=one_time_key.to_base64())

    first = hoshi(cmd='encrypt', text='привет от Hoshi')
    second = hoshi(cmd='encrypt', text='и ещё одно до ответа')
    check(first['type'] == 0 and second['type'] == 0, 'Hoshi sends pre-key messages before an answer')
    alice_identity = v.PreKeyMessage.from_base64(first['body'])
    session, plaintext = bob.create_inbound_session(identity_key_of(first, hoshi), alice_identity)
    check(plaintext.decode() == 'привет от Hoshi', 'vodozemac creates the inbound session from a Hoshi pre-key message')
    check(session.decrypt(to_vodozemac(second)).decode() == 'и ещё одно до ответа', 'vodozemac reads the second pre-key message')
    check(hoshi(cmd='session_id')['session_id'] == session.session_id, 'session ids match')

    reply = hoshi_message(session.encrypt('ответ от Element'.encode()))
    check(reply['type'] == 1, 'vodozemac answers with a normal message')
    hoshi(cmd='reload')
    check(hoshi(cmd='decrypt', **reply)['plaintext'] == 'ответ от Element', 'Hoshi reads the answer after a reload')
    after = hoshi(cmd='encrypt', text='теперь обычное')
    check(after['type'] == 1, 'Hoshi stops sending pre-key messages after the answer')
    check(session.decrypt(to_vodozemac(after)).decode() == 'теперь обычное', 'vodozemac reads it')

    # Several messages from Hoshi in one chain, delivered out of order
    batch = [hoshi(cmd='encrypt', text=f'batch {i}') for i in range(4)]
    for i in (3, 0, 2, 1):
        check(session.decrypt(to_vodozemac(batch[i])).decode() == f'batch {i}', f'vodozemac reads batch {i} out of order')
    hoshi.close()


def identity_key_of(pre_key_message, hoshi):
    # vodozemac wants the sender's identity key separately; in Matrix it comes from the event's sender_key.
    # It is inside the pre-key message too: field 3, after the version byte and two 34-byte fields.
    raw = b64decode(pre_key_message['body'])
    assert raw[1 + 34 + 34] == 0x1A and raw[1 + 34 + 34 + 1] == 32
    return v.Curve25519PublicKey.from_bytes(raw[1 + 34 + 34 + 2:1 + 34 + 34 + 2 + 32])


if __name__ == '__main__':
    tool = sys.argv[1]
    vodozemac_starts(tool)
    hoshi_starts(tool)
    print('olm interop: all passed')
