#!/usr/bin/env python3
"""Creates a user on the local test Synapse through its Unix socket, with the shared-secret admin API.

register_new_matrix_user only takes a URL, and the server listens on data/hs.sock, so this does the same
two requests (nonce, then HMAC-signed registration) over the socket. Existing users are reported, not changed.
Usage: register.py USER PASSWORD [--admin]"""
import hashlib
import hmac
import http.client
import json
import os
import socket
import sys

here = os.path.dirname(os.path.abspath(__file__))


class UnixConnection(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__('localhost')
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        # Relative name: the absolute path is longer than macOS allows for AF_UNIX
        os.chdir(os.path.join(here, 'data'))
        self.sock.connect(self.path)


def call(method, path, body=None):
    conn = UnixConnection('hs.sock')
    conn.request(method, path, json.dumps(body) if body is not None else None, {'Content-Type': 'application/json'})
    response = conn.getresponse()
    return response.status, json.loads(response.read() or b'{}')


def shared_secret():
    for line in open(os.path.join(here, 'homeserver.yaml')):
        if line.startswith('registration_shared_secret:'):
            return line.split(':', 1)[1].strip().strip('"')
    sys.exit('no registration_shared_secret in homeserver.yaml')


user, password = sys.argv[1], sys.argv[2]
admin = '--admin' in sys.argv[3:]
status, body = call('GET', '/_synapse/admin/v1/register')
nonce = body['nonce']
# HMAC-SHA1 over nonce, user, password and admin flag, separated by NUL bytes (no trailing NUL)
message = b'\x00'.join(part.encode() for part in (nonce, user, password, 'admin' if admin else 'notadmin'))
mac = hmac.new(shared_secret().encode(), message, hashlib.sha1).hexdigest()
status, body = call('POST', '/_synapse/admin/v1/register', {
    'nonce': nonce, 'username': user, 'password': password, 'admin': admin, 'mac': mac,
})
if status == 200:
    print('created', body.get('user_id'))
elif body.get('errcode') == 'M_USER_IN_USE':
    print('exists', user)
else:
    sys.exit(f'failed: {status} {body}')
