#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "audio_index.h"
#define QUEUE_VISUAL 0
#define QUEUE_PREFIX "audio"
#define QUEUE_COVERAGE "selected_audio_windows"
#define QUEUE_PUBLICATION "chutni-transcript-publication-v1"
#define QUEUE_OP "transcribe_audio"
#define QUEUE_PLAN sources_audio_window_plan
#define QUEUE_VALIDATE sources_transcript_capture_validate
#define QUEUE_CAPTURE sources_transcript_window
#define QUEUE_PUBLISH source_transcript_publish_capture
#define QUEUE_DISPATCH sources_audio_index
#define QUEUE_ADMIT sources_audio_index_admit
#define QUEUE_REPORT sources_audio_index_report
#define QUEUE_READ sources_audio_index_read_retained
#define QUEUE_SCAN sources_audio_index_scan_retained
#include "media_index_impl.h"
