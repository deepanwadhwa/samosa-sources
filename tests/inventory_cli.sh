#!/bin/sh
set -eu
sources=$1
fixture=$(mktemp -d "${TMPDIR:-/tmp}/samosa-native-inventory-cli-XXXXXX")
trap 'find "$fixture" -depth -delete' EXIT HUP INT TERM
mkdir "$fixture/root"
printf 'private source sentinel7319' > "$fixture/root/code.txt"
printf '%s' '{"contract_version":1,"op":"inventory","path":""}' | "$sources" --root "$fixture/root" --host-id fixture-cli > "$fixture/result.json"
grep -q '"total_entries":1' "$fixture/result.json"
grep -q '"content_coverage":"uninspected"' "$fixture/result.json"
grep -q '"locator":"code.txt"' "$fixture/result.json"
if grep -q '"text":' "$fixture/result.json"; then exit 1; fi
test ! -e "$fixture/root/sources.sqlite3"
printf 'PASS standalone native inventory CLI: no state/model/reading, qualified metadata\n'
