#!/bin/sh
set -eu
binary=$1
worker=$2
fixture=$3
mode=${4:-audio}
case "$mode" in
  audio) operation=prepare_audio; reader=audio-media-window-v1; derivation=native_decode_resample ;;
  visual) operation=prepare_visual; reader=visual-media-frame-v1; derivation=native_decode_scale_rgba ;;
  *) exit 2 ;;
esac
scratch=$(mktemp -d "${TMPDIR:-/tmp}/samosa-media-prepare-XXXXXX")
trap 'find "$scratch" -depth -delete' EXIT HUP INT TERM
mkdir "$scratch/root"
cp "$fixture" "$scratch/root/media.bin"
registered=$(printf '%s' '{"contract_version":1,"op":"register_media","path":"media.bin"}' | "$binary" --root "$scratch/root" --state "$scratch/state" --owner alice)
id=$(printf '%s\n' "$registered" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
request=$(printf '{"contract_version":1,"op":"%s","source_id":"%s","version":"%s","start_ms":1000,"end_ms":1500}' "$operation" "$id" "$id")
if unavailable=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice); then exit 1; fi
case "$unavailable" in *media_unavailable*) ;; *) exit 1;; esac
outside=$(printf '%s' "$request" | sed 's/"start_ms":1000/"start_ms":14400000/;s/"end_ms":1500/"end_ms":14400500/')
if invalid=$(printf '%s' "$outside" | "$binary" --state "$scratch/state" --owner alice); then exit 1; fi
case "$invalid" in *invalid_range*) ;; *) exit 1;; esac
reply=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice --media-helper "$worker")
case "$reply" in *'"kind":"derived"'*'"decoder":'*'source_time_ms'*"$reader"*"$derivation"*) ;; *) exit 1;; esac
rm "$scratch/root/media.bin"
hit=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice --media-helper "$worker")
test "$reply" = "$hit"
tracked=$(printf '%s,"operation_id":"media-cli","parent_operation_id":"media-parent","timeout_ms":10000}' "${request%?}")
saved=$(printf '%s' "$tracked" | "$binary" --state "$scratch/state" --owner alice --media-helper "$worker")
retry=$(printf '%s' "$tracked" | "$binary" --state "$scratch/state" --owner alice)
test "$saved" = "$retry"
changed=$(printf '%s' "$tracked" | sed 's/"start_ms":1000/"start_ms":1001/')
if conflict=$(printf '%s' "$changed" | "$binary" --state "$scratch/state" --owner alice); then exit 1; fi
case "$conflict" in *idempotency_conflict*) ;; *) exit 1;; esac
if foreign=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner bob --media-helper "$worker"); then exit 1; fi
case "$foreign" in *unknown_source*) ;; *) exit 1;; esac
printf 'ID3broken' > "$scratch/root/broken.mp3"
broken=$(printf '%s' '{"contract_version":1,"op":"register_media","path":"broken.mp3"}' | "$binary" --root "$scratch/root" --state "$scratch/state" --owner alice)
broken_id=$(printf '%s\n' "$broken" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#broken_id} -eq 64
broken_request=$(printf '{"contract_version":1,"op":"%s","source_id":"%s","version":"%s","start_ms":0,"end_ms":500,"operation_id":"broken-media"}' "$operation" "$broken_id" "$broken_id")
if failed=$(printf '%s' "$broken_request" | "$binary" --state "$scratch/state" --owner alice --media-helper "$worker"); then exit 1; fi
case "$failed" in *invalid_media*) ;; *) printf '%s\n' "$failed" >&2; exit 1;; esac
case "$failed" in *artifact_id*|*'"data"'*) exit 1;; esac
if failed_retry=$(printf '%s' "$broken_request" | "$binary" --state "$scratch/state" --owner alice); then exit 1; fi
test "$failed" = "$failed_retry"
survivor=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice --media-helper "$worker")
test "$reply" = "$survivor"
printf 'PASS malformed media: typed refusal, no artifact, exact offline failed retry and prior cache preservation\n'
printf 'PASS %s preparation: configured worker, immutable owner snapshot, source timeline, decoder provenance, exact cache reuse and offline durable retry/conflict\n' "$mode"
