#!/bin/bash
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
app="${1:?Usage: bash tests/run_macos27_images.sh signed-app identity fixture [image|device|device-ro]}"
app="$(cd -- "$app" && pwd)"
identity="${2:?Provide a local signing identity}"
fixture="${3:?Provide a raw volume image (image) or MBR image (device)}"
mode="${4:-image}"
case "$mode" in image|device|device-ro) ;; *) exit 1 ;; esac
export XLINUXFS_TEST_EXTENSION="$app/Contents/Extensions/lklfuse.appex"
unset XLINUXFS_TEST_DEVICE XLINUXFS_TEST_IMAGE XLINUXFS_TEST_READ_ONLY_MEDIA
mkdir -p "$root/_tmp"
work="$(mktemp -d "$root/_tmp/macos27-image-tests.XXXXXX")"
probe="$work/Test.app"
whole=""
cleanup() {
    if [[ -n "$whole" ]]; then
        if hdiutil detach "$whole"; then rm "$work/attached.img"; fi
    fi
    /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -u "$probe" || true
    rm -rf -- "$probe"
}
trap cleanup EXIT
mkdir -p "$probe/Contents/MacOS" "$probe/Contents/Resources"
cp "$app/Contents/Info.plist" "$probe/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleExecutable Probe' "$probe/Contents/Info.plist"
cp "$app/Contents/embedded.provisionprofile" "$probe/Contents/embedded.provisionprofile"
cp -c "$fixture" "$probe/Contents/Resources/input.img"
codesign -d --entitlements :- "$app" > "$work/entitlements.plist"
/usr/libexec/PlistBuddy -c 'Print :com.apple.developer.fskit.mount' "$work/entitlements.plist"
test_source="$root/tests/test_macos27_images.swift"
if [[ "$mode" != image ]]; then test_source="$root/tests/test_macos27_devices.swift"; fi
xcrun swiftc -parse-as-library -target "$(uname -m)-apple-macos27.0" \
    "$test_source" "$root"/xlinuxfs/Services/*.swift "$root/xlinuxfs/Model/LinuxDevice.swift" \
    -o "$probe/Contents/MacOS/Probe"
codesign --sign "$identity" --entitlements "$work/entitlements.plist" "$probe"
xcrun swiftc "$root/tests/verify_macos27_images.swift" -o "$work/verify-host"
if [[ "$mode" != image ]]; then
    cp -c "$fixture" "$work/attached.img"
    access=-readwrite
    if [[ "$mode" == device-ro ]]; then access=-readonly; export XLINUXFS_TEST_READ_ONLY_MEDIA=1; fi
    hdiutil attach -nomount "$access" -plist "$work/attached.img" > "$work/attached.plist"
    devices="$(xcrun swift "$root/tests/attached_devices.swift" "$work/attached.plist")"
    whole="${devices%%$'\n'*}"
    partition="${devices##*$'\n'}"
    export XLINUXFS_TEST_DEVICE="${partition#/dev/}"
    export XLINUXFS_TEST_IMAGE="$work/attached.img"
fi
"$work/verify-host" "$probe/Contents/MacOS/Probe" 2>&1 | tee "$work/results.log"
printf 'Results: %s/results.log\n' "$work"
