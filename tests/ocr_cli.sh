#!/bin/sh
set -eu
binary=$1
tesseract=$(realpath "$2")
leptonica=$(realpath "$3")
model=$(realpath "$4")
fixture=$5
scratch=$(mktemp -d "${TMPDIR:-/tmp}/samosa-ocr-cli-XXXXXX")
trap 'find "$scratch" -depth -delete' EXIT HUP INT TERM
mkdir "$scratch/root"
cp "$fixture" "$scratch/root/image.ppm"
registered=$(printf '%s' '{"contract_version":1,"op":"register_image","path":"image.ppm"}' | "$binary" --root "$scratch/root" --state "$scratch/state" --owner alice)
id=$(printf '%s\n' "$registered" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
request=$(printf '{"contract_version":1,"op":"read_image","source_id":"%s","version":"%s","start_byte":0,"end_byte":10,"operation_id":"ocr-cli","parent_operation_id":"parent","timeout_ms":10000}' "$id" "$id")
reply=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice --tesseract-library "$tesseract" --leptonica-library "$leptonica" --ocr-traineddata "$model")
case "$reply" in *'"text":"TOKEN 7319"'*'extracted_utf8'*'ocr_recognition_uncertain'*'original_pixels'*) ;; *) exit 1;; esac
rm "$scratch/root/image.ppm"
retry=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner alice)
test "$reply" = "$retry"
status=$(printf '%s' '{"contract_version":1,"op":"operation_get","operation_id":"ocr-cli"}' | "$binary" --state "$scratch/state" --owner alice)
case "$status" in *'"status":"completed"'*) ;; *) exit 1;; esac
if foreign=$(printf '%s' "$request" | "$binary" --state "$scratch/state" --owner other); then exit 1; fi
case "$foreign" in *unknown_source*) ;; *) exit 1;; esac
printf 'PASS separate native OCR CLI: actual numeric sentinel, provenance, immutable evidence, durable offline retry and owner refusal\n'
