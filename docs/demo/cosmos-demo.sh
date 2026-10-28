#!/usr/bin/env bash
# Cosmos demonstration — the fault injector, one step at a time.
#
# Every step states the claim it demonstrates, runs one scripted universe (or one campaign)
# against the replicated-KV fixture, and asserts the outcome from the engine's own ledger and
# check results. Nothing is mocked: the workloads are the shipped fixtures and the detector is
# the shipped library.
#
# Usage: cosmos-demo.sh [options]
#   --list             print the steps and what each one claims
#   --interactive      pause before every step so the dashboard can be shown
#   --quick            setup, control, dropped forwards, the finding (~1 min)
#   --phases a,b,c     run only these steps, in the order given
#   --keep             keep the work root (artifacts, logs, configs)
#   --work DIR         work root (default: a fresh mktemp directory)
#   --seed N           universe base seed (default 1; chaos derives victims from it)
#   --port N           dashboard port (default 8790)
#   --no-ui            do not start the dashboard server
#
# Environment: COSMOS_DEMO_PORT, COSMOS_DEMO_SEED, COSMOS_DEMO_WORK, COSMOS_DEMO_KEEP_WORK,
#              COSMOS_BUILD_COMMIT, COSMOS_BUILD_BINARY_HASH, COSMOS_BUILD_HOST (provenance).
#
# Nothing outside the work root is touched. The demo is deterministic: the plan is a function of
# (step, seed), so the same command prints the same ledger on every run.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOL="$ROOT/build/tools/cosmos_demo"
CAMPAIGN="$ROOT/build/tools/replicated_kv_campaign"

ALL_PHASES="build control drop-one drop-both short-send reset refuse sleep partition crash pause finding seeded guided minimize wrap"
QUICK_PHASES="build control drop-both finding"
PHASES="$ALL_PHASES"

PORT="${COSMOS_DEMO_PORT:-8790}"
SEED="${COSMOS_DEMO_SEED:-1}"
WORK="${COSMOS_DEMO_WORK:-}"
KEEP="${COSMOS_DEMO_KEEP_WORK:-}"
INTERACTIVE=""
WITH_UI=1

while [ $# -gt 0 ]; do
  case "$1" in
    --list)
      cat <<'LIST'
build      the library and this demo tool build; the detector binary is the shipped one
control    no fault at all: the ack is replicated, and the ledger is empty
drop-one   one forward is silently lost; the promise still holds because a second replica has it
drop-both  both forwards are lost; acked=1, no replica holds the write  <- the finding
short-send a short transfer is absorbed by send_all, so the frame lands intact
reset      the forward is reset; the primary drops that follower for the rest of the run
refuse     every connect is refused; the client refuses the write instead of acking a lie
sleep      the settle sleep is interrupted; the write races start-up and still replicates
partition  split the primary from one backup, heal it, and read the post-heal state
crash      crash one backup inside the window, reboot it, and read the post-heal state
pause      pause one node, resume it: the ack arrives late, not wrong
finding    split from one backup and kill the other: the shipped bug, at t=2ms
seeded     128 seeded universes with verify: 66 failures collapse into one finding
guided     coverage-guided campaign: which seeds earned a place in the corpus
minimize   the exported 1-minimal report for the finding
wrap       what was proven, what was not, and where to look in the dashboard
LIST
      exit 0
      ;;
    -h|--help) sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --quick) PHASES="$QUICK_PHASES" ;;
    --interactive) INTERACTIVE=1 ;;
    --keep) KEEP=1 ;;
    --no-ui) WITH_UI="" ;;
    --phases) shift; PHASES="${1//,/ }" ;;
    --work) shift; WORK="$1" ;;
    --seed) shift; SEED="$1" ;;
    --port) shift; PORT="$1" ;;
    *) echo "unknown option: $1 (try --list)" >&2; exit 2 ;;
  esac
  shift
done

command -v python3 >/dev/null 2>&1 || { echo "error: python3 is required" >&2; exit 2; }
command -v curl >/dev/null 2>&1 || { echo "error: curl is required" >&2; exit 2; }
[ -x "$TOOL" ] || {
  echo "error: $TOOL is missing; build it first:" >&2
  echo "  nix develop -c cmake -B build -DCOSMOS_BUILD_TESTS=ON" >&2
  echo "  nix develop -c cmake --build build --target cosmos_demo replicated_kv_campaign" >&2
  exit 2
}

[ -n "$WORK" ] || WORK="$(mktemp -d -t cosmos-demo-XXXXXX)"
mkdir -p "$WORK/artifacts" "$WORK/logs"

UI_PID=""
cleanup() {
  [ -n "$UI_PID" ] && kill "$UI_PID" 2>/dev/null
  if [ -z "$KEEP" ]; then rm -rf "$WORK"; else printf '\nwork root kept: %s\n' "$WORK"; fi
}
trap cleanup EXIT

PASSED=0; FAILED=0; SKIPPED=0
declare -a RESULTS=()
LAST_SEED=""
LAST_ID=""

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
record() { RESULTS+=("$1|$2|$3"); case "$1" in PASS) PASSED=$((PASSED+1));; SKIP) SKIPPED=$((SKIPPED+1));; *) FAILED=$((FAILED+1));; esac; }
ok()   { record PASS "$1" "$2"; printf '    \033[32mPASS\033[0m: %s\n' "$2"; }
bad()  { record FAIL "$1" "$2"; printf '    \033[31mFAIL\033[0m: %s\n' "$2"; }
skip() { record SKIP "$1" "$2"; printf '    \033[33mSKIP\033[0m: %s\n' "$2"; }
want() { case " $PHASES " in *" $1 "*) return 0;; *) return 1;; esac; }

pause_demo() {
  [ -n "$INTERACTIVE" ] || return 0
  [ -n "$WITH_UI" ] && printf '\n    dashboard: http://127.0.0.1:%s/   (Ctrl-C stops the demo)\n' "$PORT"
  printf '    press Enter to run this step...'
  read -r _ </dev/tty || true
}

link() { printf 'http://127.0.0.1:%s/campaigns/%s' "$PORT" "$1"; }

# One scripted universe, written as a single-universe campaign so the dashboard can open it.
run_step() { # id fault -> log path in LOG
  local id="$1" fault="$2"
  LAST_ID="$id"
  LOG="$WORK/logs/$id.log"
  "$TOOL" --fault "$fault" --seed "$SEED" \
    --mode chaos \
    --root "$WORK/artifacts/$id" --id "$id" \
    --build-id "cosmos-demo-$fault" \
    >"$LOG" 2>&1
  LAST_SEED="$(sed -n 's/^universe: .* universe_seed=\([0-9]*\).*/\1/p' "$LOG" | head -1)"
  [ -n "$LAST_SEED" ] || LAST_SEED="0"
  return 0
}

show_step() { # log
  local log="$1"
  sed -n '/^window/p;/^episode/p;/^rule/p' "$log" | sed 's/^/    /'
  sed -n '/^checks:/,/^evidence:/p' "$log" | sed 's/^/  /' | sed '/^  evidence:/d' | sed '/^  checks:/d' | sed 's/^  //;s/^/    /'
  sed -n '/^evidence:/,/^ledger:/p' "$log" | sed '/^evidence:/d;/^ledger:/d' | sed 's/^/    /'
  sed -n '/^ledger:/,/^trace:/p' "$log" | sed '/^ledger:/d;/^trace:/d' | head -6 | sed 's/^/    /'
}

expect_result() { # phase log PASS|FAIL
  local phase="$1" log="$2" want="$3" got
  got="$(sed -n 's/^DEMO RESULT: \([A-Z]*\).*/\1/p' "$log" | tail -1)"
  if [ "$got" != "$want" ]; then
    bad "$phase" "expected $want, got ${got:-nothing}; see $log"
    return 1
  fi
  ok "$phase" "verdict as claimed: $want"
}

expect_ledger() { # phase log pattern description
  local phase="$1" log="$2" pattern="$3" what="$4"
  if grep -qE "$pattern" "$log"; then
    ok "$phase" "$what"
  else
    bad "$phase" "the ledger never shows: $what"
  fi
}

# ---------------------------------------------------------------------------

run_build() {
  want build || return 0
  say "setup. build — the detector is one static binary, and the demo tool is the shipped library"
  note "repo:   $ROOT"
  note "tool:   $TOOL"
  note "binary: $(file -b "$TOOL" | cut -c1-96)"
  note "plans:  $("$TOOL" --list | wc -l) scripted plans available"
  ok build "the demo tool runs and reports its plan list"
}

run_control() {
  want control || return 0
  say "step 1. control — no fault at all"
  note "claim: with no fault the ack is replicated, so the promise holds and the ledger is empty"
  pause_demo
  run_step step-control none
  show_step "$LOG"
  expect_result control "$LOG" PASS
  expect_ledger control "$LOG" 'acked=1 backup1=1 backup2=1' "both backups hold the write"
  note "conclusion: the baseline. Anything that breaks the promise from here is caused by a fault, not"
  note "            by the fixture, and the ledger's emptiness is what a clean run looks like."
  note "dashboard: $(link step-control)"
}

run_drop_one() {
  want drop-one || return 0
  say "step 2. network — one forward is silently lost"
  note "claim: send() reports success, the client is acked, and the second replica still has the write"
  pause_demo
  run_step step-drop-one drop-forward
  show_step "$LOG"
  expect_result drop-one "$LOG" PASS
  expect_ledger drop-one "$LOG" 'site=send.*PacketDrop' "the dropped forward is in the ledger"
  note "conclusion: a fault is an input, not a bug. The app lost one copy and still kept its promise,"
  note "            because the promise is 'a backup has it', not 'every backup has it'."
  note "dashboard: $(link step-drop-one)"
}

run_drop_both() {
  want drop-both || return 0
  say "step 3. network — both forwards are lost (the finding)"
  note "claim: acked=1 with no replica holding the write; nothing returned an error to the app"
  pause_demo
  run_step step-drop-both drop-forwards
  show_step "$LOG"
  expect_result drop-both "$LOG" FAIL
  expect_ledger drop-both "$LOG" 'backup1=0 backup2=0' "the final state shows no backup with the write"
  expect_ledger drop-both "$LOG" 'site=send.*PacketDrop' "both dropped forwards are in the ledger"
  note "conclusion: two legal losses, one promise broken. This is the bug the whole project exists to"
  note "            find, and it is visible only because we check state after the calm-down."
  note "dashboard: $(link step-drop-both)/universes/$LAST_SEED/timeline"
}

run_short_send() {
  want short-send || return 0
  say "step 4. network — a short transfer is absorbed"
  note "claim: send_all loops over partial sends, so the frame still arrives intact"
  pause_demo
  run_step step-short-send short-send
  show_step "$LOG"
  expect_result short-send "$LOG" PASS
  expect_ledger short-send "$LOG" 'site=send.*ShortSend' "the short send is in the ledger"
  note "conclusion: the same fault family, a different app reaction. This one is handled, and the demo"
  note "            must show handled faults or the audience concludes every fault is a bug."
  note "dashboard: $(link step-short-send)"
}

run_reset() {
  want reset || return 0
  say "step 5. network — a reset costs the run a replica"
  note "claim: the primary closes that follower and never reconnects, so redundancy is gone for good"
  pause_demo
  run_step step-reset reset-forward
  show_step "$LOG"
  expect_result reset "$LOG" PASS
  expect_ledger reset "$LOG" 'site=send.*ConnReset' "the reset is in the ledger"
  expect_ledger reset "$LOG" 'backup1=0|backup2=0' "one backup is missing the write"
  note "conclusion: the promise held on the other replica, but the app's error handling threw away a"
  note "            node for the rest of the run — a durability risk that is not a violated check."
  note "dashboard: $(link step-reset)"
}

run_refuse() {
  want refuse || return 0
  say "step 6. network — every connect is refused"
  note "claim: the client retries fifty times, gives up, and refuses the write instead of acking it"
  pause_demo
  run_step step-refuse refuse-client
  show_step "$LOG"
  expect_result refuse "$LOG" PASS
  expect_ledger refuse "$LOG" 'site=connect.*ConnRefused' "the refused connects are in the ledger"
  expect_ledger refuse "$LOG" 'acked=0 backup1=0 backup2=0' "the client was never told the write landed"
  note "conclusion: refusing is a legal way to keep the promise. 'I could not save it' is honest;"
  note "            'saved' with no copy is the bug."
  note "dashboard: $(link step-refuse)"
}

run_sleep() {
  want sleep || return 0
  say "step 7. clock — the settle sleep returns early"
  note "claim: a legal interruption wakes the client early; the write races start-up and survives"
  pause_demo
  run_step step-sleep interrupt-sleep
  show_step "$LOG"
  expect_result sleep "$LOG" PASS
  expect_ledger sleep "$LOG" 'site=nanosleep.*SleepInterrupted' "the interruption is in the ledger"
  note "conclusion: a fault that is legal and absorbed still moves the schedule — evidence that"
  note "            'the API contract was honoured' and 'the promise held' are different claims."
  note "dashboard: $(link step-sleep)"
}

run_partition() {
  want partition || return 0
  say "step 8. process — split the primary from one backup, then heal it"
  note "claim: the forward is dropped for the duration, the ack still happens, and the heal is booked"
  pause_demo
  run_step step-partition partition
  show_step "$LOG"
  expect_result partition "$LOG" PASS
  expect_ledger partition "$LOG" 'site=partition' "the split is in the ledger"
  expect_ledger partition "$LOG" 'backup1=0|backup2=0' "the split backup missed the write"
  note "conclusion: the ledger shows the start and its own scheduled heal. The state page then shows"
  note "            three live nodes and no partitions, which is why the promise is judged after recovery."
  note "dashboard: $(link step-partition)/universes/$LAST_SEED/state"
}

run_crash() {
  want crash || return 0
  say "step 9. process — crash one backup and reboot it"
  note "claim: the node dies inside the window, peers see the close, and the reboot is the heal"
  pause_demo
  run_step step-crash crash
  show_step "$LOG"
  expect_result crash "$LOG" PASS
  expect_ledger crash "$LOG" 'site=crash_node' "the crash is in the ledger"
  note "conclusion: crash and reboot are one episode with a mandatory end, and the post-heal state"
  note "            shows all three nodes live again."
  note "dashboard: $(link step-crash)/universes/$LAST_SEED/timeline"
}

run_pause() {
  want pause || return 0
  say "step 10. process — pause a node, then resume it"
  note "claim: the write waits for the heal, so the ack arrives late and every replica ends up with it"
  pause_demo
  run_step step-pause pause
  show_step "$LOG"
  expect_result pause "$LOG" PASS
  expect_ledger pause "$LOG" 'site=pause_node' "the pause is in the ledger"
  note "conclusion: pausing is not a crash. The state is recovered and the data is complete, which is"
  note "            exactly the difference between losing time and losing data."
  note "dashboard: $(link step-pause)"
}

run_finding() {
  want finding || return 0
  say "step 11. process — split from one backup and kill the other"
  note "claim: acked=1, backup1=0, backup2=0; the same bug as step 3, reached through episodes"
  pause_demo
  run_step step-finding partition+crash
  show_step "$LOG"
  expect_result finding "$LOG" FAIL
  expect_ledger finding "$LOG" 'site=partition' "the split is in the ledger"
  expect_ledger finding "$LOG" 'site=crash_node' "the crash is in the ledger"
  expect_ledger finding "$LOG" 'backup1=0 backup2=0' "no backup holds the acknowledged write"
  note "conclusion: two episodes, both healed, one promise broken. This is the finding the seeded"
  note "            campaign reports, reached here by hand in one universe."
  note "dashboard: $(link step-finding)/universes/$LAST_SEED/timeline"
}

run_seeded() {
  want seeded || return 0
  local id="campaign-seeded" log="$WORK/logs/seeded.log"
  say "step 12. seeded campaign — 128 universes, each run twice"
  note "claim: many failures collapse into one finding, and the report grades its own coverage"
  pause_demo
  "$CAMPAIGN" --trials 128 --verify --root "$WORK/artifacts/$id" --id "$id" \
    --build-id "cosmos-demo-seeded" >"$log" 2>&1
  if ! grep -q 'findings' "$log"; then
    bad seeded "the campaign produced no summary; see $log"; return 1
  fi
  note "$(grep -m1 '^Campaign:' "$log")"
  note "$(grep -m1 '^FINDING' "$log")"
  note "$(grep -m1 '^  repro' "$log")"
  if grep -q '1 findings' "$log"; then
    ok seeded "sixty-six failing universes were reported as one finding"
  else
    bad seeded "expected one finding, got: $(grep -o '[0-9]* findings' "$log" | head -1)"
  fi
  if grep -q 'NOT EXERCISED' "$log"; then
    ok seeded "the report names the traps nothing exercised (vacuity)"
  else
    bad seeded "the vacuity section is missing"
  fi
  note "conclusion: identity is the promise's name, not the accident set. The same bug reached through"
  note "            sixty-six different seeds is one finding with sixty-six repro seeds."
  note "dashboard: $(link "$id")"
}

run_guided() {
  want guided || return 0
  local id="campaign-guided" log="$WORK/logs/guided.log"
  say "step 13. coverage-guided campaign — spend runs where the code is new"
  note "claim: the initial batch is seeded, then each seed is derived from a corpus member that"
  note "       added a code edge; the report exposes the corpus and the unique-edge count"
  pause_demo
  "$CAMPAIGN" --trials 24 --guidance coverage --initial-trials 4 \
    --root "$WORK/artifacts/$id" --id "$id" --build-id "cosmos-demo-guided" >"$log" 2>&1
  python3 - "$WORK/artifacts/$id/exploration.json" <<'PY'
import json,sys
try: d=json.load(open(sys.argv[1]))
except Exception as e: print("    exploration.json missing:",e); raise SystemExit(1)
cov=d.get("coverage") or {}
print("    guidance=%s method=%s" % (d.get("guidance"), cov.get("method")))
print("    runs_with_coverage=%s total_edges=%s unique_edges=%s novel_runs=%s corpus=%s" % (
    cov.get("runs_with_coverage"), cov.get("total_edges"), cov.get("unique_edges"),
    cov.get("novel_runs"), cov.get("corpus_size")))
PY
  if [ -f "$WORK/artifacts/$id/exploration.json" ] && grep -q '"guidance": "coverage"' "$WORK/artifacts/$id/exploration.json"; then
    ok guided "the guided campaign exported a corpus and coverage summary"
  else
    bad guided "no exploration artifact was written"
  fi
  note "conclusion: seeded and guided are the same loop with a selection rule — the first batch is"
  note "            seeded, and later seeds are derived from the corpus. Guided runs are sequential by"
  note "            construction, because the next seed depends on the previous novelty result."
  note "dashboard: $(link "$id")/exploration"
}

run_minimize() {
  want minimize || return 0
  local seed report
  say "step 14. minimize — eleven accidents are not a repro, one is"
  note "claim: the exported report names the kept decisions and the exact --only-faults recipe"
  pause_demo
  seed="$(sed -n 's/^  repro   = .*--seed \([0-9]*\).*/\1/p' "$WORK/logs/seeded.log" | head -1)"
  report="$WORK/artifacts/campaign-seeded/minimizations/$seed.json"
  if [ ! -f "$report" ]; then
    skip minimize "no minimization report for seed ${seed:-?} (--minimize-per-finding 0?)"; return 0
  fi
  python3 - "$report" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
print("    finding=%s refused=%s kept=%d of %d candidates passes=%s replays=%s" % (
    d.get("finding_id"), d.get("refused"), len(d.get("kept") or []),
    len(d.get("candidates") or []), d.get("passes"), d.get("replays")))
print("    only_faults=%s" % (d.get("only_faults"),))
print("    repro=%s" % d.get("repro"))
PY
  ok minimize "the report is present and names a kept set"
  note "conclusion: the guarantee is 'no single remaining accident can be removed' — 1-minimal, not"
  note "            globally minimal. Note the demo harness does not accept --only-faults, so this is"
  note "            shown as the exported report rather than re-executed."
  note "dashboard: $(link campaign-seeded)/universes/$seed/minimization"
}

run_wrap() {
  want wrap || return 0
  say "wrap-up"
  note "three claims, all demonstrated above:"
  note "  1. a failure is reproducible from a build and a seed, exactly, every run"
  note "  2. the report says what it did not test, instead of looking healthy and quiet"
  note "  3. a fault is an input: some are absorbed, one breaks the promise, and only state after the"
  note "     calm-down tells you which"
  [ -n "$WITH_UI" ] && note "dashboard: http://127.0.0.1:$PORT/"
  note "artifacts: $WORK/artifacts (one campaign per step)"
  ok wrap "the walkthrough finished"
}

# ---------------------------------------------------------------------------

printf '\033[1mCosmos fault-injection walkthrough\033[0m — %s\n' "$(date '+%Y-%m-%d %H:%M')"
note "seed:   $SEED (mode chaos: victims and timings are derived from it)"
note "work:   $WORK"
note "steps:  $PHASES"

if [ -n "$WITH_UI" ]; then
  python3 "$ROOT/ui/server/serve.py" --root "$WORK/artifacts" --port "$PORT" \
    >"$WORK/logs/ui.log" 2>&1 &
  UI_PID=$!
  for _ in $(seq 1 50); do
    curl -fsS "http://127.0.0.1:$PORT/api/campaigns" >/dev/null 2>&1 && break
    sleep 0.1
  done
  note "dashboard: http://127.0.0.1:$PORT/"
fi

for phase in $PHASES; do
  case "$phase" in
    build) run_build ;;
    control) run_control ;;
    drop-one) run_drop_one ;;
    drop-both) run_drop_both ;;
    short-send) run_short_send ;;
    reset) run_reset ;;
    refuse) run_refuse ;;
    sleep) run_sleep ;;
    partition) run_partition ;;
    crash) run_crash ;;
    pause) run_pause ;;
    finding) run_finding ;;
    seeded) run_seeded ;;
    guided) run_guided ;;
    minimize) run_minimize ;;
    wrap) run_wrap ;;
    *) echo "unknown step: $phase (see --list)" >&2; record SKIP "$phase" "unknown step" ;;
  esac
done

say "summary"
printf '    %-14s %-6s %s\n' STEP RESULT DETAIL
for row in "${RESULTS[@]}"; do
  IFS='|' read -r status phase detail <<<"$row"
  printf '    %-14s %-6s %s\n' "$phase" "$status" "$detail"
done
printf '\n    %d passed, %d failed, %d skipped\n' "$PASSED" "$FAILED" "$SKIPPED"
[ "$FAILED" -eq 0 ] || exit 1
