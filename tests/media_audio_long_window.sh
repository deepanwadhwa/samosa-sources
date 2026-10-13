#!/bin/sh
set -eu
binary=$1
worker=$2
output=$3
mkdir -p "$output"
scratch=$(mktemp -d "$output/samosa-long-audio-XXXXXX")
printf 'Native four-hour audio artifacts: %s\n' "$scratch"
# Native packet-copy looping avoids spending a qualification run encoding four
# hours. ADTS has no edit-list trimming, so its complete packets remux into one
# continuous timeline. No decoder or synthetic container implementation is used.
ffmpeg -hide_banner -loglevel error -f lavfi -i sine=frequency=431:sample_rate=8000:duration=8.192 -c:a aac -aac_pns 0 -b:a 8k -threads 1 -f adts "$scratch/loop.aac"
packets=$(ffprobe -v error -count_packets -select_streams a -show_entries stream=nb_read_packets -of csv=p=0 "$scratch/loop.aac")
duration=$(awk -v count="$packets" 'BEGIN {printf "%.6f", count*1024/8000}')
i=0
while [ "$i" -lt 1800 ]; do
    printf "file 'loop.aac'\nduration %s\n" "$duration" >> "$scratch/continuous.ffconcat"
    i=$((i + 1))
done
ffmpeg -hide_banner -loglevel error -f concat -safe 1 -i "$scratch/continuous.ffconcat" -t 14399 -c:a copy -movflags +faststart "$scratch/four-hour.m4a"
ffmpeg -hide_banner -loglevel error -i "$scratch/four-hour.m4a" -af 'aresample=16000,aformat=sample_fmts=s16:channel_layouts=mono,atrim=start_sample=230368000:end_sample=230376000' -f s16le "$scratch/four-hour.pcm"
ffmpeg -hide_banner -loglevel error -i "$scratch/four-hour.m4a" -ac 1 -ar 16000 -f s16le - | wc -c > "$scratch/full-bytes.txt"
count=$(cat "$scratch/full-bytes.txt")
test "$count" -gt 0
test $((count % 2)) -eq 0
"$binary" "$scratch/four-hour.m4a" "$scratch/four-hour.pcm" "$worker" 14398000 14398500 "$((count / 2))"
