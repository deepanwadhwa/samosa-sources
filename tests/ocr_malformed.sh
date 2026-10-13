#!/bin/sh
set -eu
binary=$1
generator=$2
tesseract=$(realpath "$3")
leptonica=$(realpath "$4")
model=$(realpath "$5")
scratch=$(mktemp -d "${TMPDIR:-/tmp}/samosa-ocr-malformed-XXXXXX")
trap 'find "$scratch" -depth -delete' EXIT HUP INT TERM
mkdir "$scratch/root"
"$generator" "$6" "$scratch/root"
for format in bmp jpg png tif gif webp; do
    dd if="$scratch/root/sentinel.$format" of="$scratch/root/truncated.$format" bs=16 count=1 2>/dev/null
    request=$(printf '{"contract_version":1,"op":"register_image","path":"truncated.%s"}' "$format")
    registered=$(printf '%s' "$request" | "$binary" --root "$scratch/root" --state "$scratch/state" --owner alice)
    id=$(printf '%s\n' "$registered" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
    test ${#id} -eq 64
    request=$(printf '{"contract_version":1,"op":"read_image","source_id":"%s","version":"%s","start_byte":0,"end_byte":10}' "$id" "$id")
    if reply=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice --tesseract-library "$tesseract" --leptonica-library "$leptonica" --ocr-traineddata "$model"); then
        printf 'Unexpected truncated %s success: %s\n' "$format" "$reply" >&2
        exit 1
    fi
    case "$reply" in *'"code":"invalid_image"'*|*'"code":"image_decode_failed"'*) ;; *) printf 'Unexpected truncated %s failure: %s\n' "$format" "$reply" >&2; exit 1;; esac
    case "$reply" in *evidence_id*) exit 1;; esac
done
# A valid decode/OCR must survive all six malformed inputs in the same state.
registered=$(printf '%s' '{"contract_version":1,"op":"register_image","path":"sentinel.png"}' | "$binary" --root "$scratch/root" --state "$scratch/state" --owner alice)
id=$(printf '%s\n' "$registered" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
request=$(printf '{"contract_version":1,"op":"read_image","source_id":"%s","version":"%s","start_byte":0,"end_byte":10}' "$id" "$id")
reply=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice --tesseract-library "$tesseract" --leptonica-library "$leptonica" --ocr-traineddata "$model")
case "$reply" in *'"text":"TOKEN 7319"'*) ;; *) exit 1;; esac
printf 'PASS malformed raster corpus: six truncated formats return decoder errors without evidence; actual PNG OCR survives\n'
