#!/usr/bin/env bash
# Builds jsc-shim.elf: main.c exercises src/engine/jsc (the QuickJS API on JavaScriptCore) on a PS5.
#
# Needs a JavaScriptCore build for the PS5 (JSCOnly, static; WebKit's JavaScriptCore/Headers, libJavaScriptCore.a,
# libWTF.a, libbmalloc.a and the JavaScriptCoreJIT object library) and the ps5-payload-sdk, both inside a docker
# image that has the SDK:
#
#   IMAGE=<image with the SDK> tests/ps5_jsc_shim/build.sh <dir with the JSC build tree, mounted at /jsc>
#
# <dir> must contain out/lib/libJavaScriptCore.a (the CMake build directory of JSCOnly is "out" there).
# Send jsc-shim.elf to elfldr (port 9021) and read UDP port 18194 or /data/jsc-shim.log; the last line says
# "N checks, 0 failures".
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTSIDER="$(cd "$HERE/../.." && pwd)"
JSC="$(realpath "${1:?usage: IMAGE=<image> build.sh <dir with out/>}")"
: "${IMAGE:?set IMAGE to a docker image with the ps5-payload-sdk in /opt/ps5-payload-sdk}"
[ -f "$JSC/out/lib/libJavaScriptCore.a" ] || { echo "no $JSC/out/lib/libJavaScriptCore.a" >&2; exit 2; }
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$JSC:/jsc" -v "$OUTSIDER:/outsider" "$IMAGE" bash -c '
set -euo pipefail
SDK=/opt/ps5-payload-sdk; J=/jsc/out; HB=$SDK/target/user/homebrew
JITOBJS=$(find $J/Source/JavaScriptCore/CMakeFiles/JavaScriptCoreJIT.dir -name "*.o" | sort)
cd /outsider/tests/ps5_jsc_shim
INC="-I/outsider/src/engine/jsc -I$J/JavaScriptCore/Headers"
$SDK/bin/prospero-clang -O2 -c $INC -o /tmp/qjs_jsc.o /outsider/src/engine/jsc/qjs_jsc.c
$SDK/bin/prospero-clang -O2 -c $INC -o /tmp/main.o main.c
$SDK/bin/prospero-clang++ -O2 -o jsc-shim.elf /tmp/main.o /tmp/qjs_jsc.o $JITOBJS $J/lib/libJavaScriptCore.a \
    $J/lib/libWTF.a $J/lib/libbmalloc.a $HB/lib/libicui18n.a $HB/lib/libicuuc.a $HB/lib/libicudata.a -lpthread -lm
ls -la jsc-shim.elf'
