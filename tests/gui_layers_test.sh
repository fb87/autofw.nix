#!/usr/bin/env bash
# GUI layer compositing automation test.
#
# Launches the gallery under the standalone test_layers binary (shell_tests::
# run_layer_test, see tests/shell_test_drivers.hpp), which drives a
# deterministic sequence of add_layer/set_layer_visible/
# set_layer_opacity/remove_layer calls straight through the engine's
# shell::layer registry. Each change pushes an "update" method call on the
# 'shell/layers' channel to the gallery's ShellLayerHost widget
# (flutter/lib/layer_host.dart), which debugPrint()s one "[layer] ..."
# line per layer in the new snapshot.
#
# Pass criteria (checked against the log):
#   * the driver ran and completed its scripted sequence.
#   * the background/application/overlay layers were all added and reached
#     Dart (push_layers -> ShellLayerHost._onMethodCall -> debugPrint).
#   * the application layer's visibility toggled off then back on.
#   * the overlay layer's opacity stepped through 0.25/0.5/0.75/1.0.
#   * the background layer was removed (absent from the final snapshot).
set -u
cd "$(dirname "$0")/.." || exit 2

fail=0
say() { printf '%s\n' "$*"; }
pass() { say "  PASS: $*"; }
bad()  { say "  FAIL: $*"; fail=1; }

TEST_BIN=$(nix build --show-trace '.#shell' --no-link --print-out-paths 2>/dev/null)/bin/test_layers
ASSETS=$(nix build '.#galleryAssets' --no-link --print-out-paths 2>/dev/null)
say "test_layers = $TEST_BIN"
say "assets= $ASSETS"

LOG=$(mktemp /tmp/gui_layers_XXXX.log)
"$TEST_BIN" --assets="$ASSETS" --icu="$ASSETS/icudtl.dat" --renderer=gl >"$LOG" 2>&1 &
APP=$!
sleep 10

say "== assertions =="
# 1. the driver ran to completion
if grep -q '\[test-layers\] add/visibility/opacity/remove sequence done' "$LOG"; then
  pass "--test-layers driver completed its scripted sequence"
else
  bad "--test-layers driver did not complete"
fi
# 2. every added layer reached Dart via the 'shell/layers' channel
for id_role in '1 background' '2 application' '3 overlay'; do
  id=${id_role% *}; role=${id_role#* }
  if grep -q "\\[layer\\] id=$id role=$role" "$LOG"; then
    pass "layer $id ($role) reached Dart"
  else
    bad "layer $id ($role) never reached Dart"
  fi
done
# 3. the application layer's visibility toggled off then back on
if grep -q '\[layer\] id=2 role=application visible=false' "$LOG"; then
  pass "application layer visibility toggled off"
else
  bad "application layer visibility never went false"
fi
if grep -q '\[layer\] id=2 role=application visible=true' "$LOG"; then
  pass "application layer visibility restored"
else
  bad "application layer visibility never restored"
fi
# 4. the overlay layer's opacity stepped through all four values
opacity_steps_ok=1
for v in 0.25 0.50 0.75 1.00; do
  grep -q "\\[layer\\] id=3 role=overlay visible=true opacity=$v" "$LOG" || opacity_steps_ok=0
done
if [ "$opacity_steps_ok" -eq 1 ]; then
  pass "overlay layer opacity stepped through 0.25/0.50/0.75/1.00"
else
  bad "overlay layer did not step through all opacity values"
fi
# 5. the background layer was removed: after the last time id=1 was ever
#    pushed, a later snapshot push (still containing layers 2 and 3) must
#    have arrived with id=1 gone -- i.e. remove_layer's push_layers landed.
done_line=$(grep -n 'sequence done' "$LOG" | tail -1 | cut -d: -f1)
last_bg_line=$(grep -n '\[layer\] id=1 ' "$LOG" | tail -1 | cut -d: -f1)
after_bg=$(sed -n "$((last_bg_line+1)),${done_line}p" "$LOG")
if printf '%s\n' "$after_bg" | grep -q '\[layer\] id=1 '; then
  bad "background layer (id=1) reappeared after its last removal push"
elif printf '%s\n' "$after_bg" | grep -q '\[layer\] id=2 ' && printf '%s\n' "$after_bg" | grep -q '\[layer\] id=3 '; then
  pass "background layer (id=1) absent from the post-remove snapshot (layers 2,3 still present)"
else
  bad "no post-remove snapshot (layers 2,3) seen after the background layer's last appearance"
fi

say "== button result =="
if [ "$fail" -eq 0 ]; then say "GUI layer test PASSED"; else say "GUI layer test FAILED"; fi
kill "$APP" 2>/dev/null
pkill -9 -f '[t]est_layers' 2>/dev/null
exit "$fail"
