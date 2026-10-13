#!/bin/sh
set -eu
binary=$1
generator=$2
library=$3
fixture=$(mktemp -d "${TMPDIR:-/tmp}/samosa-native-pdf-cli-XXXXXX")
trap 'find "$fixture" -depth -delete' EXIT HUP INT TERM
mkdir "$fixture/root"
"$generator" "$fixture/root/hundred.pdf"
registration=$(printf '%s' '{"contract_version":1,"op":"register_pdf","path":"hundred.pdf"}' | "$binary" --root "$fixture/root" --state "$fixture/state")
id=$(printf '%s\n' "$registration" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
response=$(printf '{"contract_version":1,"op":"read_pdf","source_id":"%s","version":"%s","start_page":97,"end_page":97}' "$id" "$id" | "$binary" --state "$fixture/state" --pdfium-library "$library")
printf '%s\n' "$response" | grep -q '7319'
printf '%s\n' "$response" | grep -q '"page":97'
printf '%s\n' "$response" | grep -q '"total_pages":100'
printf '%s\n' "$response" | grep -q 'text_layer_only'
printf 'PASS separate native PDF CLI: actual page97/100 citation and qualified coverage\n'
