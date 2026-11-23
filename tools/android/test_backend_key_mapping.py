#!/usr/bin/env python3
"""Compile and run the pure key/character logic of BackendAndroid.cpp on the host.

The Android half of that file needs GameActivity, EGL and JNI headers this machine does not
have, so nothing there can be compiled. What *can* be wrong without a device is the mapping
itself -- which Android keycode becomes which ImGuiKey, and which character a hardware keyboard
key stands for -- and that is plain arithmetic. This extracts those two functions verbatim from
the real file, compiles them against stubs that model the parts they touch, and prints what a
sequence of key events produces. It is the test the release notes point at for the input fixes.
"""
import re
import subprocess
import sys

SOURCE = 'editor/backend/BackendAndroid.cpp'


def extract():
    src = open(SOURCE, encoding='utf-8').read()
    start = src.index('        ImGuiKey imGuiKeyFromAndroid(int32_t keycode){')
    end = src.index('        // GameActivity\'s glue filters', start)
    block = src[start:end]
    # the clipboard half sits between the character map and the filters in some revisions
    if '        // -- the clipboard' in block:
        block = block[:block.index('        // -- the clipboard')]
    return block


# Android KeyEvent keycodes and meta flags (frameworks/base KeyEvent.java); spelled out so this
# harness does not need the SDK, and checked against the file's own use of them.
KEYCODES = {
    'AKEYCODE_A': 29, 'AKEYCODE_B': 30, 'AKEYCODE_C': 31, 'AKEYCODE_D': 32, 'AKEYCODE_E': 33,
    'AKEYCODE_F': 34, 'AKEYCODE_G': 35, 'AKEYCODE_H': 36, 'AKEYCODE_I': 37, 'AKEYCODE_J': 38,
    'AKEYCODE_K': 39, 'AKEYCODE_L': 40, 'AKEYCODE_M': 41, 'AKEYCODE_N': 42, 'AKEYCODE_O': 43,
    'AKEYCODE_P': 44, 'AKEYCODE_Q': 45, 'AKEYCODE_R': 46, 'AKEYCODE_S': 47, 'AKEYCODE_T': 48,
    'AKEYCODE_U': 49, 'AKEYCODE_V': 50, 'AKEYCODE_W': 51, 'AKEYCODE_X': 52, 'AKEYCODE_Y': 53,
    'AKEYCODE_Z': 54, 'AKEYCODE_0': 7, 'AKEYCODE_1': 8, 'AKEYCODE_2': 9, 'AKEYCODE_3': 10,
    'AKEYCODE_4': 11, 'AKEYCODE_5': 12, 'AKEYCODE_6': 13, 'AKEYCODE_7': 14, 'AKEYCODE_8': 15,
    'AKEYCODE_9': 16, 'AKEYCODE_COMMA': 55, 'AKEYCODE_PERIOD': 56, 'AKEYCODE_MINUS': 69,
    'AKEYCODE_EQUALS': 70, 'AKEYCODE_LEFT_BRACKET': 71, 'AKEYCODE_RIGHT_BRACKET': 72,
    'AKEYCODE_BACKSLASH': 73, 'AKEYCODE_SEMICOLON': 74, 'AKEYCODE_APOSTROPHE': 75,
    'AKEYCODE_SLASH': 76, 'AKEYCODE_GRAVE': 68, 'AKEYCODE_STAR': 17, 'AKEYCODE_POUND': 18,
    'AKEYCODE_NUMPAD_0': 144, 'AKEYCODE_NUMPAD_1': 145, 'AKEYCODE_NUMPAD_2': 146,
    'AKEYCODE_NUMPAD_3': 147, 'AKEYCODE_NUMPAD_4': 148, 'AKEYCODE_NUMPAD_5': 149,
    'AKEYCODE_NUMPAD_6': 150, 'AKEYCODE_NUMPAD_7': 151, 'AKEYCODE_NUMPAD_8': 152,
    'AKEYCODE_NUMPAD_9': 153, 'AKEYCODE_NUMPAD_DOT': 158, 'AKEYCODE_NUMPAD_COMMA': 159,
    'AKEYCODE_NUMPAD_DIVIDE': 154, 'AKEYCODE_NUMPAD_MULTIPLY': 155,
    'AKEYCODE_NUMPAD_SUBTRACT': 156, 'AKEYCODE_NUMPAD_ADD': 157, 'AKEYCODE_NUMPAD_EQUALS': 161,
    'AKEYCODE_SHIFT_LEFT': 59, 'AKEYCODE_SHIFT_RIGHT': 60, 'AKEYCODE_CTRL_LEFT': 113,
    'AKEYCODE_CTRL_RIGHT': 114, 'AKEYCODE_ALT_LEFT': 57, 'AKEYCODE_ALT_RIGHT': 58,
    'AKEYCODE_META_LEFT': 117, 'AKEYCODE_META_RIGHT': 118, 'AKEYCODE_INSERT': 124,
    'AKEYCODE_FORWARD_DEL': 112, 'AKEYCODE_DEL': 67, 'AKEYCODE_ENTER': 66,
    'AKEYCODE_ESCAPE': 111, 'AKEYCODE_TAB': 61, 'AKEYCODE_SPACE': 62,
    'AKEYCODE_DPAD_LEFT': 21, 'AKEYCODE_DPAD_RIGHT': 22, 'AKEYCODE_DPAD_UP': 19,
    'AKEYCODE_DPAD_DOWN': 20, 'AKEYCODE_MOVE_HOME': 122, 'AKEYCODE_MOVE_END': 123,
    'AKEYCODE_PAGE_UP': 92, 'AKEYCODE_PAGE_DOWN': 93,
    'AMETA_SHIFT_ON': 1, 'AMETA_SHIFT_LEFT_ON': 0x40, 'AMETA_CAPS_LOCK_ON': 0x100000,
}

body = extract()
keys = sorted(set(re.findall(r'ImGuiKey_[A-Za-z0-9]+', body)) - {'ImGuiKey_None'})
enum = 'enum ImGuiKey { ImGuiKey_None = 0, ' + ', '.join(keys) + ' };'
defs = '\n'.join(f'constexpr int {name} = {value};' for name, value in KEYCODES.items())

program = f'''
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
{enum}
{defs}
struct GameActivityKeyEvent {{ int32_t keyCode; int32_t metaState; int32_t action; int32_t repeatCount; int32_t flags; }};
struct ImGuiIO_t {{
    std::string chars;
    void AddInputCharacter(unsigned int c) {{ chars.push_back(static_cast<char>(c)); }}
}};
inline ImGuiIO_t& GetIO() {{ static ImGuiIO_t io; return io; }}
namespace ImGui {{ inline ImGuiIO_t& GetIO() {{ return ::GetIO(); }} }}
using ImGuiIO = ImGuiIO_t;

{body}

static void type(int keycode, int meta = 0, const char* label = "") {{
    GameActivityKeyEvent e{{keycode, meta, 0, 0, 0}};
    addImGuiCharacterFor(&e);
    if (label[0]) std::printf("%-22s -> %s\\n", label, GetIO().chars.c_str());
    GetIO().chars.clear();
}}

int main() {{
    int failures = 0;
    auto expect = [&](const char* what, int key, int meta, const char* want) {{
        GameActivityKeyEvent e{{key, meta, 0, 0, 0}};
        addImGuiCharacterFor(&e);
        const std::string got = GetIO().chars;
        GetIO().chars.clear();
        if (got != want) {{ std::printf("FAIL %s: expected [%s] got [%s]\\n", what, want, got.c_str()); ++failures; }}
    }};

    // letters, with and without the modifiers a hardware keyboard reports
    expect("lowercase a", AKEYCODE_A, 0, "a");
    expect("Shift+b", AKEYCODE_B, AMETA_SHIFT_ON, "B");
    expect("CapsLock c", AKEYCODE_C, AMETA_CAPS_LOCK_ON, "C");
    expect("CapsLock+Shift d", AKEYCODE_D, AMETA_CAPS_LOCK_ON | AMETA_SHIFT_ON, "d");
    expect("ShiftLeft flag e", AKEYCODE_E, AMETA_SHIFT_LEFT_ON, "E");
    // digits and their shifted symbols
    expect("digit 1", AKEYCODE_1, 0, "1");
    expect("Shift+1", AKEYCODE_1, AMETA_SHIFT_ON, "!");
    expect("Shift+0", AKEYCODE_0, AMETA_SHIFT_ON, ")");
    // punctuation
    expect("comma", AKEYCODE_COMMA, 0, ",");
    expect("Shift+comma", AKEYCODE_COMMA, AMETA_SHIFT_ON, "<");
    expect("period", AKEYCODE_PERIOD, 0, ".");
    expect("Shift+period", AKEYCODE_PERIOD, AMETA_SHIFT_ON, ">");
    expect("slash", AKEYCODE_SLASH, 0, "/");
    expect("Shift+slash", AKEYCODE_SLASH, AMETA_SHIFT_ON, "?");
    expect("apostrophe", AKEYCODE_APOSTROPHE, 0, "'");
    expect("Shift+apostrophe", AKEYCODE_APOSTROPHE, AMETA_SHIFT_ON, "\\"");
    expect("minus", AKEYCODE_MINUS, 0, "-");
    expect("Shift+minus", AKEYCODE_MINUS, AMETA_SHIFT_ON, "_");
    // editing keys that a text field needs as characters
    expect("backspace", AKEYCODE_DEL, 0, "\\b");
    expect("enter", AKEYCODE_ENTER, 0, "\\r");
    expect("tab", AKEYCODE_TAB, 0, "\\t");
    expect("space", AKEYCODE_SPACE, 0, " ");
    // a key with no character adds nothing
    expect("shift alone", AKEYCODE_SHIFT_LEFT, 0, "");
    expect("f5 (unmapped)", 131, 0, "");

    // the key map: every letter and digit must map, so the event filter lets them through
    int unmapped = 0;
    for (int k = AKEYCODE_A; k <= AKEYCODE_Z; ++k)
        if (imGuiKeyFromAndroid(k) == ImGuiKey_None) {{ std::printf("FAIL letter %d unmapped\\n", k); ++unmapped; }}
    for (int k = AKEYCODE_0; k <= AKEYCODE_9; ++k)
        if (imGuiKeyFromAndroid(k) == ImGuiKey_None) {{ std::printf("FAIL digit %d unmapped\\n", k); ++unmapped; }}
    failures += unmapped;

    std::printf("character+map checks: %d failure(s)\\n", failures);
    return failures == 0 ? 0 : 1;
}}
'''

open('/tmp/backend_android_keys.cpp', 'w', encoding='utf-8').write(program)
compile_result = subprocess.run(
    ['g++', '-std=c++20', '-Wall', '-o', '/tmp/backend_android_keys', '/tmp/backend_android_keys.cpp'],
    capture_output=True, text=True)
if compile_result.returncode != 0:
    print(compile_result.stderr[:4000])
    sys.exit(1)
run = subprocess.run(['/tmp/backend_android_keys'], capture_output=True, text=True)
print(run.stdout)
sys.exit(run.returncode)
