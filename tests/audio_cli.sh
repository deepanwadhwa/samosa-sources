#!/bin/sh
set -eu
binary=$1
fixture=$2
scratch=$(mktemp -d "${TMPDIR:-/tmp}/samosa-audio-cli-XXXXXX")
trap 'find "$scratch" -depth -delete' EXIT HUP INT TERM
mkdir "$scratch/root"
"$fixture" --fixture "$scratch/root/audio.wav"
registered=$(printf '%s' '{"contract_version":1,"op":"register_audio","path":"audio.wav"}' | "$binary" --root "$scratch/root" --state "$scratch/state" --owner alice)
id=$(printf '%s\n' "$registered" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
request=$(printf '{"contract_version":1,"op":"prepare_audio","source_id":"%s","version":"%s","start_ms":1000,"end_ms":1500,"operation_id":"audio-cli","parent_operation_id":"parent","timeout_ms":10000}' "$id" "$id")
reply=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice)
case "$reply" in *'"kind":"derived"'*'"artifact_type":"prepared_audio"'*'"encoding":"base64"'*'"start_ms":1000,"end_ms":1500'*'source_time_ms'*'"start_ms":0,"end_ms":500'*'media_preparation_only'*) ;; *) exit 1;; esac
case "$reply" in *evidence_id*|*'"text"'*) exit 1;; esac
rm "$scratch/root/audio.wav"
retry=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice)
test "$reply" = "$retry"
if foreign=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner bob); then exit 1; fi
case "$foreign" in *unknown_source*) ;; *) exit 1;; esac
changed=$(printf '{"contract_version":1,"op":"prepare_audio","source_id":"%s","version":"%s","start_ms":1001,"end_ms":1500,"operation_id":"audio-cli","parent_operation_id":"parent","timeout_ms":10000}' "$id" "$id")
if conflict=$(printf '%s' "$changed" | "$binary" --state "$scratch/state" --owner alice); then exit 1; fi
case "$conflict" in *idempotency_conflict*) ;; *) exit 1;; esac
printf 'PASS separate audio CLI: prepared media, absolute/local time, model-free preparation, immutable durable retry, foreign refusal and range conflict\n'
