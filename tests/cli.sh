#!/bin/sh
set -eu
binary=$1
fixture=$(mktemp -d "${TMPDIR:-/tmp}/samosa-native-cli-XXXXXX")
trap 'find "$fixture" -depth -delete' EXIT HUP INT TERM
mkdir "$fixture/root"
printf 'Access code is 7319.' > "$fixture/root/code.txt"
registration=$(printf '%s' '{"contract_version":1,"op":"register_text","path":"code.txt"}' | "$binary" --root "$fixture/root" --state "$fixture/state")
id=$(printf '%s\n' "$registration" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
printf 'changed' > "$fixture/root/code.txt"
response=$(printf '{"contract_version":1,"op":"read_text","source_id":"%s","version":"%s","start_byte":0,"end_byte":20}' "$id" "$id" | "$binary" --state "$fixture/state")
printf '%s\n' "$response" | grep -q 'Access code is 7319.'
printf '%s\n' "$response" | grep -q '"status":"complete"'
printf '%s' '{"contract_version":1,"op":"describe"}' | "$binary" | grep -q 'samosa-sources'
printf 'PASS separate native CLI processes: retained immutable bytes\n'
