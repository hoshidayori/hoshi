#!/usr/bin/env python3
"""Megolm compatibility with vodozemac, the library behind Element.

  make OUT.json   writes vectors produced by vodozemac: a session key, an exported key, and messages
                  at indices that cross the 2^8 ratchet boundary. core/megolm_test.cpp decrypts them.
  check IN.json   decrypts with vodozemac the messages Hoshi produced (megolm_tool encrypt) and checks
                  that vodozemac reads Hoshi's session key and export.
"""
import json
import os
import sys

import vodozemac as v


def make(path):
    session = v.GroupSession()
    session_key = session.session_key.to_base64()
    messages = []
    plaintexts = {0: 'первое', 1: 'second', 2: '🌠 третье'}
    for index in range(0, 260):
        text = plaintexts.get(index, f'message {index}')
        message = session.encrypt(text.encode())
        if index in (0, 1, 2, 255, 256, 259):
            messages.append({'index': index, 'plaintext': text, 'ciphertext': message.to_base64()})
    inbound = v.InboundGroupSession(v.SessionKey(session_key))
    exported = inbound.export_at(256)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        json.dump({
            'session_id': session.session_id,
            'session_key': session_key,
            'export_256': exported.to_base64(),
            'messages': messages,
        }, f, ensure_ascii=False, indent=1)


def check(path):
    data = json.load(open(path))
    inbound = v.InboundGroupSession(v.SessionKey(data['session_key']))
    assert inbound.session_id == data['session_id'], 'session id'
    for m in data['messages']:
        decrypted = inbound.decrypt(v.MegolmMessage.from_base64(m['ciphertext']))
        assert decrypted.message_index == m['index'], f"index {decrypted.message_index} != {m['index']}"
        assert decrypted.plaintext.decode() == m['plaintext'], f"plaintext at {m['index']}"
    exported = v.InboundGroupSession.import_session(v.ExportedSessionKey(data['export']))
    later = [m for m in data['messages'] if m['index'] >= exported.first_known_index]
    for m in later:
        assert exported.decrypt(v.MegolmMessage.from_base64(m['ciphertext'])).plaintext.decode() == m['plaintext']
    print(f"vodozemac read {len(data['messages'])} Hoshi messages and the export at {exported.first_known_index}")


if __name__ == '__main__':
    {'make': make, 'check': check}[sys.argv[1]](sys.argv[2])
