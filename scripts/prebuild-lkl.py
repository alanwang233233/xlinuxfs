#!/usr/bin/env python3
"""Build and cache the pinned LKL submodule's universal Darwin archive."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    lkl = root / "vendor/lkl"
    builder = lkl / "tools/lkl/darwin/build.py"
    if not builder.is_file():
        raise SystemExit("LKL submodule is missing. Run: git submodule update --init --recursive")
    configured = subprocess.run(["git", "config", "--local", "--get", "xlinuxfs.lklClang"],
                                cwd=root, text=True, capture_output=True).stdout.strip()
    explicit = os.environ.get("LKL_CLANG") or os.environ.get("CLANG") or configured
    candidates = [explicit] if explicit else [shutil.which("clang-19"),
                  "/opt/homebrew/opt/llvm@19/bin/clang", "/usr/local/opt/llvm@19/bin/clang"]
    compiler = next((str(Path(c).resolve()) for c in candidates if c and Path(c).is_file()), None)
    if not compiler:
        raise SystemExit("LLVM 19 is required. Set LKL_CLANG or run: "
                         "git config --local xlinuxfs.lklClang /path/to/llvm19/bin/clang")

    def output(command, cwd=root):
        return subprocess.check_output(command, cwd=cwd)

    work = root / "_tmp/lkl-build"
    work.mkdir(parents=True, exist_ok=True)
    library = root / "libs/liblkl.a"
    receipt = root / "libs/lkl-build.json"
    profile_headers = lkl / "tools/lkl/darwin/profile/host-include"
    # Serialize Xcode and CLI invocations; publish the receipt after both
    # architecture slices and their matching public headers are complete.
    with (work / "build.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        fingerprint = hashlib.sha256()
        for part in [Path(__file__).read_bytes(), str(lkl.resolve()).encode(),
                     output(["git", "rev-parse", "HEAD"], lkl),
                     output(["git", "diff", "HEAD", "--binary"], lkl),
                     compiler.encode(), output([compiler, "--version"]),
                     str(Path(output(["xcrun", "--show-sdk-path"]).decode().strip()).resolve()).encode(),
                     output(["xcrun", "--show-sdk-version"]),
                     output(["xcrun", "ld", "-version_details"])]:
            fingerprint.update(part)
            fingerprint.update(b"\0")
        signature = fingerprint.hexdigest()
        try:
            cached = json.loads(receipt.read_text())
        except (OSError, ValueError):
            cached = {}
        headers_match = all(
            (root / "libs/include" / p.relative_to(profile_headers)).is_file() and
            (root / "libs/include" / p.relative_to(profile_headers)).read_bytes() == p.read_bytes()
            for p in profile_headers.rglob("*.h"))
        if not args.force and cached.get("inputs") == signature and library.is_file() and headers_match:
            if hashlib.sha256(library.read_bytes()).hexdigest() == cached.get("archive_sha256"):
                print("LKL: universal archive and headers are up to date")
                return
        subprocess.run(["python3", str(builder), "--clang", compiler,
                        "--output", str(root / "libs"), "--work", str(work)], check=True)
        data = {"inputs": signature,
                "lkl_commit": output(["git", "rev-parse", "HEAD"], lkl).decode().strip(),
                "archive_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
                "compiler": compiler, "architectures": ["arm64", "x86_64"]}
        temporary = receipt.with_suffix(".tmp")
        temporary.write_text(json.dumps(data, indent=2) + "\n")
        temporary.replace(receipt)


if __name__ == "__main__":
    main()
