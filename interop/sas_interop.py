#!/usr/bin/env python3
"""SAS compatibility with vodozemac, the library behind Element.

Talks to Hoshi through build/sas_tool (one JSON command per line): both sides exchange ephemeral keys and must
derive the same emoji, the same numbers and the same hkdf-hmac-sha256.v2 MACs for the same info strings.
Several runs, so that every emoji position gets more than one value.
Usage: sas_interop.py PATH_TO_SAS_TOOL
"""
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


def check(condition, what):
    if not condition:
        raise AssertionError(what)


def one_run(tool, run):
    hoshi = Hoshi(tool)
    theirs = v.Sas()
    hoshi_key = hoshi(cmd='key')['key']
    check(hoshi(cmd='their_key', key=theirs.public_key.to_base64())['ok'], 'hoshi takes the vodozemac key')
    established = theirs.diffie_hellman(v.Curve25519PublicKey.from_base64(hoshi_key))

    info = f'MATRIX_KEY_VERIFICATION_SAS|@alice:x|A|{hoshi_key}|@bob:x|B|key|txn{run}'
    expected = established.bytes(info)
    got = hoshi(cmd='bytes', info=info)
    check(got['emoji'] == list(expected.emoji_indices), f'emoji {got["emoji"]} != {list(expected.emoji_indices)}')
    check(got['decimals'] == list(expected.decimals), f'decimals {got["decimals"]} != {list(expected.decimals)}')

    mac_info = f'MATRIX_KEY_VERIFICATION_MAC@alice:xA@bob:xBtxn{run}ed25519:A'
    mac_input = 'nj0VIGHr1xBFYXKqpM2XQsw6eG4DVl6EaXXDtG2Mu6U'
    check(hoshi(cmd='mac', input=mac_input, info=mac_info)['mac'] == established.calculate_mac(mac_input, mac_info),
          'the same MAC')
    hoshi.close()
    return got['emoji']


if __name__ == '__main__':
    tool = sys.argv[1]
    seen = [set() for _ in range(7)]
    for run in range(20):
        for position, index in enumerate(one_run(tool, run)):
            seen[position].add(index)
    check(all(len(values) > 1 for values in seen), 'every emoji position varies')

    # The emoji table against the spec's sas-emoji.json as Element bundles it, when Element is downloaded
    if len(sys.argv) > 2:
        import sas_emoji_from_element
        spec = sas_emoji_from_element.load(sys.argv[2])
        hoshi = Hoshi(tool)
        ours = hoshi(cmd='emoji_table')['table']
        hoshi.close()
        check([(e['emoji'], e['description']) for e in ours] == [(e['emoji'], e['description']) for e in spec],
              'the emoji table is the spec one')
    print('sas interop: all passed')
