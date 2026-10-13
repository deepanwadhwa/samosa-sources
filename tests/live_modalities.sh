#!/bin/sh
set -eu
driver=$1
native=$2
media=$3
pdfium=$(realpath "$4")
tesseract=$(realpath "$5")
leptonica=$(realpath "$6")
model=$(realpath "$7")
image=$8
docx_fixture=$9
shift 9
pdf_fixture=$1
scratch=$(mktemp -d "${TMPDIR:-/tmp}/samosa-live-corpus-XXXXXX")
trap 'find "$scratch" -depth -delete' EXIT HUP INT TERM
printf 'code 7319' > "$scratch/text"
printf '<html><body><p>code 7319</p></body></html>' > "$scratch/html"
"$docx_fixture" "$scratch/docx"
"$pdf_fixture" "$scratch/pdf"
ffmpeg -v error -nostdin -f lavfi -i 'sine=frequency=431:sample_rate=16000:duration=2' -c:a pcm_s16le "$scratch/wave.wav"
ffmpeg -v error -nostdin -f lavfi -i 'color=blue:size=64x64:rate=10:duration=2' -f lavfi -i 'sine=frequency=431:sample_rate=16000:duration=2' -c:v mpeg4 -c:a aac -shortest "$scratch/media.mp4"
run(){
    "$driver" "$1" "$2" "$3" "$4" "$native" "$media" "$pdfium" "$tesseract" "$leptonica" "$model"
}
run "$scratch/text" register_text read_text '"start_byte":0,"end_byte":9'
run "$scratch/html" register_html read_html '"start_byte":0,"end_byte":9'
run "$scratch/docx" register_docx read_docx '"start_byte":0,"end_byte":4'
run "$scratch/pdf" register_pdf read_pdf '"start_page":1,"end_page":1'
# Explicit OCR can render text-bearing PDFs too; image-only PDF qualification
# remains in the existing real-engine OCR corpus.
run "$scratch/pdf" register_pdf read_pdf_ocr '"start_page":1,"end_page":1'
run "$image" register_image read_image '"start_byte":0,"end_byte":10'
run "$scratch/wave.wav" register_audio prepare_audio '"start_ms":1000,"end_ms":1500'
run "$scratch/media.mp4" register_media prepare_audio '"start_ms":1000,"end_ms":1500'
run "$scratch/media.mp4" register_media prepare_visual '"start_ms":1000,"end_ms":1500'
