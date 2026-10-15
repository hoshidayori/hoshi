#!/usr/bin/env python3
"""Creates a room with many numbered messages from alice and joins bob, for trying history loading by hand.
Usage: make_history_room.py NAME COUNT"""
import os
import sys
import uuid
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from say import call  # noqa: E402


def login(user, password):
    return call('POST', '/_matrix/client/v3/login', {
        'type': 'm.login.password', 'identifier': {'type': 'm.id.user', 'user': user}, 'password': password,
    })['access_token']


name, count = sys.argv[1], int(sys.argv[2])
alice = login('alice', 'alice-test-pw')
bob = login('bob', 'bob-test-pw')
room = call('POST', '/_matrix/client/v3/createRoom', {'name': name, 'preset': 'public_chat'}, alice)['room_id']
quoted = urllib.parse.quote(room, safe='')
call('POST', f'/_matrix/client/v3/join/{quoted}', {}, bob)
for i in range(1, count + 1):
    call('PUT', f'/_matrix/client/v3/rooms/{quoted}/send/m.room.message/{uuid.uuid4().hex}',
         {'msgtype': 'm.text', 'body': f'Сообщение {i} из {count}'}, alice)
for token in (alice, bob):
    call('POST', '/_matrix/client/v3/logout', {}, token)
print('room', room)
