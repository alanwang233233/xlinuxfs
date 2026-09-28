#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
fixtures="${1:?Usage: bash tests/run_bridge_platforms.sh raw-image-directory}"
mkdir -p "$root/_tmp"
bash "$root/scripts/build-liblkl.sh"
work="$(mktemp -d "$root/_tmp/platform-bridge.XXXXXX")"
cleanup() { rm -f -- "$work/fixture.img" "$work/test-arm64" "$work/test-x86_64"; }
trap cleanup EXIT
for arch in arm64 x86_64; do
    xcrun clang -arch "$arch" -mmacosx-version-min=15.4 -O2 -DLKL_HOST_CONFIG_POSIX \
        -I"$root/lklfuse/bridge" -I"$root/libs/include" \
        "$root/tests/test_bridge_platform.c" "$root/cli-test/lkl_file_backend.c" \
        "$root/lklfuse/bridge/lkl_fskit.c" "$root/lklfuse/bridge/lkl_blk_ops.c" \
        "$root/libs/liblkl.a" -o "$work/test-$arch" > "$work/build-$arch.log" 2>&1
    for fs in ext4 xfs btrfs; do
        cp -c "$fixtures/$fs-rawfs.img" "$work/fixture.img"
        before="$(shasum -a 256 "$work/fixture.img")"
        "$work/test-$arch" "$work/fixture.img" ro > "$work/$arch-$fs-ro.log" 2>&1
        test "$before" = "$(shasum -a 256 "$work/fixture.img")"
        "$work/test-$arch" "$work/fixture.img" rw > "$work/$arch-$fs-rw.log" 2>&1
        before="$(shasum -a 256 "$work/fixture.img")"
        "$work/test-$arch" "$work/fixture.img" verify > "$work/$arch-$fs-verify.log" 2>&1
        test "$before" = "$(shasum -a 256 "$work/fixture.img")"
        rm "$work/fixture.img"
        printf 'PASS %s %s: RO hash, create/rename/readback, remount persistence\n' "$arch" "$fs"
    done
done
printf 'Logs: %s\n' "$work"
