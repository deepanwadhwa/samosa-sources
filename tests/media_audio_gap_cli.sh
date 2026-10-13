#!/bin/sh
set -eu
binary=$1
worker=$2
output=$3
mkdir -p "$output"
scratch=$(mktemp -d "$output/samosa-audio-gap-XXXXXX")
printf 'Late audio gap artifacts: %s\n' "$scratch"
ffmpeg -hide_banner -loglevel error -f lavfi -i color=c=blue:s=16x16:r=1:d=61 -itsoffset 59.005 -f lavfi -i sine=frequency=431:sample_rate=44100:duration=1 -map 0:v -map 1:a -c:v mpeg4 -c:a pcm_s16le -threads 1 "$scratch/gap.mov"
registered=$(printf '%s' '{"contract_version":1,"op":"register_media","path":"gap.mov"}' | "$binary" --root "$scratch" --state "$scratch/state" --owner gap)
id=$(printf '%s\n' "$registered" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
request=$(printf '{"contract_version":1,"op":"prepare_audio","source_id":"%s","version":"%s","start_ms":59000,"end_ms":59500,"operation_id":"gap"}' "$id" "$id")
if reply=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner gap --media-helper "$worker"); then exit 1; fi
case "$reply" in *'"code":"invalid_range"'*) ;; *) printf '%s\n' "$reply"; exit 1;; esac
case "$reply" in *'"data"'*|*artifact_id*) exit 1;; esac
rm "$scratch/gap.mov"
if retry=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner gap); then exit 1; fi
test "$retry" = "$reply"
printf 'PASS late audio gap: no manufactured samples/artifact and exact durable retry without original/helper\n'
