#!/bin/sh
set -eu
binary=$1
worker=$2
output=$3
mkdir -p "$output"
scratch=$(mktemp -d "$output/samosa-late-window-XXXXXX")
printf 'Late visual window artifacts: %s\n' "$scratch"
# Exercise a late selection, beyond the per-request30s window, without requiring
# full-duration model extraction (a separate Inference/indexing requirement).
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=s=640x360:r=2:d=90 -c:v mpeg4 -pix_fmt yuv420p -threads 1 "$scratch/late.mp4"
ffmpeg -hide_banner -loglevel error -filter_threads 1 -i "$scratch/late.mp4" -vf 'select=gte(t\,59),scale=512:288:flags=bilinear:in_color_matrix=bt601:in_range=limited:out_range=full,format=rgba' -frames:v 1 -f rawvideo "$scratch/late.rgba"
"$binary" "$scratch/late.mp4" "$scratch/late.rgba" 512 288 59000 59500 "$worker" 59000000
# More than the50000-packet work bound precedes this selected window. Seeking
# must preserve the source timestamp and make the final allowed second usable.
ffmpeg -hide_banner -loglevel error -f lavfi -i color=c=blue:s=16x16:r=25:d=14400 -c:v mpeg4 -pix_fmt yuv420p -threads 1 "$scratch/four-hour.mp4"
ffmpeg -hide_banner -loglevel error -filter_threads 1 -ss 14399 -i "$scratch/four-hour.mp4" -vf 'scale=16:16:flags=bilinear:in_color_matrix=bt601:in_range=limited:out_range=full,format=rgba' -frames:v 1 -f rawvideo "$scratch/four-hour.rgba"
"$binary" "$scratch/four-hour.mp4" "$scratch/four-hour.rgba" 16 16 14399000 14400000 "$worker" 14399000000
