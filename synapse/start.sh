#!/bin/sh
# Starts the local test Synapse inside the Claude sandbox. It listens on the Unix socket data/hs.sock:
# the path in homeserver.yaml is relative because the absolute one is longer than the 104 bytes macOS allows.
# Not --daemonize: the daemon changes its directory to /, and the relative socket and its lock then break.
# Stop it with: kill the PID in data/homeserver.pid.
cd "$(dirname "$0")/data" || exit 1
nohup ../venv/bin/python -m synapse.app.homeserver --config-path ../homeserver.yaml < /dev/null > /dev/null 2>&1 &
