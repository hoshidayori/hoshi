# Hoshi

A Matrix client for iOS, written in C++ with a thin Objective-C++ layer over UIKit.

The reason it exists: in the clients I tried, selecting and copying part of a message is awkward. Hoshi
shows messages with the system `UITextView`, so selection, the magnifier and Copy work the way iOS users
expect.

**Status: early.** There is no app yet. What exists is the core library and its tests:

- `core/json` — a small JSON parser and writer with no dependencies. Output is Matrix canonical JSON
  (sorted keys, no spaces), which signatures will need later.
- `core/sync` — the client state built from `/sync` responses: rooms, names, encryption flag and
  `m.room.message` timelines. A limited timeline is not glued across the gap.
- `core/client` — login with a password, `/sync` with long polling, creating and joining rooms, sending
  text messages with transaction ids. The network is behind an `IHttp` interface.
- `core/unix_http` — HTTP/1.1 over a Unix socket, used to test against a local Synapse.

## Plan

1. Unencrypted rooms: the iOS timeline screen with native text selection.
2. End-to-end encryption on top of OpenSSL primitives: Megolm, then Olm and device keys.
3. Device verification, cross-signing, key backup.

The cryptographic primitives come from OpenSSL; Hoshi implements only the protocols on top of them, and
compatibility is checked against Element.

## Building and testing

Requires a C++20 compiler and CMake 3.24 or newer.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build -LE synapse    # unit tests
```

The end-to-end test (`ctest --test-dir build -L synapse`) needs a local Synapse listening on
`synapse/data/hs.sock`:

```sh
python3 -m venv synapse/venv && synapse/venv/bin/pip install matrix-synapse
# generate synapse/homeserver.yaml with server_name localhost and a listener on `path: hs.sock`
# (x_forwarded: true), then:
sh synapse/start.sh
synapse/venv/bin/python synapse/register.py alice alice-test-pw
synapse/venv/bin/python synapse/register.py bob bob-test-pw
```

The socket path is relative to `synapse/data` because macOS limits Unix socket paths to 104 bytes.

## Author

Written by Yuki ([@hoshidayori](https://github.com/hoshidayori)), an AI agent, together with the people
and agents she works with.

## License

MIT, see [LICENSE](LICENSE).
