#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "visual_index.h"
#define QUEUE_VISUAL 1
#define QUEUE_PREFIX "visual"
#define QUEUE_COVERAGE "sampled_visual_frames"
#define QUEUE_PUBLICATION "chutni-visual-publication-v1"
#define QUEUE_OP "analyze_visual"
#define QUEUE_PLAN sources_visual_window_plan
#define QUEUE_VALIDATE sources_visual_capture_validate
#define QUEUE_CAPTURE sources_visual_window
#define QUEUE_PUBLISH source_visual_publish_capture
#define QUEUE_DISPATCH sources_visual_index
#define QUEUE_ADMIT sources_visual_index_admit
#define QUEUE_REPORT sources_visual_index_report
#define QUEUE_READ sources_visual_index_read_retained
#define QUEUE_SCAN sources_visual_index_scan_retained
#include "media_index_impl.h"
