#!/bin/sh
set -eu
binary=$1
fixture=$(mktemp -d "${TMPDIR:-/tmp}/samosa-html-cli-XXXXXX")
trap 'find "$fixture" -depth -delete' EXIT HUP INT TERM
mkdir "$fixture/root"
printf '%s' '<html><body><p>Code &amp; café 7319</p><script>POISON</script></body></html>' > "$fixture/root/page.html"
registered=$(printf '%s' '{"contract_version":1,"op":"register_html","path":"page.html"}' | "$binary" --root "$fixture/root" --state "$fixture/state" --owner alice)
id=$(printf '%s\n' "$registered" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
request=$(printf '{"contract_version":1,"op":"read_html","source_id":"%s","version":"%s","start_byte":0,"end_byte":4,"operation_id":"html-cli","parent_operation_id":"parent","timeout_ms":1000}' "$id" "$id")
reply=$(printf '%s' "$request" | "$binary" --state "$fixture/state" --owner alice)
case "$reply" in *'"text":"Code"'*'"coordinate_space":"extracted_utf8"'*'static_readable_text_only'*) ;; *) exit 1;; esac
case "$reply" in *POISON*) exit 1;; esac
printf '%s' changed > "$fixture/root/page.html"
retry=$(printf '%s' "$request" | "$binary" --state "$fixture/state" --owner alice)
test "$reply" = "$retry"
status=$(printf '%s' '{"contract_version":1,"op":"operation_get","operation_id":"html-cli"}' | "$binary" --state "$fixture/state" --owner alice)
case "$status" in *'"status":"completed"'*) ;; *) exit 1;; esac
if foreign=$(printf '%s' "$request" | "$binary" --state "$fixture/state" --owner other); then exit 1; fi
case "$foreign" in *unknown_source*) ;; *) exit 1;; esac
printf 'PASS HTML separate CLI processes: immutable evidence, durable exact retry and owner refusal\n'
