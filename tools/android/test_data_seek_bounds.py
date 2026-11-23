#!/usr/bin/env python3
"""Check that engine/core/io/Data.cpp still has the bounds the test models, and run that test.

The test file copies three methods because linking the real Data.cpp would pull in File,
ResourcePack and the whole io layer for three functions. A copy can drift, and a copy that drifts
is worse than no test: it passes while the real code crashes. So this extracts the real bodies and
asserts the properties that make them safe, then compiles and runs the test with ASan and UBSan.
"""
import re
import subprocess
import sys


def body(source: str, signature: str) -> str:
    start = source.index(signature)
    depth = 0
    for index in range(start, len(source)):
        char = source[index]
        if char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise SystemExit(f"unterminated body for {signature}")


def strip_comments(text: str) -> str:
    """Only the code matters: the comments in Data.cpp quote the old, broken expressions."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def main() -> int:
    source = strip_comments(open('engine/core/io/Data.cpp', encoding='utf-8').read())
    read_body = body(source, 'unsigned int Data::read(unsigned char *aDst, unsigned int aBytes)')
    seek_body = body(source, 'void Data::seek(int aOffset)')
    problems = []

    # -- Data::read ---------------------------------------------------------------------------
    # the guard must be before any arithmetic and must reject a null buffer and a cursor at the end
    if 'dataPtr == nullptr' not in read_body:
        problems.append('Data::read does not reject a null dataPtr (the empty-Data SEGV)')
    if 'aDst == nullptr' not in read_body:
        problems.append('Data::read does not reject a null destination')
    if 'offset >= dataLength' not in read_body:
        problems.append('Data::read does not reject a cursor at or past the end')
    if re.search(r'offset\s*\+\s*aBytes\s*>=', read_body):
        problems.append('Data::read still adds offset + aBytes, which wraps for the remaining count')
    if 'dataLength - offset' not in read_body:
        problems.append('Data::read no longer computes the available count')
    if 'aBytes > available' not in read_body:
        problems.append('Data::read does not clamp to the available bytes')
    # the guard has to come first: a clamp before the null check would still dereference
    guard_at = read_body.find('dataPtr == nullptr')
    copy_at = read_body.find('memcpy')
    if guard_at >= 0 and copy_at >= 0 and guard_at > copy_at:
        problems.append('Data::read checks the pointer only after memcpy')

    # -- Data::seek ---------------------------------------------------------------------------
    if re.search(r'offset\s*=\s*dataLength\s*\+\s*aOffset', seek_body):
        problems.append('Data::seek still adds to the unsigned dataLength (the negative-seek wrap)')
    if 'long long' not in seek_body:
        problems.append('Data::seek does not widen to a signed type')
    if re.search(r'target\s*<\s*0', seek_body) is None:
        problems.append('Data::seek has no lower bound')
    if re.search(r'target\s*>\s*static_cast<long long>\(dataLength\)', seek_body) is None:
        problems.append('Data::seek does not clamp the upper bound to dataLength')
    if re.search(r'dataLength\s*-\s*1', seek_body):
        problems.append('Data::seek still clamps against dataLength - 1')

    for problem in problems:
        print(f"data-bounds: FAIL: {problem}")

    compile_result = subprocess.run(
        ['g++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-o', '/tmp/data_seek_bounds_test',
         'engine/tests/core/io/DataSeekBoundsTest.cpp'],
        capture_output=True, text=True)
    if compile_result.returncode != 0:
        print(compile_result.stderr[:2000], file=sys.stderr)
        print('data-bounds: FAIL: the test does not compile', file=sys.stderr)
        return 1

    run = subprocess.run(['/tmp/data_seek_bounds_test'], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode != 0:
        print(run.stderr[:2000], file=sys.stderr)
        print('data-bounds: FAIL: the test aborts under ASan/UBSan', file=sys.stderr)
        return 1
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
