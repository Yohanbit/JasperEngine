#!/usr/bin/env python3
"""The --gradle output, which names the APK and named nothing (CI run 45112874466).

Two defects, both invisible until a build finally reached Gradle:

  * the flags were emitted without their -P, so Gradle did not read them as properties at all. It
    treats a bare `name=value` as a TASK NAME: "Task 'jasperReleaseName=candidate-86430b3
    jasperBuildNumber=9 jasperCommit=...' not found in root project 'androidstudio'".
  * all five were on one line. tools/build.sh reads them with `mapfile`, which splits on newlines,
    so the five arrived as one argument -- the whole string would have been a single -P even with
    the prefix.

Neither is catchable by looking at a passing build, and neither is a device problem: they are the
reason a release build could not be produced at all. This checks the shape, and the two properties
that make the shape usable.
"""

import subprocess
import sys


def run(*args: str) -> str:
    result = subprocess.run([sys.executable, 'tools/android/release_identity.py', *args],
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        raise SystemExit(f'release_identity.py {args} failed')
    return result.stdout


def main() -> int:
    problems = []
    out = run('--gradle')
    lines = [line for line in out.splitlines() if line.strip()]

    if len(lines) != 5:
        problems.append(f'expected 5 lines (one per -P), got {len(lines)}: {lines!r}')

    for line in lines:
        if not line.startswith('-P'):
            problems.append(f'line without its -P (Gradle would read it as a task name): {line!r}')
        if '=' not in line:
            problems.append(f'line without an assignment: {line!r}')
        if ' ' in line:
            problems.append(f'line with a space (mapfile keeps it whole, but -P values must not): {line!r}')

    expected = {'jasperReleaseName', 'jasperBuildNumber', 'jasperCommit', 'jasperVersionCode', 'jasperDirty'}
    found = {line.split('=', 1)[0].removeprefix('-P') for line in lines if '=' in line}
    if found != expected:
        problems.append(f'properties differ: missing {sorted(expected - found)}, extra {sorted(found - expected)}')

    # The JSON form is what the rest of the tooling reads; --gradle must agree with it.
    ident = run('--json')
    import json
    identity = json.loads(ident)
    by_name = {line.split('=', 1)[0].removeprefix('-P'): line.split('=', 1)[1] for line in lines if '=' in line}
    if by_name.get('jasperCommit') != identity.get('commit'):
        problems.append(f"--gradle commit {by_name.get('jasperCommit')!r} != --json {identity.get('commit')!r}")
    if by_name.get('jasperVersionCode') != str(identity.get('versionCode')):
        problems.append('--gradle versionCode disagrees with --json')
    if by_name.get('jasperDirty') not in ('true', 'false'):
        problems.append(f"jasperDirty is {by_name.get('jasperDirty')!r}, not true/false")

    for problem in problems:
        print(f'release-identity: FAIL: {problem}')
    if not problems:
        print(f'release-identity: {len(lines)} -P flags, one per line, agreeing with --json')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
