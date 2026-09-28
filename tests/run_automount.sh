#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
fixtures="${1:?Usage: bash tests/run_automount.sh image-directory}"
mkdir -p "$root/_tmp"
work="$(mktemp -d "$root/_tmp/automount.XXXXXX")"
whole=""
cleanup() {
    if [[ -n "$whole" ]]; then hdiutil detach "$whole" || return; fi
    rm -f -- "$work/fixture.img"
}
trap cleanup EXIT
xcrun swiftc "$root/tests/verify_automount.swift" -o "$work/verify"
for fs in ext4 xfs btrfs; do
    cp -c "$fixtures/$fs-mbr.img" "$work/fixture.img"
    before="$(shasum -a 256 "$work/fixture.img")"
    hdiutil attach -readonly -plist "$work/fixture.img" > "$work/$fs.plist"
    devices="$(xcrun swift "$root/tests/attached_devices.swift" "$work/$fs.plist")"
    whole="${devices%%$'\n'*}"
    "$work/verify" "$work/$fs.plist"
    hdiutil detach "$whole"
    whole=""
    test "$before" = "$(shasum -a 256 "$work/fixture.img")"
    rm "$work/fixture.img"
done
printf 'Logs: %s\n' "$work"
