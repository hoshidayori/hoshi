#!/bin/sh
# Builds static OpenSSL libcrypto for the iOS simulator (arm64) and iPhone (arm64) from the verified
# source archive in third_party/src. Only libcrypto is needed: Hoshi uses OpenSSL for primitives, not TLS.
# Writes third_party/openssl/<platform>/{include,lib} and third_party/openssl/status.json
# {"state": running|done|failed, "step": ...}. Run detached: nohup sh build-openssl-ios.sh &
version=3.5.9
here=$(cd "$(dirname "$0")" && pwd)
out=$here/openssl
status=$out/status.json
mkdir -p "$out"

say() {
    printf '{"state":"%s","step":"%s"}\n' "$1" "$2" > "$status.tmp" && mv "$status.tmp" "$status"
}

build() {
    platform=$1
    target=$2
    say running "$platform"
    work=$here/build/$platform
    rm -rf "$work"
    mkdir -p "$work"
    tar -xzf "$here/src/openssl-$version.tar.gz" -C "$work" || return 1
    cd "$work/openssl-$version" || return 1
    ./Configure "$target" no-shared no-tests no-apps no-docs no-engine no-dso no-ui-console \
        -mios-version-min=17.0 --prefix="$out/$platform" > "$work/configure.log" 2>&1 || return 1
    make -j8 build_libs > "$work/make.log" 2>&1 || return 1
    make install_dev > "$work/install.log" 2>&1 || return 1
}

if build iphonesimulator iossimulator-arm64-xcrun && build iphoneos ios64-xcrun; then
    say done all
else
    say failed "$platform"
fi
