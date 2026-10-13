#!/bin/sh
set -eu
binary=$1
worker=$2
output=$3
mkdir -p "$output"
scratch=$(mktemp -d "$output/samosa-index-audio-media-XXXXXX")
selection=${4:-'mp3-cbr mp3-vbr m4a aac flac ogg opus aiff wav'}
for format in $selection; do
    case "$format" in
        mp3-cbr) extension=mp3; codec='libmp3lame'; options='-b:a 64k' ;;
        mp3-vbr) extension=mp3; codec='libmp3lame'; options='-q:a 4' ;;
        m4a|aac) extension=$format; codec=aac; options='-aac_pns 0 -b:a 48k' ;;
        flac) extension=flac; codec=flac; options='' ;;
        ogg) extension=ogg; codec=vorbis; options='-strict -2 -q:a 3' ;;
        opus) extension=opus; codec=opus; options='-strict -2 -b:a 48k' ;;
        aiff) extension=aiff; codec=pcm_s16be; options='' ;;
        wav) extension=wav; codec=pcm_s16le; options='' ;;
        *) printf 'Unknown audio corpus case: %s\n' "$format" >&2; exit 2 ;;
    esac
    for version in old new; do
        length=31.1
        frequency=431
        if [ "$version" = new ]; then length=32.2; frequency=719; fi
        # Codec options are fixed arguments declared above, never user text.
        ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=$frequency:sample_rate=48000:duration=$length" -ac 2 -c:a "$codec" $options -threads 1 "$scratch/$format-$version.$extension"
    done
    "$binary" "$worker" "$scratch/$format-old.$extension" "$scratch/$format-new.$extension" "$extension"
done
