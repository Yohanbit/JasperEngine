#!/bin/sh
# Fast checks for the Android port, before a build is worth spending 45 minutes on.
#
# It exists because of two specific failures that reached CI: a wrong class qualifier
# (WindowInsetsCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE, which is on the controller --
# four minutes to find, by annotation) and a brace balance broken by a scripted edit. Neither
# needs a compiler. The real build is still the authority: this only makes sure the build is
# not wasted.
#
#   tools/android_precheck.sh            # check what changed since the merge base
#   tools/android_precheck.sh --all      # check every Android source in the tree
#
# Exit code 0: nothing found. 1: at least one finding, each with its file and line.

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
exec python3 "$ROOT/tools/android_precheck.py" "$@"
