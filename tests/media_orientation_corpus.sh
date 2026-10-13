#!/bin/sh
set -eu
binary=$1
worker=$2
fixture_writer=$3
output=$4
mkdir -p "$output"
scratch=$(mktemp -d "$output/samosa-orientation-corpus-XXXXXX")
printf 'Native orientation corpus artifacts: %s\n' "$scratch"
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=s=768x432:r=25:d=2 -c:v libx264 -pix_fmt yuv420p -colorspace bt709 -color_range tv -threads 1 "$scratch/base.mp4"
for code in 0 1 2 3 4 5 6 7 8; do
    input="$scratch/orientation-$code.mp4"
    "$fixture_writer" "$scratch/base.mp4" "$input" "$code"
    if [ "$code" = 8 ]; then
        "$binary" "$input" --refuse media_orientation_unsupported "$worker"
        continue
    fi
    width=512; height=288
    case "$code" in
        0) transform=null ;;
        1) transform=transpose=clock; width=288; height=512 ;;
        2) transform=hflip,vflip ;;
        3) transform=transpose=cclock; width=288; height=512 ;;
        4) transform=hflip ;;
        5) transform=vflip ;;
        6) transform=transpose=cclock_flip; width=288; height=512 ;;
        7) transform=transpose=clock_flip; width=288; height=512 ;;
    esac
    reference="$scratch/orientation-$code.rgba"
    ffmpeg -hide_banner -loglevel error -noautorotate -i "$input" -vf "select=gte(t\,1),scale=512:288:flags=bilinear:in_color_matrix=bt709:in_range=limited:out_range=full,format=rgba,$transform" -frames:v 1 -f rawvideo "$reference"
    printf 'Checking orientation %s\n' "$code"
    "$binary" "$input" "$reference" "$width" "$height" 1000 1500 "$worker"
done
