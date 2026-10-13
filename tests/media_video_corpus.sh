#!/bin/sh
set -eu
binary=$1
worker=$2
output=$3
vp8_fixture=$4
mkdir -p "$output"
scratch=$(mktemp -d "$output/samosa-visual-corpus-XXXXXX")
printf 'Native visual corpus artifacts: %s\n' "$scratch"
for item in wide-mpeg4 wide-h264 portrait-mjpeg hevc vp8 vp9 h264-full hevc-10bit-bt2020 hevc-pq hevc-hlg hevc-pq-wrong-primaries delayed-video uncovered-video variable-rate; do
    matrix=bt601
    range=limited
    extension=mp4
    timestamp=1000000
    hdr_transfer=
    case "$item" in
        wide-mpeg4)
            width=512; height=288
            ffmpeg -hide_banner -loglevel error -f lavfi -i color=c=blue:s=960x540:r=25:d=2 -c:v mpeg4 -pix_fmt yuv420p -threads 1 "$scratch/$item.mp4"
            ;;
        wide-h264)
            width=512; height=288; matrix=bt709
            ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=s=768x432:r=25:d=2 -c:v libx264 -pix_fmt yuv420p -colorspace bt709 -color_range tv -threads 1 "$scratch/$item.mp4"
            ;;
        portrait-mjpeg)
            width=256; height=512; extension=mov; range=full
            ffmpeg -hide_banner -loglevel error -f lavfi -i color=c=red:s=300x600:r=25:d=2 -c:v mjpeg -pix_fmt yuvj420p -threads 1 "$scratch/$item.mov"
            ;;
        hevc)
            width=320; height=180
            ffmpeg -hide_banner -loglevel error -f lavfi -i color=c=green:s=320x180:r=25:d=2 -c:v libx265 -x265-params pools=none:frame-threads=1:log-level=error -pix_fmt yuv420p -threads 1 "$scratch/$item.mp4"
            ;;
        vp9)
            width=320; height=180
            ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=s=320x180:r=25:d=2 -c:v libvpx-vp9 -pix_fmt yuv420p -threads 1 "$scratch/$item.mp4"
            ;;
        vp8)
            width=320; height=180
            ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=s=320x180:r=25:d=2 -c:v libvpx -pix_fmt yuv420p -threads 1 "$scratch/$item.webm"
            "$vp8_fixture" "$scratch/$item.webm" "$scratch/$item.mp4"
            ffprobe -v error -select_streams v -show_entries packet=data_hash -show_data_hash sha256 -of csv=p=0 "$scratch/$item.webm" > "$scratch/$item-original.sha256"
            ffprobe -v error -select_streams v -show_entries packet=data_hash -show_data_hash sha256 -of csv=p=0 "$scratch/$item.mp4" > "$scratch/$item-wrapped.sha256"
            cmp "$scratch/$item-original.sha256" "$scratch/$item-wrapped.sha256"
            ;;
        h264-full)
            width=320; height=180; matrix=bt709; range=full
            ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=s=320x180:r=25:d=2 -c:v libx264 -pix_fmt yuvj420p -colorspace bt709 -color_range pc -threads 1 "$scratch/$item.mp4"
            ;;
        hevc-10bit-bt2020)
            width=320; height=180; matrix=bt2020
            ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=s=320x180:r=25:d=2 -c:v libx265 -x265-params pools=none:frame-threads=1:log-level=error -pix_fmt yuv420p10le -colorspace bt2020nc -color_primaries bt2020 -color_trc bt2020-10 -threads 1 "$scratch/$item.mp4"
            ;;
        hevc-pq|hevc-hlg|hevc-pq-wrong-primaries)
            width=512; height=288; hdr_transfer=smpte2084; primaries=bt2020; transfer_id=16; primaries_id=9
            if [ "$item" = hevc-hlg ]; then hdr_transfer=arib-std-b67; transfer_id=18; fi
            if [ "$item" = hevc-pq-wrong-primaries ]; then primaries=bt709; primaries_id=1; fi
            ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=s=768x432:r=25:d=2 -c:v libx265 -x265-params "pools=none:frame-threads=1:log-level=error:colorprim=$primaries_id:transfer=$transfer_id:colormatrix=9" -pix_fmt yuv420p10le -colorspace bt2020nc -color_primaries "$primaries" -color_trc "$hdr_transfer" -threads 1 "$scratch/$item.mp4"
            if [ "$item" = hevc-pq-wrong-primaries ]; then
                "$binary" "$scratch/$item.mp4" --refuse media_visual_format_unsupported "$worker"
                continue
            fi
            ;;
        delayed-video|uncovered-video)
            width=320; height=180; offset=1.2; timestamp=1200000
            if [ "$item" = uncovered-video ]; then offset=2; fi
            ffmpeg -hide_banner -loglevel error -itsoffset "$offset" -f lavfi -i testsrc2=s=320x180:r=25:d=2 -f lavfi -i sine=frequency=400:sample_rate=16000:duration=4 -map 0:v -map 1:a -fps_mode passthrough -c:v libx264 -pix_fmt yuv420p -c:a aac -threads 1 "$scratch/$item.mp4"
            if [ "$item" = uncovered-video ]; then
                "$binary" "$scratch/$item.mp4" --refuse invalid_range "$worker"
                continue
            fi
            ;;
        variable-rate)
            width=320; height=180; timestamp=1120000
            ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=s=320x180:r=25:d=2 -vf 'select=not(mod(n\,7))' -fps_mode vfr -c:v libx264 -pix_fmt yuv420p -threads 1 "$scratch/$item.mp4"
            ;;
    esac
    filters="scale=$width:$height:flags=bilinear:in_color_matrix=$matrix:in_range=$range:out_range=full,format=rgba"
    if [ -n "$hdr_transfer" ]; then
        if ffmpeg -hide_banner -filters 2>/dev/null | awk '$2 == "zscale" { found=1 } END { exit !found }'; then
            filters="zscale=w=$width:h=$height:filter=bilinear:tin=$hdr_transfer:pin=bt2020:min=bt2020nc:t=linear:p=bt709:r=full:npl=100,format=gbrpf32le,tonemap=tonemap=hable:desat=0:peak=10,zscale=t=iec61966-2-1:p=bt709:m=bt709:r=full,format=rgba"
        else
            filters="scale=w=$width:h=$height:flags=bilinear:in_transfer=$hdr_transfer:out_transfer=linear:in_primaries=bt2020:out_primaries=bt709:in_color_matrix=bt2020:out_range=full,format=gbrpf32le,tonemap=tonemap=hable:desat=0:peak=10,scale=in_transfer=linear:out_transfer=iec61966-2-1:in_primaries=bt709:out_primaries=bt709:out_range=full,format=rgba"
        fi
    fi
    reference_input="$scratch/$item.$extension"
    if [ "$item" = vp8 ]; then reference_input="$scratch/$item.webm"; fi
    ffmpeg -hide_banner -loglevel error -threads 1 -filter_threads 1 -i "$reference_input" -vf "select=gte(t\,1),$filters" -frames:v 1 -f rawvideo "$scratch/$item.rgba"
    printf 'Checking %s\n' "$item"
    if [ -n "$hdr_transfer" ]; then
        "$binary" "$scratch/$item.$extension" "$scratch/$item.rgba" "$width" "$height" 1000 1500 "$worker" "$timestamp" "$transfer_id"
    else
        "$binary" "$scratch/$item.$extension" "$scratch/$item.rgba" "$width" "$height" 1000 1500 "$worker" "$timestamp"
    fi
done
