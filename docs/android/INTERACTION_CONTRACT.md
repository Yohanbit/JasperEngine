# The Android interaction contract

What a person does on the phone, and what the log must say afterwards. This exists because "it still
works" is not a test, and because two changes in this port broke something that had worked — the
inset that shifted every tap, and the keyboard call that emptied the field it was meant to help —
and both looked fine in a green build.

**Run this after every APK, in one pass.** Each row is one action and the line it must leave behind.
A row that stops producing its line is a regression, and the log names it.

## The pass

| # | Do this | The log must show | A failure looks like |
|---|---|---|---|
| 1 | open the app | `status bar visible: false` and `navigation bar visible: false` | the bars are on screen |
| 2 | New Project, name it, Create | `New project: '<name>' into <path>`, then `Project loaded successfully` | `ignored: a creation is still running` |
| 3 | tap the name field, type, press backspace | the field loses **one** character per press | nothing goes, or the whole field empties |
| 4 | drag a panel by its title bar | the panel follows the finger | it jumps to the cursor, or does not move |
| 5 | open the assets screen | the asset list, no `crash.txt` | the app closes |
| 6 | press Play | `Scene '<name>' started`, then sound | `isLongTimeZeroData` in logcat |
| 7 | press Stop, then tap a menu | the editor answers at once | a freeze of a second or more |
| 8 | drag a divider between two panels | the divider moves, the layout follows | nothing moves |

Rows 3, 4 and 8 are the ones a build breaks without noticing, because they are the ones ImGui does
not report: a click still registers and a field still takes text while the *drag* is dead.

## What the log lines are, and why they are these

| Line | Written by | It answers |
|---|---|---|
| `status bar visible:` | `EditorBootLog`, from `WindowInsets` | did the app actually ask for immersive, or only intend to |
| `New project: '<name>' into <path>` | `App::hubCreateProject` | was Create heard, and where is it writing |
| `New project ignored: <why>` | the same, three early returns | Create was heard and refused, and why |
| `[start] first drawn frame: N ms` | the engine's start-up timing | how long the black screen lasts |
| `Scene '<name>' started` | `Project` | Play reached the scene |
| `[selftest] ...` (planned) | the Android self-test | the machine-checked rows, without a human |

Any line above changing shape — its text, its fields, its order — is a change to this contract and
belongs in the same commit that changes it.

## Why the checker cannot replace this

`tools/android_precheck.py` catches a wrong class qualifier, an unbalanced brace and an unguarded
API 30 call in seconds. It cannot catch a wrong *value*: the drag fix was four lines and every one of
them compiled. The rows above are the only thing that can, and row 4 in particular is the one that
was missed for two builds.
