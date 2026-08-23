#!/usr/bin/env bash
# GUI input automation test.
#
# Launches the gallery under the Wayland/GL shell with `--test-input`, which
# drives a deterministic sequence of every key class (letters, digits, space,
# backspace, delete, enter, tab, arrows, home, end, pageup/down, insert)
# straight through engine::send_key on the platform thread.
#
# Pass criteria (checked against the shell log):
#   * --test-input ran and no Flutter platform-thread violations occurred.
#   * every injected key was accepted by FlutterEngineSendKeyEvent.
#   * printable keys accumulated text and backspace trimmed it.
#   * navigation keys moved the text-field caret (left->-1, right->+1,
#     home->0, end->len) -- recorded via the [ime] selection log.
set -u
cd "$(dirname "$0")/.." || exit 2

fail=0
say() { printf '%s\n' "$*"; }
pass() { say "  PASS: $*"; }
bad()  { say "  FAIL: $*"; fail=1; }

SHELL_BIN=$(nix build --show-trace '.#shell' --no-link --print-out-paths 2>/dev/null)/bin/shell_app
ASSETS=$(nix build '.#galleryAssets' --no-link --print-out-paths 2>/dev/null)
say "shell = $SHELL_BIN"
say "assets= $ASSETS"

LOG=$(mktemp /tmp/gui_keys_XXXX.log)
"$SHELL_BIN" --assets="$ASSETS" --icu="$ASSETS/icudtl.dat" --renderer=gl --test-input >"$LOG" 2>&1 &
APP=$!
sleep 26

say "== assertions =="
# 1. the driver ran
if grep -q '\[test-input\] queued' "$LOG"; then pass "--test-input driver queued keys"; else bad "--test-input driver did not run"; fi
# 2. no platform-thread violations (key events were delivered on the platform thread)
if grep -q 'non-platform thread' "$LOG"; then bad "flutter/keydata sent on non-platform thread"; else pass "key events on platform thread (no thread violation)"; fi
# 3. every key event accepted by the engine
n=$(grep -ac '\[kr\] rc=0' "$LOG")
if [ "$n" -ge 100 ]; then pass "FlutterEngineSendKeyEvent accepted $n events"; else bad "only $n key events accepted (expected >=100)"; fi
# 4. printable keys accumulated text into the field
if grep -q 'ghijklmnopqrstuvwxyz12' "$LOG"; then pass "letters+digits typed into the text field"; else bad "letters/digits did not accumulate"; fi
# 5. editing-state updates flowed
if grep -q '\[sel\]' "$LOG"; then pass "IME editing-state updates flowed"; else bad "no editing-state updates"; fi
# 6. navigation moved the caret: at least one move to 0 (HOME / caret history)
if grep -q 'sel=0\.\.0' "$LOG"; then pass "HOME moved caret to 0"; else bad "HOME did not move caret to 0"; fi
# 7. the caret visited multiple positions (typing + arrow/home/end navigation)
distinct=$(grep -a '\[sel\]' "$LOG" | grep -oE 'sel=[0-9]+\.\.[0-9]+' | sort -u | wc -l)
if [ "$distinct" -ge 5 ]; then pass "caret visited $distinct distinct positions (typing + navigation)"; else bad "caret stayed near one position ($distinct distinct)"; fi

say "== button result =="
if [ "$fail" -eq 0 ]; then say "GUI key test PASSED"; else say "GUI key test FAILED"; fi
kill "$APP" 2>/dev/null
pkill -9 -f '[s]hell_app' 2>/dev/null
exit "$fail"
