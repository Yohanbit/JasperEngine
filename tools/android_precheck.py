#!/usr/bin/env python3
"""Fast checks for the Android port, before a build is worth spending 45 minutes on.

Two failures reached CI that no compiler was needed to find:

  * WindowInsetsCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE -- the constant lives on
    WindowInsetsControllerCompat. The build said "cannot find symbol" and nothing else;
    finding the line took a four-minute round trip.
  * a brace balance broken by a scripted edit, which the compiler reports hundreds of
    lines away, in another function.

The real build stays the authority. This only makes sure the build is not wasted.

    tools/android_precheck.py            # what changed since the merge base
    tools/android_precheck.py --all      # every Android source in the tree

Exit 0: clean. Exit 1: findings, each with file and line.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Symbols whose owner a reader gets wrong, because the wrong owner also compiles in a
# nearby context or because the name reads as if it belonged next to the other one.
OWNED_SYMBOLS = {
    "BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE": "WindowInsetsControllerCompat",
    "BEHAVIOR_SHOW_BARS_BY_SWIPE": "WindowInsetsControllerCompat",
    "BEHAVIOR_DEFAULT": "WindowInsetsControllerCompat",
    "BEHAVIOR_SHOW_BARS_BY_TOUCH": "WindowInsetsControllerCompat",
    "isAppearanceLightStatusBars": "WindowInsetsControllerCompat",
    "Type": "WindowInsetsCompat",
}

# API 30+ calls on a minSdk 24 app need a guard, or lint fails the build.
API_30_SYMBOLS = [
    r"WindowInsets\.Type",
    r"VERSION_CODES\.R\b",
    r"VERSION_CODES\.S\b",
    r"setDecorFitsSystemWindows\b(?!.*WindowCompat)",
]

GUARD = re.compile(r"SDK_INT\s*>=\s*(29|30|31|32|33|34)|VERSION_CODES\.(Q|R|S|TIRAMISU)")


def strip_comments_and_literals(src: str) -> str:
    """The source with //, /* */ and string/char literals blanked.

    A checker that counts braces with tr/grep counts the ones in a string too: on the
    engine's own test files that is 600 findings in one run, and a checker that cries wolf
    is worse than none.
    """
    out: list[str] = []
    i, n = 0, len(src)
    state = None  # None / "line" / "block" / the quote character
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/":
                state = "line"
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                i += 2
                continue
            # A C++ digit separator is an apostrophe between digits (1'000'000), not the
            # start of a literal; reading it as one swallows the rest of the line and makes
            # the brace count meaningless. Four of them in Project.cpp alone.
            if c == "'" and i > 0 and src[i - 1].isdigit() and nxt.isdigit():
                out.append(c)
                i += 1
                continue
            # R"delim( ... )delim" is a raw string: no escapes, and the delimiter is chosen so
            # that no )delim" appears inside it.
            raw = re.match(r'R"([^()\\ ]{0,16})\(', src[i:i + 20])
            if c == "R" and raw:
                close = ")" + raw.group(1) + '"'
                end = src.find(close, i + raw.end())
                i = (end + len(close)) if end != -1 else n
                out.append('""')
                continue
            if c in ("'", '"'):
                state = c
                i += 1
                continue
            out.append(c)
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = None
                out.append(c)
            i += 1
            continue
        if state == "block":
            if c == "*" and nxt == "/":
                state = None
                i += 2
                continue
            i += 1
            continue
        # inside a literal
        if c == "\\":
            i += 2
            continue
        if c == state:
            state = None
        i += 1
    return "".join(out)


def changed_files(all_files: bool) -> tuple[list[Path], list[Path]]:
    if all_files:
        # The port's own files, and the engine's non-vendored ones. engine/libs is upstream code
        # nobody here edits, and the tests are not shipped: a finding in them is noise, and a
        # checker that is noisy about files you did not touch gets turned off.
        native = sorted([*ROOT.glob("editor/**/*.cpp"), *ROOT.glob("editor/**/*.h"),
                         *ROOT.glob("engine/core/**/*.cpp"), *ROOT.glob("engine/core/**/*.h"),
                         *ROOT.glob("engine/platform/**/*.cpp"), *ROOT.glob("engine/platform/**/*.h")])
        native = [p for p in native if "/tests/" not in str(p) and "/libs/" not in str(p)]
        java = sorted(ROOT.glob("engine/workspaces/androidstudio/**/*.java"))
        return native, java

    for base in ("origin/HEAD", "HEAD~1"):
        try:
            out = subprocess.run(["git", "diff", "--name-only", base],
                                 cwd=ROOT, capture_output=True, text=True, check=True).stdout
            break
        except subprocess.CalledProcessError:
            continue
    else:
        return [], []

    native, java = [], []
    for name in out.split():
        path = ROOT / name
        if not path.is_file():
            continue
        if name.endswith(".java"):
            java.append(path)
        elif name.endswith((".cpp", ".h")) and name.split("/")[0] in ("editor", "engine"):
            native.append(path)
    return native, java


def main() -> int:
    all_files = "--all" in sys.argv[1:]
    native, java = changed_files(all_files)
    findings: list[str] = []

    def note(where: str, message: str) -> None:
        findings.append(f"{where}: {message}")

    if not native and not java:
        print("android precheck: nothing changed")
        return 0

    # ---- 1. balance
    for path in [*native, *java]:
        rel = path.relative_to(ROOT)
        code = strip_comments_and_literals(path.read_text(encoding="utf-8", errors="replace"))
        for opener, closer, name in (("{", "}", "braces"), ("(", ")", "parentheses")):
            if code.count(opener) != code.count(closer):
                note(str(rel), f"{name} {code.count(opener)} open, {code.count(closer)} close")
        if path.suffix in (".cpp", ".h"):
            ifs = len(re.findall(r"^\s*#if", code, re.M))
            endifs = len(re.findall(r"^\s*#endif", code, re.M))
            if ifs != endifs:
                note(str(rel), f"#if {ifs}, #endif {endifs}")

    # ---- 2. Java symbols and their owners
    for path in java:
        rel = path.relative_to(ROOT)
        for number, line in enumerate(path.read_text(encoding="utf-8",
                                                     errors="replace").splitlines(), 1):
            if line.lstrip().startswith(("//", "*")):
                continue
            for symbol, owner in OWNED_SYMBOLS.items():
                for match in re.finditer(rf"([A-Za-z_][\w.]*)\.{re.escape(symbol)}\b", line):
                    if match.group(1) != owner:
                        note(f"{rel}:{number}", f"{symbol} belongs to {owner}, not {match.group(1)}")

    # ---- 3. API 30+ without a guard
    for path in java:
        rel = path.relative_to(ROOT)
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        for number, line in enumerate(lines, 1):
            if line.lstrip().startswith(("//", "*")):
                continue
            if not any(re.search(symbol, line) for symbol in API_30_SYMBOLS):
                continue
            window = "\n".join(lines[max(0, number - 11):number])
            if not GUARD.search(window):
                note(f"{rel}:{number}", "API 30+ call without an SDK_INT guard (minSdk is 24)")

    # ---- 4. native syntax against the Android stubs, when they exist
    stub = Path("/tmp/stub")
    if stub.is_dir():
        for path in native:
            if path.suffix != ".cpp":
                continue
            proc = subprocess.run(
                ["g++", "-fsyntax-only", "-std=c++20", "-D__ANDROID__", f"-I{stub}",
                 f"-I{ROOT}", f"-I{ROOT / 'engine' / 'core'}", f"-I{ROOT / 'editor'}",
                 f"-I{ROOT / 'editor' / 'backend'}", str(path)],
                capture_output=True, text=True)
            errors = [l for l in proc.stderr.splitlines() if re.search(r"error:", l)]
            for error in errors[:3]:
                note(str(path.relative_to(ROOT)), error.strip())

    return report(findings)


BASELINE = ROOT / "tools" / "android_precheck_baseline.txt"


def load_baseline() -> list[str]:
    """Findings that were already there before this work started.

    The checker is for regressions in the port, not for the repository's existing debt: two
    editor files ship with an unbalanced brace today and have nothing to do with Android. A
    checker that reports them on every run is one people learn to ignore, and then it misses
    the finding that matters. Each line is a file name plus the finding's kind, with a note
    saying where it came from.
    """
    if not BASELINE.is_file():
        return []
    entries = []
    for line in BASELINE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            entries.append(line)
    return entries


def report(findings: list[str]) -> int:
    baseline = load_baseline()
    known = [f for f in findings if any(f.startswith(entry) for entry in baseline)]
    fresh = [f for f in findings if f not in known]

    for finding in known:
        print(f"  (known) {finding}")
    if not fresh:
        print("\nandroid precheck: clean" + (f" ({len(known)} known)" if known else ""))
        return 0
    for finding in fresh:
        print(f"  {finding}")
    print(f"\nandroid precheck: {len(fresh)} finding(s)" + (f", {len(known)} known" if known else ""))
    return 1


if __name__ == "__main__":
    sys.exit(main())
