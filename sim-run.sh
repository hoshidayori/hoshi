#!/bin/sh
# Boots an iPhone simulator, installs build-sim/Hoshi.app, launches it and saves a screenshot to
# screenshots/. The simulator service is not reachable from the Claude sandbox, so this runs outside it.
set -e
cd "$(dirname "$0")"
app=build-sim/Hoshi.app
codesign --force --sign - "$app"

# First available iPhone; reuse it if it is already booted
device=$(xcrun simctl list devices available | grep -E 'iPhone' | grep -Eo '[0-9A-F-]{36}' | head -1)
if [ -z "$device" ]; then
    echo "no available iPhone simulator" >&2
    exit 1
fi
xcrun simctl boot "$device" 2>/dev/null || true
xcrun simctl bootstatus "$device" -b > /dev/null

xcrun simctl install "$device" "$app"
xcrun simctl launch "$device" com.github.hoshidayori.hoshi
sleep 3
mkdir -p screenshots
xcrun simctl io "$device" screenshot screenshots/timeline.png
echo "device $device"
