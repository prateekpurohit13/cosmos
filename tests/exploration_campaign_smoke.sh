#!/usr/bin/env bash
# End-to-end smoke for the user-facing exploration exporter. It catches Release-build assert
# removal, a missing snapshot, and a forced-choice regression without duplicating the tool in C++.
set -euo pipefail
tool="$1"
root="$2"
rm -rf "$root"
"$tool" "$root" 6 >/dev/null
test -s "$root/exploration.json"
test -s "$root/manifest.json"
grep -q '"campaign_id": "exploration"' "$root/exploration.json"
grep -q '"forced_choice_applied": true' "$root/exploration.json"
grep -q '"status": "failed"' "$root/exploration.json"
grep -q '"parent_branch_index": 0' "$root/exploration.json"
grep -q '"status": "vacuous"' "$root/exploration.json"
grep -q '"exploration_file": "exploration.json"' "$root/manifest.json"
