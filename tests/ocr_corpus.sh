#!/bin/sh
set -eu
binary=$1
generator=$2
scratch=$(mktemp -d "${TMPDIR:-/tmp}/samosa-ocr-corpus-XXXXXX")
trap 'find "$scratch" -depth -delete' EXIT HUP INT TERM
"$generator" "$6" "$scratch"
for format in bmp jpg png tif gif webp; do
    sh tests/ocr_cli.sh "$binary" "$3" "$4" "$5" "$scratch/sentinel.$format"
done
printf 'PASS native raster corpus: BMP JPEG PNG TIFF GIF WebP through public registration, actual OCR, provenance, durable retry and owner refusal\n'
