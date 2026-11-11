#!/usr/bin/env bash
# Builds the mGBA Android APK.
#
# Usage: SDL2_SRC=/path/to/SDL2 ./build-apk.sh [--release]
#
# Prerequisites:
#   - ANDROID_HOME with build-tools 34, platform android-34, NDK 26.3.11579264, cmake 3.22.1
#   - JDK 17
#   - Gradle >= 8.5 (or a gradlew wrapper in this directory)
#   - Optional release signing: create keystore.properties (see app/build.gradle)
set -euo pipefail

cd "$(dirname "$0")"

SDL2_SRC="${SDL2_SRC:?Set SDL2_SRC to the path of the SDL2 source tree}"
BUILDTYPE=debug
if [ "${1:-}" = "--release" ]; then
	BUILDTYPE=release
fi

if [ ! -d "$SDL2_SRC" ]; then
	echo "SDL2 source not found at $SDL2_SRC" >&2
	exit 1
fi

# SDL 2.30's AAudio driver writes each callback with a 1 ms timeout and then
# drops whatever the stream did not accept. Queue the whole buffer instead.
python3 ./patch-sdl-aaudio.py "$SDL2_SRC"

GRADLE_CMD=gradle
if [ -x gradlew ]; then
	GRADLE_CMD=./gradlew
fi

"$GRADLE_CMD" -PSDL2_SRC="$SDL2_SRC" -PMGBA_VERSION="$(grep -hE '^set\(LIB_VERSION_(MAJOR|MINOR|PATCH) ' ../../../version.cmake | sed 's/^set(LIB_VERSION_[A-Z]* //;s/)$//' | paste -sd. -)" -PAPP_VERSION_CODE="${APP_VERSION_CODE:-1}" "assemble${BUILDTYPE^}" --stacktrace

echo
echo "APK: $(pwd)/app/build/outputs/apk/$BUILDTYPE/app-$BUILDTYPE.apk"
