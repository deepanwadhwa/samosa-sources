#!/bin/sh
set -eu
binary=$1
worker=$2
fixture=$3
format=$4
expected_ms=$5
case "$format" in audio) registration=register_audio; policy=pcm_samples_floor_ms;; media) registration=register_media; policy=decoded_samples_floor_ms;; *) exit 2;; esac
scratch=$(mktemp -d "${TMPDIR:-/tmp}/samosa-audio-inspect-XXXXXX")
trap 'find "$scratch" -depth -delete' EXIT HUP INT TERM
mkdir "$scratch/root"
cp "$fixture" "$scratch/root/audio.bin"
registered=$(printf '{"contract_version":1,"op":"%s","path":"audio.bin"}' "$registration" | "$binary" --root "$scratch/root" --state "$scratch/state" --owner alice)
id=$(printf '%s\n' "$registered" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
request=$(printf '{"contract_version":1,"op":"inspect_audio","source_id":"%s","version":"%s"}' "$id" "$id")
reply=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice --media-helper "$worker")
case "$reply" in *'"kind":"derived"'*'"artifact_type":"audio_timeline"'*"\"duration_ms\":$expected_ms,"*"$policy"*) ;; *) printf '%s\n' "$reply" >&2; exit 1;; esac
case "$reply" in *evidence_id*|*'"data"'*) exit 1;; esac
last=$(printf '{"contract_version":1,"op":"prepare_audio","source_id":"%s","version":"%s","start_ms":%s,"end_ms":%s}' "$id" "$id" "$((expected_ms-1))" "$expected_ms")
printf '%s' "$last" | "$binary" --state "$scratch/state" --owner alice --media-helper "$worker" > "$scratch/prepared.json"
outside=$(printf '{"contract_version":1,"op":"prepare_audio","source_id":"%s","version":"%s","start_ms":%s,"end_ms":%s}' "$id" "$id" "$expected_ms" "$((expected_ms+1))")
if invalid=$(printf '%s' "$outside" | "$binary" --state "$scratch/state" --owner alice --media-helper "$worker"); then exit 1; fi
case "$invalid" in *invalid_range*) ;; *) exit 1;; esac
tracked=$(printf '%s,"operation_id":"audio-cli-inspection","parent_operation_id":"audio-parent","timeout_ms":20000}' "${request%?}")
saved=$(printf '%s' "$tracked" | "$binary" --state "$scratch/state" --owner alice --media-helper "$worker")
rm "$scratch/root/audio.bin"
retry=$(printf '%s' "$tracked" | "$binary" --state "$scratch/state" --owner alice)
test "$saved" = "$retry"
again=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice --media-helper "$worker")
test "$reply" = "$again"
if foreign=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner bob --media-helper "$worker"); then exit 1; fi
case "$foreign" in *unknown_source*) ;; *) exit 1;; esac
extra=$(printf '%s,"path":"audio.bin"}' "${request%?}")
if invalid=$(printf '%s' "$extra" | "$binary" --state "$scratch/state" --owner alice --media-helper "$worker"); then exit 1; fi
case "$invalid" in *unknown_field*) ;; *) exit 1;; esac
printf 'PASS public %s audio inspection: exact %sms endpoint, uncovered tail refusal, immutable replay, owner and field controls\n' "$format" "$expected_ms"
