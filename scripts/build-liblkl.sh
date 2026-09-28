#!/bin/bash
# Shared entry point for Xcode and command-line builds.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/scripts/prebuild-lkl.py" "$@"
