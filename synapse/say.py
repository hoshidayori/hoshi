#!/usr/bin/env python3
"""Sends a text message as a test user to every joined room with the given name on the local test Synapse,
through its Unix socket. For trying the app by hand: someone else writes while you watch the timeline.
Usage: say.py USER PASSWORD ROOM_NAME TEXT"""
import http.client
import json
import os
import socket
import sys
import urllib.parse
import uuid

here = os.path.dirname(os.path.abspath(__file__))


class UnixConnection(http.client.HTTPConnection):
    def __init__(self):
        super().__init__('localhost')

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        os.chdir(os.path.join(here, 'data'))  # relative name: the absolute path is too long for macOS
        self.sock.connect('hs.sock')


def call(method, path, body=None, token=None):
    conn = UnixConnection()
    headers = {'Content-Type': 'application/json', 'X-Forwarded-For': '127.0.0.1'}
    if token:
        headers['Authorization'] = 'Bearer ' + token
    conn.request(method, path, json.dumps(body) if body is not None else None, headers)
    response = conn.getresponse()
    data = json.loads(response.read() or b'{}')
    if response.status != 200:
        sys.exit(f'{method} {path}: {response.status} {data}')
    return data


user, password, room_name, text = sys.argv[1:5]
token = call('POST', '/_matrix/client/v3/login', {
    'type': 'm.login.password', 'identifier': {'type': 'm.id.user', 'user': user}, 'password': password,
})['access_token']
for room in call('GET', '/_matrix/client/v3/joined_rooms', token=token)['joined_rooms']:
    quoted = urllib.parse.quote(room, safe='')
    conn = UnixConnection()
    conn.request('GET', f'/_matrix/client/v3/rooms/{quoted}/state/m.room.name/',
                 headers={'Authorization': 'Bearer ' + token, 'X-Forwarded-For': '127.0.0.1'})
    response = conn.getresponse()
    name = json.loads(response.read() or b'{}').get('name') if response.status == 200 else None
    if name == room_name:
        txn = uuid.uuid4().hex
        sent = call('PUT', f'/_matrix/client/v3/rooms/{quoted}/send/m.room.message/{txn}',
                    {'msgtype': 'm.text', 'body': text}, token)
        print('sent', room, sent['event_id'])
call('POST', '/_matrix/client/v3/logout', {}, token)
