#!/bin/sh
set -eu
binary=$1
worker=$2
output=$3
mkdir -p "$output"
scratch=$(mktemp -d "$output/samosa-audio-window-XXXXXX")
printf 'Native audio window artifacts: %s\n' "$scratch"
for item in aac-no-pns mp3-cbr mp3-vbr pcm-s16le pcm-s16be pcm-s24le pcm-s32le pcm-f32le; do
    extension=mov
    case "$item" in
        aac-no-pns)
            extension=m4a
            ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=0.3*sin(2*PI*431*t)|0.2*sin(2*PI*719*t):s=48000:d=61' -c:a aac -aac_pns 0 -b:a 48k -threads 1 "$scratch/$item.$extension"
            ;;
        mp3-cbr|mp3-vbr)
            extension=mp3
            if [ "$item" = mp3-cbr ]; then
                ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=0.3*sin(2*PI*(431*t+0.5*t*t)):s=44100:d=61' -c:a libmp3lame -b:a 64k -threads 1 "$scratch/$item.$extension"
            else
                ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=(0.2+0.15*sin(t))*sin(2*PI*(431*t+0.5*t*t)):s=44100:d=61' -c:a libmp3lame -q:a 4 -threads 1 "$scratch/$item.$extension"
            fi
            ;;
        pcm-*)
            codec=$(printf '%s' "$item" | sed 's/pcm-/pcm_/')
            ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=0.3*sin(2*PI*431*t)|0.2*sin(2*PI*719*t):s=24000:d=61' -c:a "$codec" -threads 1 "$scratch/$item.$extension"
            ;;
    esac
    ffmpeg -hide_banner -loglevel error -i "$scratch/$item.$extension" -af 'aresample=16000,aformat=sample_fmts=s16:channel_layouts=mono,atrim=start_sample=944208:end_sample=952208' -f s16le "$scratch/$item.pcm"
    printf 'Checking %s at59013ms against sequential reference\n' "$item"
    "$binary" "$scratch/$item.$extension" "$scratch/$item.pcm" "$worker" 59013 59513
done
# PNS advances native decoder state. Its seek reference must start at the same
# native packet as the worker. Native first_pts aligns the source-wide output
# sample grid independently; no tolerance replaces the exact byte comparison.
ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=0.3*sin(2*PI*431*t)|0.2*sin(2*PI*719*t):s=48000:d=61' -c:a aac -b:a 48k -threads 1 "$scratch/aac-pns.m4a"
first=$(ffprobe -v error -select_streams a:0 -read_intervals '57%+0.001' -show_entries packet=pts_time -of csv=p=0 "$scratch/aac-pns.m4a" | head -1)
ffmpeg -hide_banner -loglevel error -noaccurate_seek -ss "$first" -copyts -i "$scratch/aac-pns.m4a" -af 'aresample=16000:async=1:first_pts=0,aformat=sample_fmts=s16:channel_layouts=mono,atrim=start_sample=944000:end_sample=952000' -f s16le "$scratch/aac-pns.pcm"
printf 'Checking AAC PNS against native seek reference\n'
"$binary" "$scratch/aac-pns.m4a" "$scratch/aac-pns.pcm" "$worker" 59000 59500
