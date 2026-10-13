#!/bin/sh
set -eu
binary=$1
fixture=$2
scratch=$(mktemp -d "${TMPDIR:-/tmp}/samosa-media-registration-XXXXXX")
trap 'find "$scratch" -depth -delete' EXIT HUP INT TERM
mkdir "$scratch/root"
cp "$fixture" "$scratch/root/media.bin"
request='{"contract_version":1,"op":"register_media","path":"media.bin"}'
reply=$(printf '%s' "$request" | "$binary" --root "$scratch/root" --state "$scratch/state" --owner alice)
id=$(printf '%s\n' "$reply" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
again=$(printf '%s' "$request" | "$binary" --root "$scratch/root" --state "$scratch/state" --owner alice)
test "$reply" = "$again"
printf 'ordinary text' > "$scratch/root/media.bin"
if rejected=$(printf '%s' "$request" | "$binary" --root "$scratch/root" --state "$scratch/state" --owner alice); then exit 1; fi
case "$rejected" in *unsupported_source*) ;; *) exit 1;; esac
cp "$fixture" "$scratch/root/media.bin"
foreign=$(printf '%s' "$request" | "$binary" --root "$scratch/root" --state "$scratch/state" --owner bob)
case "$foreign" in *"$id"*) ;; *) exit 1;; esac
printf 'PASS model/decoder-free immutable media registration, exact repeat, unsupported input and independent owner claim\n'
