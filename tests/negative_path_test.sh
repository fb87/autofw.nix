#!/usr/bin/env bash
# Negative-path robustness test.
#
# Confirms the shell fails CLEANLY (nonzero exit, no signal, a readable
# error message) on three invalid-input cases that previously had zero
# automated coverage:
#   1. --assets pointing at a nonexistent directory (no kernel_blob.bin to
#      load) -- exercises engine::init()'s FlutterEngineInitialize failure
#      path.
#   2. a malformed numeric flag (--width=bogus) -- previously an uncaught
#      std::invalid_argument from std::stoul inside parse_args, crashing
#      the process instead of printing a usage error.
#   3. no Wayland compositor reachable (bad WAYLAND_DISPLAY) -- exercises
#      backend::wayland::connect's failure path.
#
# "Clean failure" here means: process exits via a normal return from
# main() (WIFEXITED, not WIFSIGNALED), with a nonzero status -- not a
# crash, not a hang.
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

check_clean_failure() {
  local name="$1" expect_log="$2" log="$3" status="$4"
  if [ "$status" -ge 128 ]; then
    bad "$name: terminated by signal (exit status $status), not a clean failure"
    return
  fi
  if [ "$status" -eq 0 ]; then
    bad "$name: exited 0 -- expected a clean nonzero failure"
    return
  fi
  if grep -qF "$expect_log" "$log"; then
    pass "$name: clean nonzero exit ($status) with expected error message"
  else
    bad "$name: exited nonzero ($status) but expected log message '$expect_log' missing"
  fi
}

# 1. nonexistent --assets.
LOG1=$(mktemp /tmp/negpath_assets_XXXX.log)
timeout 10 "$SHELL_BIN" --assets=/nonexistent/path/xyz \
  --icu=/nonexistent/path/xyz/icudtl.dat --renderer=gl >"$LOG1" 2>&1
check_clean_failure "nonexistent --assets" "engine init failed" "$LOG1" "$?"

# 2. malformed numeric flag.
LOG2=$(mktemp /tmp/negpath_badflag_XXXX.log)
"$SHELL_BIN" --child-renderer --width=bogus --height=100 --ipc-fd=3 --input-fd=4 \
  >"$LOG2" 2>&1
check_clean_failure "malformed --width" "bad value for --width" "$LOG2" "$?"

# 3. no reachable Wayland compositor.
LOG3=$(mktemp /tmp/negpath_wayland_XXXX.log)
WAYLAND_DISPLAY=nonexistent-socket-xyz timeout 10 "$SHELL_BIN" \
  --assets="$ASSETS" --icu="$ASSETS/icudtl.dat" --renderer=gl >"$LOG3" 2>&1
check_clean_failure "unreachable Wayland compositor" "wayland connect failed" "$LOG3" "$?"

say "== button result =="
if [ "$fail" -eq 0 ]; then say "negative-path test PASSED"; else say "negative-path test FAILED"; fi
pkill -9 -f '[s]hell_app' 2>/dev/null
exit "$fail"
