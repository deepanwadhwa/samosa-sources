CC ?= cc
.DEFAULT_GOAL := all
CPPFLAGS ?=
CPPFLAGS += -isystem vendor/pdfium-headers -isystem vendor/miniz
CFLAGS ?= -O2 -g
WARN = -std=c11 -Wall -Wextra -Werror -Wpedantic
SQLITE_MODE ?= bundled
SQLITE_LIBS ?= -lsqlite3
ifeq ($(SQLITE_MODE),bundled)
CPPFLAGS += -isystem vendor/sqlite
SQLITE_LIBS = build/libsqlite3.a
SQLITE_BUILD_DEP = build/libsqlite3.a
endif
LIBS = $(SQLITE_LIBS) -pthread
ifeq ($(shell uname -s),Linux)
LIBS += -ldl
SHARED = -shared -fPIC
else
SHARED = -dynamiclib
endif
HEADERS = src/index_embedding.h src/embedding.h src/enrichment.h src/summary.h src/semantic.h src/index_media_impl.h src/media_index_impl.h src/audio_index.h src/visual_index.h src/chutni.h src/search.h src/fetch.h src/uploads.h src/bound_db.h src/watch.h src/cache.h src/audio.h src/native.h src/ocr.h $(DOCX_HEADERS) src/samosa_html.h src/operations.h src/sources.h src/control.h src/json.h src/sha256.h src/pdf.h src/inventory.h src/index.h src/locator.h vendor/pdfium-headers/fpdfview.h vendor/pdfium-headers/fpdf_text.h
SOURCES = src/index_embedding.c src/embedding.c src/enrichment.c src/summary.c src/semantic.c src/audio_index.c src/visual_index.c src/transcript.c src/publication.c src/chutni.c src/search.c src/fetch_store.c src/fetch.c src/uploads.c src/bound_db.c src/watch.c src/media_validate.c src/audio.c src/native.c src/ocr.c src/samosa_docx.c vendor/miniz/miniz.c vendor/miniz/miniz_tinfl.c vendor/miniz/miniz_zip.c src/samosa_html.c src/operations.c src/sources.c src/control.c src/pdf.c src/inventory.c src/index.c src/locator.c
HEADERS += src/audio_index.h src/transcript.h src/visual.h
SOURCES += src/visual.c src/temporal.c src/temporal_capture.c
SOURCES += src/service.c src/service_http.c
SOURCES += src/service_upload.c
SOURCES += src/service_lease.c
HEADERS += src/service_lease.h
HEADERS += src/service.h src/service_http.h src/service_upload.h
HEADERS += src/temporal.h

.PHONY: test-service-http sanitize-service-http
.PHONY: test-service sanitize-service
.PHONY: test-service-lease sanitize-service-lease
test: test-service-lease test-service test-service-control test-service-formats
sanitize: sanitize-service-lease sanitize-service sanitize-service-control sanitize-service-formats
.PHONY: test-service-control sanitize-service-control
.PHONY: test-service-formats sanitize-service-formats
.PHONY: test-service-root sanitize-service-root
test: test-service-root
sanitize: sanitize-service-root
build/test-service-root: tests/test_service_root.c tests/test_service.c src/service_lease.c src/service_lease.h src/json.h src/sources.h | build
	$(CC) $(CFLAGS) $(WARN) -Isrc tests/test_service_root.c src/service_lease.c -o $@
test-service-root: all build/test-service-root
	./build/test-service-root '$(CURDIR)/build/samosa-sources'
sanitize-service-root: build/samosa-sources-inference-sanitize | build
	$(CC) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_service_root.c src/service_lease.c -o build/test-service-root-sanitize
	./build/test-service-root-sanitize '$(CURDIR)/build/samosa-sources-inference-sanitize'
build/test-service-formats: tests/test_service_formats.c tests/test_service.c tests/docx_fixture.h tests/pdf_fixture.h src/service_lease.c src/service_lease.h src/json.h src/sha256.h src/sources.h | build
	$(CC) $(CFLAGS) $(WARN) -Isrc tests/test_service_formats.c src/service_lease.c -o $@
test-service-formats: all build/test-service-formats
	./build/test-service-formats '$(CURDIR)/build/samosa-sources'
sanitize-service-formats: build/samosa-sources-inference-sanitize | build
	$(CC) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_service_formats.c src/service_lease.c -o build/test-service-formats-sanitize
	./build/test-service-formats-sanitize '$(CURDIR)/build/samosa-sources-inference-sanitize'
build/test-service-control: tests/test_service_control.c tests/test_service.c src/service_lease.c src/service_lease.h src/json.h src/sources.h | build
	$(CC) $(CFLAGS) $(WARN) -Isrc tests/test_service_control.c src/service_lease.c -o $@
test-service-control: all build/test-service-control
	./build/test-service-control '$(CURDIR)/build/samosa-sources'
sanitize-service-control: build/samosa-sources-inference-sanitize | build
	$(CC) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_service_control.c src/service_lease.c -o build/test-service-control-sanitize
	./build/test-service-control-sanitize '$(CURDIR)/build/samosa-sources-inference-sanitize'
build/test-service-lease: tests/test_service_lease.c src/service_lease.c src/service_lease.h src/sources.h | build
	$(CC) $(CFLAGS) $(WARN) -Isrc tests/test_service_lease.c src/service_lease.c -o $@
test-service-lease: build/test-service-lease
	./build/test-service-lease
sanitize-service-lease: | build
	$(CC) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_service_lease.c src/service_lease.c -o build/test-service-lease-sanitize
	./build/test-service-lease-sanitize
build/test-service: tests/test_service.c src/json.h src/sources.h src/service_lease.c src/service_lease.h | build
	$(CC) $(CFLAGS) $(WARN) -Isrc tests/test_service.c src/service_lease.c -o $@
test-service: all build/test-service
	./build/test-service '$(CURDIR)/build/samosa-sources'
sanitize-service: build/samosa-sources-inference-sanitize | build
	$(CC) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_service.c src/service_lease.c -o build/test-service-sanitize
	./build/test-service-sanitize '$(CURDIR)/build/samosa-sources-inference-sanitize'
build/test-service-http: tests/test_service_http.c src/service_http.c src/service_http.h src/json.h src/sources.h | build
	$(CC) $(CFLAGS) $(WARN) -Isrc tests/test_service_http.c src/service_http.c -o $@
test-service-http: build/test-service-http
	./build/test-service-http
sanitize-service-http: | build
	$(CC) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_service_http.c src/service_http.c -o build/test-service-http-sanitize
	./build/test-service-http-sanitize
.PHONY: test-visual sanitize-visual
build/test-visual: tests/test_visual.c tests/visual_fixture.h tests/temporal_fixture.h $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc $< $(SOURCES) $(LIBS) -o $@
build/test-visual-sanitize: tests/test_visual.c tests/visual_fixture.h tests/temporal_fixture.h $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc $< $(SOURCES) $(LIBS) -o $@
test-visual: build/test-visual
	$(abspath build/test-visual) '$(MEDIA_HELPER)' '$(MEDIA_FIXTURE)'
sanitize-visual: build/test-visual-sanitize
	$(abspath build/test-visual-sanitize) '$(MEDIA_HELPER)' '$(MEDIA_FIXTURE)'
build/test-visual-real: tests/test_visual_real.c src/json.h src/sha256.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc $< -o $@
build/test-visual-real-sanitize: tests/test_visual_real.c src/json.h src/sha256.h | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc $< -o $@
SOURCES += src/inference_process.c
HEADERS += src/fetch_public.h
SOURCES += src/fetch_public.c
.PHONY: test-inference-process sanitize-inference-process
.PHONY: test-audio-index-admit sanitize-audio-index-admit
.PHONY: test-index-audio-failures sanitize-index-audio-failures
.PHONY: test-index-audio-media sanitize-index-audio-media
build/test-index-audio-media: tests/test_index_audio_media.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc $< $(SOURCES) $(LIBS) -o $@
test-index-audio-media: media-worker build/test-index-audio-media
	sh tests/index_audio_media_corpus.sh '$(CURDIR)/build/test-index-audio-media' '$(CURDIR)/build/samosa-sources-media-worker' '$(CURDIR)/build/index-audio-media'
sanitize-index-audio-media: build/samosa-sources-media-worker-sanitize $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -Isrc tests/test_index_audio_media.c $(SOURCES) $(LIBS) -o build/test-index-audio-media-sanitize
	sh tests/index_audio_media_corpus.sh '$(CURDIR)/build/test-index-audio-media-sanitize' '$(CURDIR)/build/samosa-sources-media-worker-sanitize' '$(CURDIR)/build/index-audio-media-sanitize'
test: test-audio-index-admit test-index-audio-failures
sanitize: sanitize-audio-index-admit sanitize-index-audio-failures
build/test-index-audio-failures: tests/test_index_audio_failures.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc $< $(SOURCES) $(LIBS) -o $@
test-index-audio-failures: build/test-index-audio-failures
	./build/test-index-audio-failures
sanitize-index-audio-failures: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_index_audio_failures.c $(SOURCES) $(LIBS) -o build/test-index-audio-failures-sanitize
	./build/test-index-audio-failures-sanitize
build/test-audio-index-admit: tests/test_audio_index_admit.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_audio_index_admit.c $(SOURCES) $(LIBS) -o $@
test-audio-index-admit: build/test-audio-index-admit
	./build/test-audio-index-admit
sanitize-audio-index-admit: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -Isrc tests/test_audio_index_admit.c $(SOURCES) $(LIBS) -o build/test-audio-index-admit-sanitize
	./build/test-audio-index-admit-sanitize
.PHONY: test-fetch-public sanitize-fetch-public
.PHONY: test-search-presets sanitize-search-presets
build/test-search-presets: tests/test_search_presets.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_search_presets.c $(SOURCES) $(LIBS) -o $@
build/test-search-presets-sanitize: tests/test_search_presets.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -Isrc tests/test_search_presets.c $(SOURCES) $(LIBS) -o $@
test-search-presets: fetch-worker build/test-search-presets
	./build/test-search-presets '$(CURDIR)/build/samosa-sources-fetch-worker'
sanitize-search-presets: build/samosa-sources-fetch-worker-sanitize build/test-search-presets-sanitize
	./build/test-search-presets-sanitize '$(CURDIR)/build/samosa-sources-fetch-worker-sanitize'
build/test-fetch-public: tests/test_fetch_public.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_fetch_public.c $(SOURCES) $(LIBS) -o $@
build/test-fetch-public-sanitize: tests/test_fetch_public.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -Isrc tests/test_fetch_public.c $(SOURCES) $(LIBS) -o $@
test-fetch-public: fetch-worker build/test-fetch-public
	./build/test-fetch-public '$(CURDIR)/build/samosa-sources-fetch-worker'
sanitize-fetch-public: build/samosa-sources-fetch-worker-sanitize build/test-fetch-public-sanitize
	./build/test-fetch-public-sanitize '$(CURDIR)/build/samosa-sources-fetch-worker-sanitize'
build/test-inference-process: tests/test_inference_process.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_inference_process.c $(SOURCES) $(LIBS) -o $@
test-inference-process: all build/test-inference-process
	./build/test-inference-process "$$(pwd)/build/samosa-sources" "$$(pwd)/build/test-inference-process"
build/samosa-sources-inference-sanitize: src/main.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc src/main.c $(SOURCES) $(LIBS) -o $@
sanitize-inference-process: build/samosa-sources-inference-sanitize $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_inference_process.c $(SOURCES) $(LIBS) -o build/test-inference-process-sanitize
	./build/test-inference-process-sanitize "$$(pwd)/build/samosa-sources-inference-sanitize" "$$(pwd)/build/test-inference-process-sanitize"
build/test-transcript: tests/test_transcript.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_transcript.c $(SOURCES) $(LIBS) -o $@
.PHONY: test-transcript sanitize-transcript
test: test-transcript test-inference-process
sanitize: sanitize-transcript sanitize-inference-process
test-transcript: build/test-transcript
	./build/test-transcript
sanitize-transcript: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_transcript.c $(SOURCES) $(LIBS) -o build/test-transcript-sanitize
	./build/test-transcript-sanitize
define BUILD_CONFIGURATION
CC=$(CC)
CPPFLAGS=$(CPPFLAGS)
CFLAGS=$(CFLAGS)
LIBS=$(LIBS)
DESCRIPTOR_DB=$(DESCRIPTOR_DB)
endef
export BUILD_CONFIGURATION
.PHONY: FORCE
FORCE:
build/.configuration: FORCE | build
	@printf '%s\n' "$$BUILD_CONFIGURATION" > $@.pending
	@if cmp -s $@.pending $@; then rm -f $@.pending; else sleep 1; touch $@.pending; mv $@.pending $@; fi
HEADERS += build/.configuration
HEADERS += $(SQLITE_BUILD_DEP)
build/verify-sqlite: tests/verify_sqlite.c src/sha256.h build/.configuration | build
	$(CC) $(CFLAGS) $(WARN) -Isrc tests/verify_sqlite.c -o $@
.PHONY: verify-sqlite
verify-sqlite: build/verify-sqlite
	./build/verify-sqlite
build/sqlite3.o: vendor/sqlite/sqlite3.c vendor/sqlite/sqlite3.h build/.configuration | build verify-sqlite
	$(CC) -O2 -g -std=c11 -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -c vendor/sqlite/sqlite3.c -o $@
build/libsqlite3.a: build/sqlite3.o
	$(AR) rcs $@ $<
sanitize sanitize-binding-pins sanitize-operations sanitize-watch-runtime sanitize-queue-runtime sanitize-watch sanitize-reindex sanitize-index sanitize-index-freshness sanitize-live-read sanitize-locator sanitize-large-sources sanitize-media-video sanitize-media-cache sanitize-media-public sanitize-media-worker sanitize-audio sanitize-ocr-corpus sanitize-ocr-pdf sanitize-docx sanitize-html sanitize-control sanitize-pdf: $(SQLITE_BUILD_DEP)
.PHONY: all test test-pdf sanitize release clean
.PHONY: test-upload sanitize-upload
.PHONY: test-upload-sessions
.PHONY: test-upload-large sanitize-upload-large
build/test-upload-large: tests/test_upload_large.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_upload_large.c $(SOURCES) $(LIBS) -o $@
test-upload-large: build/test-upload-large
	./build/test-upload-large
sanitize-upload-large: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_upload_large.c $(SOURCES) $(LIBS) -o build/test-upload-large-sanitize
	./build/test-upload-large-sanitize
build/test-upload-sessions: tests/test_upload_sessions.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_upload_sessions.c $(SOURCES) $(LIBS) -o $@
test-upload-sessions: build/test-upload-sessions
	./build/test-upload-sessions
sanitize-upload-sessions: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_upload_sessions.c $(SOURCES) $(LIBS) -o build/test-upload-sessions-sanitize
	./build/test-upload-sessions-sanitize
.PHONY: sanitize-upload-sessions
test: test-upload-sessions test-upload-large
sanitize: sanitize-upload-sessions sanitize-upload-large
build/test-upload: tests/test_upload.c tests/docx_fixture.h $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_upload.c $(SOURCES) $(LIBS) -o $@
test-upload: build/test-upload
	./build/test-upload
sanitize-upload: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_upload.c $(SOURCES) $(LIBS) -o build/test-upload-sanitize
	./build/test-upload-sanitize
test: test-upload
sanitize: sanitize-upload
.PHONY: test-large-sources sanitize-large-sources
.PHONY: test-watch sanitize-watch
.PHONY: test-binding-pins sanitize-binding-pins
# Descriptor-bound store qualification gates.
.PHONY: test-bound-db sanitize-bound-db
build/test-bound-db: tests/test_bound_db.c src/bound_db.c src/bound_db.h build/.configuration $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_bound_db.c src/bound_db.c $(LIBS) -o $@
test-bound-db: build/test-bound-db
	./build/test-bound-db
sanitize-bound-db: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_bound_db.c src/bound_db.c $(LIBS) -o build/test-bound-db-sanitize
	./build/test-bound-db-sanitize
build/test-binding-pins: tests/test_binding_pins.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_binding_pins.c $(SOURCES) $(LIBS) -o $@
test-binding-pins: build/test-binding-pins
	./build/test-binding-pins
sanitize-binding-pins: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_binding_pins.c $(SOURCES) $(LIBS) -o build/test-binding-pins-sanitize
	./build/test-binding-pins-sanitize
test: test-bound-db test-binding-pins
sanitize: sanitize-bound-db sanitize-binding-pins
.PHONY: test-queue-runtime sanitize-queue-runtime
build/test-queue-runtime: tests/test_queue_runtime.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_queue_runtime.c $(SOURCES) $(LIBS) -o $@
test-queue-runtime: all build/test-queue-runtime
	./build/test-queue-runtime '$(CURDIR)/build/samosa-sources'
sanitize-queue-runtime: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc src/main.c $(SOURCES) $(LIBS) -o build/samosa-sources-queue-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_queue_runtime.c $(SOURCES) $(LIBS) -o build/test-queue-runtime-sanitize
	./build/test-queue-runtime-sanitize '$(CURDIR)/build/samosa-sources-queue-sanitize'
test: test-queue-runtime
sanitize: sanitize-queue-runtime
.PHONY: test-watch-runtime sanitize-watch-runtime
build/test-watch-runtime: tests/test_watch_runtime.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_watch_runtime.c $(SOURCES) $(LIBS) -o $@
test-watch-runtime: all build/test-watch-runtime
	./build/test-watch-runtime '$(CURDIR)/build/samosa-sources'
sanitize-watch-runtime: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc src/main.c $(SOURCES) $(LIBS) -o build/samosa-sources-watch-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_watch_runtime.c $(SOURCES) $(LIBS) -o build/test-watch-runtime-sanitize
	./build/test-watch-runtime-sanitize '$(CURDIR)/build/samosa-sources-watch-sanitize'
test: test-watch-runtime
sanitize: sanitize-watch-runtime
build/test-watch: tests/test_watch.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_watch.c $(SOURCES) $(LIBS) -o $@
test-watch: build/test-watch
	./build/test-watch
sanitize-watch: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_watch.c $(SOURCES) $(LIBS) -o build/test-watch-sanitize
	./build/test-watch-sanitize
test: test-watch
sanitize: sanitize-watch
.PHONY: test-live-read sanitize-live-read
.PHONY: test-reindex sanitize-reindex
build/test-reindex: tests/test_reindex.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_reindex.c $(SOURCES) $(LIBS) -o $@
test-reindex: build/test-reindex
	./build/test-reindex
sanitize-reindex: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_reindex.c $(SOURCES) $(LIBS) -o build/test-reindex-sanitize
	./build/test-reindex-sanitize
test: test-reindex
sanitize: sanitize-reindex
.PHONY: test-index-refresh sanitize-index-refresh
build/test-index-refresh: tests/test_index_refresh.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc $< $(SOURCES) $(LIBS) -o $@
test-index-refresh: all build/test-index-refresh
	./build/test-index-refresh '$(CURDIR)/build/samosa-sources'
sanitize-index-refresh: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc src/main.c $(SOURCES) $(LIBS) -o build/samosa-sources-refresh-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_index_refresh.c $(SOURCES) $(LIBS) -o build/test-index-refresh-sanitize
	./build/test-index-refresh-sanitize '$(CURDIR)/build/samosa-sources-refresh-sanitize'
test: test-index-refresh
sanitize: sanitize-index-refresh

.PHONY: test-index-observation sanitize-index-observation
build/test-index-observation: tests/test_index_observation.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc $< $(SOURCES) $(LIBS) -o $@
test-index-observation: $(SQLITE_BUILD_DEP) build/test-index-observation
	./build/test-index-observation
sanitize-index-observation: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_index_observation.c $(SOURCES) $(LIBS) -o build/test-index-observation-sanitize
	./build/test-index-observation-sanitize
test: test-index-observation
sanitize: sanitize-index-observation

.PHONY: test-index-recovery sanitize-index-recovery
build/test-index-recovery: tests/test_index_recovery.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc $< $(SOURCES) $(LIBS) -o $@
test-index-recovery: $(SQLITE_BUILD_DEP) build/test-index-recovery
	./build/test-index-recovery
sanitize-index-recovery: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_index_recovery.c $(SOURCES) $(LIBS) -o build/test-index-recovery-sanitize
	./build/test-index-recovery-sanitize
test: test-index-recovery
sanitize: sanitize-index-recovery

.PHONY: test-index-discovery sanitize-index-discovery
build/test-index-discovery: tests/test_index_discovery.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc $< $(SOURCES) $(LIBS) -o $@
test-index-discovery: $(SQLITE_BUILD_DEP) build/test-index-discovery
	./build/test-index-discovery
sanitize-index-discovery: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_index_discovery.c $(SOURCES) $(LIBS) -o build/test-index-discovery-sanitize
	./build/test-index-discovery-sanitize
test: test-index-discovery
sanitize: sanitize-index-discovery

.PHONY: test-index-freshness sanitize-index-freshness
.PHONY: test-index sanitize-index
test-index: build/test-index
	./build/test-index
sanitize-index: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_index.c $(SOURCES) $(LIBS) -o build/test-index-sanitize
	./build/test-index-sanitize
build/test-index-freshness: tests/test_index_freshness.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_index_freshness.c $(SOURCES) $(LIBS) -o $@
test-index-freshness: build/test-index-freshness
	./build/test-index-freshness
sanitize-index-freshness: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_index_freshness.c $(SOURCES) $(LIBS) -o build/test-index-freshness-sanitize
	./build/test-index-freshness-sanitize
test: test-index-freshness
sanitize: sanitize-index-freshness
build/test-live-read: tests/test_live_read.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_live_read.c $(SOURCES) $(LIBS) -o $@
test-live-read: build/test-live-read
	./build/test-live-read
sanitize-live-read: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_live_read.c $(SOURCES) $(LIBS) -o build/test-live-read-sanitize
	./build/test-live-read-sanitize
test: test-live-read
sanitize: sanitize-live-read
build/test-live-modalities: tests/test_live_modalities.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_live_modalities.c $(SOURCES) $(LIBS) -o $@
build/test-live-modalities-sanitize: tests/test_live_modalities.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_live_modalities.c $(SOURCES) $(LIBS) -o $@
.PHONY: test-live-modalities sanitize-live-modalities
test-live-modalities: all media-worker build/test-live-modalities build/docx-fixture build/pdf-fixture
	sh tests/live_modalities.sh '$(CURDIR)/build/test-live-modalities' '$(CURDIR)/build/samosa-sources' '$(CURDIR)/build/samosa-sources-media-worker' '$(PDFIUM_LIBRARY)' '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)' '$(CURDIR)/build/docx-fixture' '$(CURDIR)/build/pdf-fixture'
sanitize-live-modalities: build/samosa-sources-inference-sanitize build/samosa-sources-media-worker-sanitize build/test-live-modalities-sanitize build/docx-fixture build/pdf-fixture
	sh tests/live_modalities.sh '$(CURDIR)/build/test-live-modalities-sanitize' '$(CURDIR)/build/samosa-sources-inference-sanitize' '$(CURDIR)/build/samosa-sources-media-worker-sanitize' '$(PDFIUM_LIBRARY)' '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)' '$(CURDIR)/build/docx-fixture' '$(CURDIR)/build/pdf-fixture'
.PHONY: test-locator sanitize-locator
test-locator: build/test-locator
	./build/test-locator
sanitize-locator: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_locator.c $(SOURCES) $(LIBS) -o build/test-locator-sanitize
	./build/test-locator-sanitize
build/test-large-sources: tests/test_large_sources.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_large_sources.c $(SOURCES) $(LIBS) -o $@
test-large-sources: build/test-large-sources
	./build/test-large-sources
sanitize-large-sources: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_large_sources.c $(SOURCES) $(LIBS) -o build/test-large-sources-sanitize
	./build/test-large-sources-sanitize
.PHONY: test-audio-inspect test-media-primitive sanitize-media-primitive
test-audio-inspect: all media-worker
	@test -n '$(MEDIA_FIXTURE)' -a -n '$(MEDIA_EXPECTED_MS)' || { echo 'configure media fixture and expected whole milliseconds'; exit 2; }
	sh tests/audio_inspect_cli.sh '$(CURDIR)/build/samosa-sources' '$(CURDIR)/build/samosa-sources-media-worker' '$(MEDIA_FIXTURE)' '$(or $(MEDIA_FORMAT),media)' '$(MEDIA_EXPECTED_MS)'
.PHONY: media-worker
.PHONY: sanitize-media-video
sanitize-media-video: | build
	@test -n '$(MEDIA_FIXTURE)' || { echo 'configure explicit native video fixture'; exit 2; }
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) tests/test_media_video.c src/media_video.c src/media_input.c src/media_decode.c src/media_artifacts.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o build/test-media-video-sanitize
	./build/test-media-video-sanitize '$(MEDIA_FIXTURE)'
build/test-media-video: tests/test_media_video.c src/media_video.c src/media_video.h src/media_input.h src/media_input.c src/media_decode.c src/media_artifacts.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) tests/test_media_video.c src/media_video.c src/media_input.c src/media_decode.c src/media_artifacts.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o $@
.PHONY: test-media-video-corpus sanitize-media-video-corpus
build/test-media-vp8-fixture: tests/test_media_vp8_fixture.c | build
	$(CC) $(CFLAGS) $(WARN) $$(pkg-config --cflags libavformat libavcodec libavutil) $< $$(pkg-config --libs libavformat libavcodec libavutil) -o $@
.PHONY: test-media-late-window sanitize-media-late-window
.PHONY: test-media-audio-window-corpus sanitize-media-audio-window-corpus
.PHONY: test-media-audio-long-window sanitize-media-audio-long-window
.PHONY: test-media-audio-gap sanitize-media-audio-gap
test-media-audio-gap: all media-worker
	@test -n '$(MEDIA_CORPUS_DIR)' || { echo 'configure explicit native corpus output directory'; exit 2; }
	sh tests/media_audio_gap_cli.sh '$(CURDIR)/build/samosa-sources' '$(CURDIR)/build/samosa-sources-media-worker' '$(MEDIA_CORPUS_DIR)'
sanitize-media-audio-gap: build/samosa-sources-inference-sanitize
	@test -n '$(MEDIA_CORPUS_DIR)' -a -x '$(MEDIA_HELPER)' || { echo 'configure explicit native corpus output directory and instrumented helper'; exit 2; }
	sh tests/media_audio_gap_cli.sh '$(CURDIR)/build/samosa-sources-inference-sanitize' '$(MEDIA_HELPER)' '$(MEDIA_CORPUS_DIR)'
test-media-audio-long-window: build/test-media-audio-window media-worker
	@test -n '$(MEDIA_CORPUS_DIR)' || { echo 'configure explicit native corpus output directory'; exit 2; }
	sh tests/media_audio_long_window.sh ./build/test-media-audio-window '$(CURDIR)/build/samosa-sources-media-worker' '$(MEDIA_CORPUS_DIR)'
sanitize-media-audio-long-window: build/test-media-audio-window-sanitize
	@test -n '$(MEDIA_CORPUS_DIR)' -a -x '$(MEDIA_HELPER)' || { echo 'configure explicit native corpus output directory and instrumented helper'; exit 2; }
	sh tests/media_audio_long_window.sh ./build/test-media-audio-window-sanitize '$(MEDIA_HELPER)' '$(MEDIA_CORPUS_DIR)'
test-media-audio-window-corpus: build/test-media-audio-window media-worker
	@test -n '$(MEDIA_CORPUS_DIR)' || { echo 'configure explicit native corpus output directory'; exit 2; }
	sh tests/media_audio_window_corpus.sh ./build/test-media-audio-window '$(CURDIR)/build/samosa-sources-media-worker' '$(MEDIA_CORPUS_DIR)'
sanitize-media-audio-window-corpus: build/test-media-audio-window-sanitize
	@test -n '$(MEDIA_CORPUS_DIR)' -a -x '$(MEDIA_HELPER)' || { echo 'configure explicit native corpus output directory and instrumented helper'; exit 2; }
	sh tests/media_audio_window_corpus.sh ./build/test-media-audio-window-sanitize '$(MEDIA_HELPER)' '$(MEDIA_CORPUS_DIR)'
build/test-media-audio-window: tests/test_media_audio_window.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) tests/test_media_audio_window.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o $@
build/test-media-audio-window-sanitize: tests/test_media_audio_window.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) tests/test_media_audio_window.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o $@
test-media-late-window: build/test-media-video-corpus media-worker
	@test -n '$(MEDIA_CORPUS_DIR)' || { echo 'configure explicit native corpus output directory'; exit 2; }
	sh tests/media_late_window.sh ./build/test-media-video-corpus '$(CURDIR)/build/samosa-sources-media-worker' '$(MEDIA_CORPUS_DIR)'
sanitize-media-late-window: build/test-media-video-corpus-sanitize
	@test -n '$(MEDIA_CORPUS_DIR)' -a -x '$(MEDIA_HELPER)' || { echo 'configure explicit native corpus output directory and instrumented helper'; exit 2; }
	sh tests/media_late_window.sh ./build/test-media-video-corpus-sanitize '$(MEDIA_HELPER)' '$(MEDIA_CORPUS_DIR)'
build/test-media-orientation-fixture: tests/test_media_orientation_fixture.c src/sources.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_media_orientation_fixture.c -o $@
.PHONY: test-media-orientation sanitize-media-orientation
test-media-orientation: build/test-media-video-corpus build/test-media-orientation-fixture media-worker
	@test -n '$(MEDIA_CORPUS_DIR)' || { echo 'configure explicit native corpus output directory'; exit 2; }
	sh tests/media_orientation_corpus.sh ./build/test-media-video-corpus '$(CURDIR)/build/samosa-sources-media-worker' ./build/test-media-orientation-fixture '$(MEDIA_CORPUS_DIR)'
sanitize-media-orientation: build/test-media-video-corpus-sanitize build/test-media-orientation-fixture
	@test -n '$(MEDIA_CORPUS_DIR)' -a -x '$(MEDIA_HELPER)' || { echo 'configure explicit native corpus output directory and instrumented helper'; exit 2; }
	sh tests/media_orientation_corpus.sh ./build/test-media-video-corpus-sanitize '$(MEDIA_HELPER)' ./build/test-media-orientation-fixture '$(MEDIA_CORPUS_DIR)'
test-media-video-corpus: build/test-media-video-corpus media-worker build/test-media-vp8-fixture
	@test -n '$(MEDIA_CORPUS_DIR)' || { echo 'configure explicit native corpus output directory'; exit 2; }
	sh tests/media_video_corpus.sh ./build/test-media-video-corpus '$(CURDIR)/build/samosa-sources-media-worker' '$(MEDIA_CORPUS_DIR)' '$(CURDIR)/build/test-media-vp8-fixture'
sanitize-media-video-corpus: build/test-media-video-corpus-sanitize build/test-media-vp8-fixture
	@test -n '$(MEDIA_CORPUS_DIR)' -a -x '$(MEDIA_HELPER)' || { echo 'configure explicit native corpus output directory and instrumented helper'; exit 2; }
	sh tests/media_video_corpus.sh ./build/test-media-video-corpus-sanitize '$(MEDIA_HELPER)' '$(MEDIA_CORPUS_DIR)' '$(CURDIR)/build/test-media-vp8-fixture'
build/test-media-video-corpus: tests/test_media_video_corpus.c src/media_video.c src/media_video.h src/media_input.c src/media_decode.c src/media_artifacts.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) tests/test_media_video_corpus.c src/media_video.c src/media_input.c src/media_decode.c src/media_artifacts.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o $@
build/test-media-video-corpus-sanitize: tests/test_media_video_corpus.c src/media_video.c src/media_video.h src/media_input.c src/media_decode.c src/media_artifacts.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) tests/test_media_video_corpus.c src/media_video.c src/media_input.c src/media_decode.c src/media_artifacts.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o $@
build/test-media-cache: tests/test_media_cache.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_media_cache.c $(SOURCES) $(LIBS) -o $@
build/test-media-cache-control: tests/test_media_cache.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_media_cache.c $(SOURCES) $(LIBS) -o $@
build/test-media-cache-control-sanitize: tests/test_media_cache.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_media_cache.c $(SOURCES) $(LIBS) -o $@
.PHONY: test-media-cache test-media-public
# MEDIA_MODE=--visual selects frame cache checks; MEDIA_KIND=visual selects the CLI.
test-media-cache: build/test-media-cache
	@test -n '$(MEDIA_FIXTURE)' -a -x '$(MEDIA_HELPER)' || { echo 'configure explicit native media fixture and helper'; exit 2; }
	./build/test-media-cache '$(MEDIA_HELPER)' '$(MEDIA_FIXTURE)' $(MEDIA_MODE)
test-media-public: all media-worker
	@test -n '$(MEDIA_FIXTURE)' || { echo 'configure explicit native media fixture'; exit 2; }
	sh tests/media_prepare_cli.sh ./build/samosa-sources '$(CURDIR)/build/samosa-sources-media-worker' '$(MEDIA_FIXTURE)' $(MEDIA_KIND)
.PHONY: sanitize-media-cache
sanitize-media-cache: | build
	@test -n '$(MEDIA_FIXTURE)' -a -x '$(MEDIA_HELPER)' || { echo 'configure explicit native media fixture and instrumented helper'; exit 2; }
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_media_cache.c $(SOURCES) $(LIBS) -o build/test-media-cache-sanitize
	./build/test-media-cache-sanitize '$(MEDIA_HELPER)' '$(MEDIA_FIXTURE)' $(MEDIA_MODE)
.PHONY: sanitize-media-public
sanitize-media-public: | build
	@test -n '$(MEDIA_FIXTURE)' || { echo 'configure explicit native media fixture'; exit 2; }
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc src/main.c $(SOURCES) $(LIBS) -o build/samosa-sources-media-public-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -DSAMOSA_SOURCES_MEDIA_WORKER -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) src/main.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c src/media_response.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o build/samosa-sources-media-public-worker-sanitize
	sh tests/media_prepare_cli.sh ./build/samosa-sources-media-public-sanitize '$(CURDIR)/build/samosa-sources-media-public-worker-sanitize' '$(MEDIA_FIXTURE)' $(MEDIA_KIND)
.PHONY: sanitize-media-worker
.PHONY: test-media-worker-faults
test-media-worker-faults: | build
	@test -n '$(MEDIA_FIXTURE)' || { echo 'configure explicit native media fixture'; exit 2; }
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -DSAMOSA_SOURCES_MEDIA_WORKER -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) src/main.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c src/media_response.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o build/samosa-sources-media-worker-faults
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_media_worker.c $(SOURCES) $(LIBS) -o build/test-media-worker-faults
	./build/test-media-worker-faults '$(CURDIR)/build/samosa-sources-media-worker-faults' '$(MEDIA_FIXTURE)' $(MEDIA_MODE)
build/samosa-sources-media-worker-sanitize: src/main.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c src/media_response.c src/media_decode.h src/media_video.h src/media_artifacts.h src/media_input.h $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -DSAMOSA_SOURCES_TEST_FAULTS -DSAMOSA_SOURCES_MEDIA_WORKER -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) src/main.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c src/media_response.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o build/samosa-sources-media-worker-sanitize
sanitize-media-worker: build/samosa-sources-media-worker-sanitize | build
	@test -n '$(MEDIA_FIXTURE)' || { echo 'configure explicit native media fixture'; exit 2; }
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_media_worker.c $(SOURCES) $(LIBS) -o build/test-media-worker-sanitize
	./build/test-media-worker-sanitize '$(CURDIR)/build/samosa-sources-media-worker-sanitize' '$(MEDIA_FIXTURE)' $(MEDIA_MODE)
media-worker: build/samosa-sources-media-worker
build/test-media-worker: tests/test_media_worker.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_media_worker.c $(SOURCES) $(LIBS) -o $@
build/samosa-sources-media-worker: src/main.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c src/media_response.c src/media_decode.h src/media_video.h src/media_artifacts.h src/media_input.h $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_MEDIA_WORKER -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) src/main.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c src/media_response.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o $@
build/test-media-primitive: tests/test_media_decode.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c src/media_artifacts.h src/media_decode.h src/media_input.h $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) tests/test_media_decode.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o $@
test-media-primitive: build/test-media-primitive
	@test -n '$(MEDIA_FIXTURE)' || { echo 'configure explicit native media fixture'; exit 2; }
	./build/test-media-primitive '$(MEDIA_FIXTURE)'
sanitize-media-primitive: | build
	@test -n '$(MEDIA_FIXTURE)' || { echo 'configure explicit native media fixture'; exit 2; }
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc $$(pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libavfilter) tests/test_media_decode.c src/media_decode.c src/media_video.c src/media_input.c src/media_artifacts.c $(SOURCES) $(LIBS) $$(pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libavfilter) -o build/test-media-primitive-sanitize
	./build/test-media-primitive-sanitize '$(MEDIA_FIXTURE)'
.PHONY: test-audio sanitize-audio
build/test-audio: tests/test_audio.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_audio.c $(SOURCES) $(LIBS) -o $@
test-audio: all build/test-audio
	./build/test-audio
	sh tests/audio_cli.sh ./build/samosa-sources ./build/test-audio
sanitize-audio: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_audio.c $(SOURCES) $(LIBS) -o build/test-audio-sanitize
	./build/test-audio-sanitize
test: test-large-sources test-audio
sanitize: sanitize-large-sources sanitize-audio
.PHONY: test-ocr-primitive sanitize-ocr-primitive test-ocr-pdf sanitize-ocr-pdf
.PHONY: test-ocr-cli ocr-fixture
.PHONY: test-ocr-corpus sanitize-ocr-corpus
build/ocr-corpus: tests/ocr_corpus.c | build
	$(CC) $(CFLAGS) $(WARN) $$(pkg-config --cflags lept) $< $$(pkg-config --libs lept) -o $@
test-ocr-corpus: all build/ocr-corpus
	@test -n '$(TESSERACT_LIBRARY)' -a -n '$(LEPTONICA_LIBRARY)' -a -n '$(OCR_TRAINEDDATA)' -a -n '$(OCR_FIXTURE)' || { echo 'configure explicit OCR libraries, model and fixture'; exit 2; }
	sh tests/ocr_corpus.sh ./build/samosa-sources ./build/ocr-corpus '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)'
	sh tests/ocr_malformed.sh ./build/samosa-sources ./build/ocr-corpus '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)'
sanitize-ocr-corpus: build/ocr-corpus | build
	@test -n '$(TESSERACT_LIBRARY)' -a -n '$(LEPTONICA_LIBRARY)' -a -n '$(OCR_TRAINEDDATA)' -a -n '$(OCR_FIXTURE)' || { echo 'configure explicit OCR libraries, model and fixture'; exit 2; }
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer src/main.c $(SOURCES) $(LIBS) -o build/samosa-sources-corpus-sanitize
	sh tests/ocr_corpus.sh ./build/samosa-sources-corpus-sanitize ./build/ocr-corpus '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)'
	sh tests/ocr_malformed.sh ./build/samosa-sources-corpus-sanitize ./build/ocr-corpus '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)'
build/ocr-fixture: tests/ocr_fixture.c | build
	$(CC) $(CFLAGS) $(WARN) $$(pkg-config --cflags freetype2) tests/ocr_fixture.c $$(pkg-config --libs freetype2) -o $@
ocr-fixture: build/ocr-fixture
	@test -n '$(OCR_FONT)' -a -n '$(OCR_FIXTURE)' || { echo 'configure explicit fixture font and output'; exit 2; }
	./build/ocr-fixture '$(OCR_FIXTURE)' '$(OCR_FONT)'
test-ocr-cli: all
	@test -n '$(TESSERACT_LIBRARY)' -a -n '$(LEPTONICA_LIBRARY)' -a -n '$(OCR_TRAINEDDATA)' -a -n '$(OCR_FIXTURE)' || { echo 'configure explicit OCR libraries, model and fixture'; exit 2; }
	sh tests/ocr_cli.sh ./build/samosa-sources '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)'
build/test-ocr: tests/test_ocr.c tests/ocr_pdf_fixture.h src/ocr.c src/ocr.h $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_ocr.c $(SOURCES) $(LIBS) -o $@
test-ocr-primitive: build/test-ocr
	@test -n '$(TESSERACT_LIBRARY)' -a -n '$(LEPTONICA_LIBRARY)' -a -n '$(OCR_TRAINEDDATA)' -a -n '$(OCR_FIXTURE)' || { echo 'configure explicit OCR libraries, model and fixture'; exit 2; }
	./build/test-ocr '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)'
sanitize-ocr-primitive: | build
	@test -n '$(TESSERACT_LIBRARY)' -a -n '$(LEPTONICA_LIBRARY)' -a -n '$(OCR_TRAINEDDATA)' -a -n '$(OCR_FIXTURE)' || { echo 'configure explicit OCR libraries, model and fixture'; exit 2; }
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_ocr.c $(SOURCES) $(LIBS) -o build/test-ocr-sanitize
	./build/test-ocr-sanitize '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)'
test-ocr-pdf: build/test-ocr
	@test -n '$(TESSERACT_LIBRARY)' -a -n '$(LEPTONICA_LIBRARY)' -a -n '$(OCR_TRAINEDDATA)' -a -n '$(OCR_FIXTURE)' -a -n '$(PDFIUM_LIBRARY)' || { echo 'configure explicit PDFium and OCR libraries, model and fixture'; exit 2; }
	./build/test-ocr '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)' '$(PDFIUM_LIBRARY)'
.PHONY: test-index-pdf-ocr sanitize-index-pdf-ocr
test-index-pdf-ocr: build/test-ocr chutni-worker
	@test -n '$(CHUTNI_LIBRARY)' -a -n '$(PDFIUM_LIBRARY)' -a -n '$(TESSERACT_LIBRARY)' -a -n '$(LEPTONICA_LIBRARY)' -a -n '$(OCR_TRAINEDDATA)' -a -n '$(OCR_FIXTURE)' || { echo 'configure explicit PDFium, OCR and Chutni dependencies'; exit 2; }
	./build/test-ocr '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)' '$(PDFIUM_LIBRARY)' '$(CHUTNI_LIBRARY)' '$(CURDIR)/build/samosa-sources-chutni-worker'
sanitize-index-pdf-ocr: build/samosa-sources-chutni-worker-sanitize | build
	@test -n '$(CHUTNI_LIBRARY)' -a -n '$(PDFIUM_LIBRARY)' -a -n '$(TESSERACT_LIBRARY)' -a -n '$(LEPTONICA_LIBRARY)' -a -n '$(OCR_TRAINEDDATA)' -a -n '$(OCR_FIXTURE)' || { echo 'configure explicit PDFium, OCR and Chutni dependencies'; exit 2; }
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_ocr.c $(SOURCES) $(LIBS) -o build/test-index-pdf-ocr-sanitize
	./build/test-index-pdf-ocr-sanitize '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)' '$(PDFIUM_LIBRARY)' '$(CHUTNI_LIBRARY)' '$(CURDIR)/build/samosa-sources-chutni-worker-sanitize'
sanitize-ocr-pdf: | build
	@test -n '$(TESSERACT_LIBRARY)' -a -n '$(LEPTONICA_LIBRARY)' -a -n '$(OCR_TRAINEDDATA)' -a -n '$(OCR_FIXTURE)' -a -n '$(PDFIUM_LIBRARY)' || { echo 'configure explicit PDFium and OCR libraries, model and fixture'; exit 2; }
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_ocr.c $(SOURCES) $(LIBS) -o build/test-ocr-sanitize
	./build/test-ocr-sanitize '$(TESSERACT_LIBRARY)' '$(LEPTONICA_LIBRARY)' '$(OCR_TRAINEDDATA)' '$(OCR_FIXTURE)' '$(PDFIUM_LIBRARY)'
.PHONY: test-html-parser sanitize-html-parser
.PHONY: test-docx-parser sanitize-docx-parser
.PHONY: test-docx sanitize-docx
build/test-docx-source: tests/test_docx_source.c tests/docx_fixture.h $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_docx_source.c $(SOURCES) $(LIBS) -o $@
build/docx-fixture: tests/docx_fixture.c tests/docx_fixture.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) tests/docx_fixture.c -o $@
test-docx: all test-docx-parser build/test-docx-source build/docx-fixture
	./build/test-docx-source
	sh tests/docx_cli.sh ./build/samosa-sources ./build/docx-fixture
sanitize-docx: sanitize-docx-parser | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_docx_source.c $(SOURCES) $(LIBS) -o build/test-docx-source-sanitize
	./build/test-docx-source-sanitize
test: test-docx
sanitize: sanitize-docx
DOCX_SOURCES = src/samosa_docx.c src/samosa_html.c vendor/miniz/miniz.c vendor/miniz/miniz_tinfl.c vendor/miniz/miniz_zip.c
DOCX_HEADERS = src/samosa_docx.h src/samosa_html.h vendor/miniz/miniz.h vendor/miniz/miniz_common.h vendor/miniz/miniz_export.h vendor/miniz/miniz_tdef.h vendor/miniz/miniz_tinfl.h vendor/miniz/miniz_zip.h
build/test-docx-parser: tests/test_docx.c tests/docx_fixture.h $(DOCX_SOURCES) $(DOCX_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc -isystem vendor/miniz tests/test_docx.c $(DOCX_SOURCES) -o $@
test-docx-parser: build/test-docx-parser
	./build/test-docx-parser
sanitize-docx-parser: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc -isystem vendor/miniz tests/test_docx.c $(DOCX_SOURCES) -o build/test-docx-parser-sanitize
	./build/test-docx-parser-sanitize
.PHONY: test-html sanitize-html
build/test-html-source: tests/test_html_source.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_html_source.c $(SOURCES) $(LIBS) -o $@
test-html: all test-html-parser build/test-html-source
	./build/test-html-source
	sh tests/html_cli.sh ./build/samosa-sources
sanitize-html: sanitize-html-parser | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_html_source.c $(SOURCES) $(LIBS) -o build/test-html-source-sanitize
	./build/test-html-source-sanitize
test: test-html
sanitize: sanitize-html
build/test-html-parser: tests/test_html.c src/samosa_html.c src/samosa_html.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_html.c src/samosa_html.c -o $@
test-html-parser: build/test-html-parser
	./build/test-html-parser
sanitize-html-parser: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_html.c src/samosa_html.c -o build/test-html-parser-sanitize
	./build/test-html-parser-sanitize
all: build/samosa-sources
.PHONY: test-control sanitize-control
build/test-control: tests/test_control.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_control.c $(SOURCES) $(LIBS) -o $@
test-control: build/test-control
	./build/test-control
sanitize-control: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_control.c $(SOURCES) $(LIBS) -o build/test-control-sanitize
	./build/test-control-sanitize
test: test-control
sanitize: sanitize-control
build:
	mkdir -p build
build/samosa-sources: src/main.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) src/main.c $(SOURCES) $(LIBS) -o $@
build/test-sources: tests/test_sources.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_sources.c $(SOURCES) $(LIBS) -o $@
build/test-pdf: tests/test_pdf.c tests/pdf_fixture.h $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_pdf.c $(SOURCES) $(LIBS) -o $@
build/pdf-spy: tests/pdf_spy.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(SHARED) tests/pdf_spy.c $(LIBS) -o $@
build/pdf-fixture: tests/pdf_fixture.c tests/pdf_fixture.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) tests/pdf_fixture.c -o $@
build/test-inventory: tests/test_inventory.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_inventory.c $(SOURCES) $(LIBS) -o $@
build/test-index: tests/test_index.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_index.c $(SOURCES) $(LIBS) -o $@
build/test-scope: tests/test_scope.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_scope.c $(SOURCES) $(LIBS) -o $@
build/test-locator: tests/test_locator.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_locator.c $(SOURCES) $(LIBS) -o $@
test: all build/test-sources build/test-inventory build/test-index build/test-scope build/test-locator
	./build/test-sources
	./build/test-inventory
	./build/test-index
	./build/test-scope
	./build/test-locator
	sh tests/inventory_cli.sh ./build/samosa-sources
	sh tests/cli.sh ./build/samosa-sources
test-pdf: all build/test-pdf build/pdf-spy build/pdf-fixture
	@test -n '$(PDFIUM_LIBRARY)' || { echo 'configure external PDFIUM_LIBRARY; missing is not a native pass'; exit 2; }
	./build/test-pdf '$(PDFIUM_LIBRARY)' './build/pdf-spy'
	sh tests/pdf_cli.sh ./build/samosa-sources ./build/pdf-fixture '$(PDFIUM_LIBRARY)'
.PHONY: sanitize-pdf
sanitize-pdf: build/pdf-spy
	@test -n '$(PDFIUM_LIBRARY)' || { echo 'configure external PDFIUM_LIBRARY; missing is not a native pass'; exit 2; }
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_pdf.c $(SOURCES) $(LIBS) -o build/test-pdf-sanitize
	./build/test-pdf-sanitize '$(PDFIUM_LIBRARY)' './build/pdf-spy'
sanitize: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_locator.c $(SOURCES) $(LIBS) -o build/test-locator-sanitize
	./build/test-locator-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_scope.c $(SOURCES) $(LIBS) -o build/test-scope-sanitize
	./build/test-scope-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_index.c $(SOURCES) $(LIBS) -o build/test-index-sanitize
	./build/test-index-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_sources.c $(SOURCES) $(LIBS) -o build/test-sources-sanitize
	./build/test-sources-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_inventory.c $(SOURCES) $(LIBS) -o build/test-inventory-sanitize
	./build/test-inventory-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer src/main.c $(SOURCES) $(LIBS) -o build/samosa-sources-sanitize
	sh tests/cli.sh ./build/samosa-sources-sanitize
release: test
	mkdir -p dist
	COPYFILE_DISABLE=1 tar --no-xattrs --exclude='__pycache__' --exclude='*.pyc' --exclude='.DS_Store' --exclude='._*' -czf dist/samosa-sources-0.1.0.tar.gz Makefile README.md DEPENDENCIES.md LICENSE .gitignore src tests docs contracts vendor
clean:
	find build dist -depth -delete

.PHONY: test-operations sanitize-operations
build/test-operations: tests/test_operations.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_operations.c $(SOURCES) $(LIBS) -o $@
test-operations: build/test-operations
	./build/test-operations
sanitize-operations: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_operations.c $(SOURCES) $(LIBS) -o build/test-operations-sanitize
	./build/test-operations-sanitize
test: test-operations
sanitize: sanitize-operations

.PHONY: fetch-worker test-fetch sanitize-fetch
fetch-worker: build/samosa-sources-fetch-worker
build/samosa-sources-fetch-worker: src/main.c src/fetch_http.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_FETCH_WORKER -Isrc $$(pkg-config --cflags libcurl) src/main.c src/fetch_http.c $(SOURCES) $(LIBS) $$(pkg-config --libs libcurl) -o $@
build/test-fetch: tests/test_fetch.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_fetch.c $(SOURCES) $(LIBS) -o $@
test-fetch: all fetch-worker build/test-fetch
	./build/test-fetch '$(CURDIR)/build/samosa-sources-fetch-worker' '$(CURDIR)/build/samosa-sources'
FETCH_SANITIZE = -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
build/samosa-sources-fetch-worker-sanitize: src/main.c src/fetch_http.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -DSAMOSA_SOURCES_FETCH_WORKER -Isrc $$(pkg-config --cflags libcurl) src/main.c src/fetch_http.c $(SOURCES) $(LIBS) $$(pkg-config --libs libcurl) -o $@
build/samosa-sources-fetch-cli-sanitize: src/main.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -Isrc src/main.c $(SOURCES) $(LIBS) -o $@
build/test-fetch-sanitize: tests/test_fetch.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_fetch.c $(SOURCES) $(LIBS) -o $@
sanitize-fetch: build/samosa-sources-fetch-worker-sanitize build/samosa-sources-fetch-cli-sanitize build/test-fetch-sanitize
	./build/test-fetch-sanitize '$(CURDIR)/build/samosa-sources-fetch-worker-sanitize' '$(CURDIR)/build/samosa-sources-fetch-cli-sanitize'
.PHONY: test-fetch-tls sanitize-fetch-tls
build/test-fetch-tls: tests/test_fetch_tls.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc $$(pkg-config --cflags openssl) tests/test_fetch_tls.c $(SOURCES) $(LIBS) $$(pkg-config --libs openssl) -o $@
test-fetch-tls: all fetch-worker build/test-fetch-tls
	./build/test-fetch-tls '$(CURDIR)/build/samosa-sources-fetch-worker' '$(CURDIR)/build/samosa-sources'
build/test-fetch-tls-sanitize: tests/test_fetch_tls.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -Isrc $$(pkg-config --cflags openssl) tests/test_fetch_tls.c $(SOURCES) $(LIBS) $$(pkg-config --libs openssl) -o $@
sanitize-fetch-tls: build/samosa-sources-fetch-worker-sanitize build/samosa-sources-fetch-cli-sanitize build/test-fetch-tls-sanitize
	./build/test-fetch-tls-sanitize '$(CURDIR)/build/samosa-sources-fetch-worker-sanitize' '$(CURDIR)/build/samosa-sources-fetch-cli-sanitize'
.PHONY: test-search sanitize-search
build/test-search: tests/test_search.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_search.c $(SOURCES) $(LIBS) -o $@
build/test-search-cli: tests/test_search_cli.c src/json.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_search_cli.c -o $@
test-search: all fetch-worker build/test-search build/test-search-cli
	./build/test-search '$(CURDIR)/build/samosa-sources-fetch-worker'
	./build/test-search-cli '$(CURDIR)/build/samosa-sources' '$(CURDIR)/build/samosa-sources-fetch-worker'
build/test-search-sanitize: tests/test_search.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -Isrc tests/test_search.c $(SOURCES) $(LIBS) -o $@
build/test-search-cli-sanitize: tests/test_search_cli.c src/json.h | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -Isrc tests/test_search_cli.c -o $@
sanitize-search: build/samosa-sources-fetch-worker-sanitize build/samosa-sources-fetch-cli-sanitize build/test-search-sanitize build/test-search-cli-sanitize
	./build/test-search-sanitize '$(CURDIR)/build/samosa-sources-fetch-worker-sanitize'
	./build/test-search-cli-sanitize '$(CURDIR)/build/samosa-sources-fetch-cli-sanitize' '$(CURDIR)/build/samosa-sources-fetch-worker-sanitize'

.PHONY: chutni-worker
build/samosa-sources-chutni-worker: src/main.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_CHUTNI_WORKER -Isrc src/main.c $(SOURCES) $(LIBS) -o $@
chutni-worker: build/samosa-sources-chutni-worker
build/samosa-sources-chutni-worker-sanitize: src/main.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) $(WARN) -DSAMOSA_SOURCES_CHUTNI_WORKER -Isrc src/main.c $(SOURCES) $(LIBS) -o $@
.PHONY: test-chutni sanitize-chutni
.PHONY: test-transcript-publication sanitize-transcript-publication
build/test-transcript-publication: tests/test_transcript_publication.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_transcript_publication.c $(SOURCES) $(LIBS) -o $@
build/test-transcript-publication-sanitize: tests/test_transcript_publication.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_transcript_publication.c $(SOURCES) $(LIBS) -o $@
test-transcript-publication: chutni-worker build/test-transcript-publication
	./build/test-transcript-publication '$(CURDIR)/build/samosa-sources-chutni-worker' '$(CHUTNI_LIBRARY)'
sanitize-transcript-publication: build/samosa-sources-chutni-worker-sanitize build/test-transcript-publication-sanitize
	./build/test-transcript-publication-sanitize '$(CURDIR)/build/samosa-sources-chutni-worker-sanitize' '$(CHUTNI_LIBRARY)'
build/test-chutni-validate: tests/test_chutni_validate.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -D_POSIX_C_SOURCE=200809L -Isrc tests/test_chutni_validate.c $(SOURCES) $(LIBS) -o $@
build/test-chutni-validate-sanitize: tests/test_chutni_validate.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) $(WARN) -D_POSIX_C_SOURCE=200809L -Isrc tests/test_chutni_validate.c $(SOURCES) $(LIBS) -o $@
build/test-chutni-sdk: tests/test_chutni_sdk.c src/json.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_chutni_sdk.c $(if $(filter Linux,$(shell uname -s)),-ldl,) -o $@
build/test-chutni-sdk-sanitize: tests/test_chutni_sdk.c src/json.h | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) $(WARN) -Isrc tests/test_chutni_sdk.c $(if $(filter Linux,$(shell uname -s)),-ldl,) -o $@
test-chutni: all chutni-worker build/test-chutni-validate build/test-chutni-sdk
	./build/test-chutni-validate
	@test -n '$(CHUTNI_LIBRARY)' || { echo 'Set CHUTNI_LIBRARY to the external public SDK shared library'; exit 1; }
	./build/test-chutni-sdk '$(CURDIR)/build/samosa-sources' '$(CURDIR)/build/samosa-sources-chutni-worker' '$(CHUTNI_LIBRARY)'
sanitize-chutni: build/samosa-sources-fetch-cli-sanitize build/samosa-sources-chutni-worker-sanitize build/test-chutni-validate-sanitize build/test-chutni-sdk-sanitize
	./build/test-chutni-validate-sanitize
	@test -n '$(CHUTNI_LIBRARY)' || { echo 'Set CHUTNI_LIBRARY to the external public SDK shared library'; exit 1; }
	./build/test-chutni-sdk-sanitize '$(CURDIR)/build/samosa-sources-fetch-cli-sanitize' '$(CURDIR)/build/samosa-sources-chutni-worker-sanitize' '$(CHUTNI_LIBRARY)'

build/samosa-sources-publication-test: src/main.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc src/main.c $(SOURCES) $(LIBS) -o $@
build/samosa-sources-publication-test-sanitize: src/main.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc src/main.c $(SOURCES) $(LIBS) -o $@
build/test-publication-native: tests/test_publication.c tests/publication_readers.h src/json.h src/sha256.h | build
	$(CC) $(CFLAGS) $(WARN) -Isrc tests/test_publication.c $(if $(filter Linux,$(shell uname -s)),-ldl,) -o $@
build/test-publication-native-sanitize: tests/test_publication.c tests/publication_readers.h src/json.h src/sha256.h | build
	$(CC) $(FETCH_SANITIZE) $(WARN) -Isrc tests/test_publication.c $(if $(filter Linux,$(shell uname -s)),-ldl,) -o $@
.PHONY: test-publication sanitize-publication
test-publication: all chutni-worker build/samosa-sources-publication-test build/pdf-fixture build/docx-fixture build/test-publication-native
	@test -n '$(CHUTNI_LIBRARY)' || { echo 'Set CHUTNI_LIBRARY to the external public SDK shared library'; exit 1; }
	SAMOSA_PUBLICATION_OCR_FIXTURE='$(OCR_FIXTURE)' SAMOSA_PUBLICATION_TESSERACT='$(TESSERACT_LIBRARY)' SAMOSA_PUBLICATION_LEPTONICA='$(LEPTONICA_LIBRARY)' SAMOSA_PUBLICATION_TRAINEDDATA='$(OCR_TRAINEDDATA)' SAMOSA_PUBLICATION_PDFIUM='$(PDFIUM_LIBRARY)' SAMOSA_PUBLICATION_PDF_FIXTURE='$(CURDIR)/build/pdf-fixture' SAMOSA_PUBLICATION_DOCX_FIXTURE='$(CURDIR)/build/docx-fixture' ./build/test-publication-native '$(CURDIR)/build/samosa-sources' '$(CURDIR)/build/samosa-sources-chutni-worker' '$(CHUTNI_LIBRARY)' '$(CURDIR)/build/samosa-sources-publication-test'
sanitize-publication: build/samosa-sources-fetch-cli-sanitize build/samosa-sources-chutni-worker-sanitize build/samosa-sources-publication-test-sanitize build/pdf-fixture build/docx-fixture build/test-publication-native-sanitize
	@test -n '$(CHUTNI_LIBRARY)' || { echo 'Set CHUTNI_LIBRARY to the external public SDK shared library'; exit 1; }
	SAMOSA_PUBLICATION_OCR_FIXTURE='$(OCR_FIXTURE)' SAMOSA_PUBLICATION_TESSERACT='$(TESSERACT_LIBRARY)' SAMOSA_PUBLICATION_LEPTONICA='$(LEPTONICA_LIBRARY)' SAMOSA_PUBLICATION_TRAINEDDATA='$(OCR_TRAINEDDATA)' SAMOSA_PUBLICATION_PDFIUM='$(PDFIUM_LIBRARY)' SAMOSA_PUBLICATION_PDF_FIXTURE='$(CURDIR)/build/pdf-fixture' SAMOSA_PUBLICATION_DOCX_FIXTURE='$(CURDIR)/build/docx-fixture' ./build/test-publication-native-sanitize '$(CURDIR)/build/samosa-sources-fetch-cli-sanitize' '$(CURDIR)/build/samosa-sources-chutni-worker-sanitize' '$(CHUTNI_LIBRARY)' '$(CURDIR)/build/samosa-sources-publication-test-sanitize'

.PHONY: test-visual-publication sanitize-visual-publication
build/test-visual-publication: tests/test_visual_publication.c tests/visual_fixture.h $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_visual_publication.c $(SOURCES) $(LIBS) -o $@
build/test-visual-publication-sanitize: tests/test_visual_publication.c tests/visual_fixture.h $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) $(WARN) -Isrc tests/test_visual_publication.c $(SOURCES) $(LIBS) -o $@
test-visual-publication: media-worker chutni-worker build/test-visual-publication
	./build/test-visual-publication '$(CURDIR)/build/samosa-sources-media-worker' '$(MEDIA_FIXTURE)' '$(CURDIR)/build/samosa-sources-chutni-worker' '$(CHUTNI_LIBRARY)'
sanitize-visual-publication: build/samosa-sources-media-worker-sanitize build/samosa-sources-chutni-worker-sanitize build/test-visual-publication-sanitize
	./build/test-visual-publication-sanitize '$(CURDIR)/build/samosa-sources-media-worker-sanitize' '$(MEDIA_FIXTURE)' '$(CURDIR)/build/samosa-sources-chutni-worker-sanitize' '$(CHUTNI_LIBRARY)'

.PHONY: test-visual-queue sanitize-visual-queue
build/test-visual-queue: tests/test_visual_queue.c tests/visual_fixture.h $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_visual_queue.c $(SOURCES) $(LIBS) -o $@
build/test-visual-queue-sanitize: tests/test_visual_queue.c tests/visual_fixture.h $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_visual_queue.c $(SOURCES) $(LIBS) -o $@
test-visual-queue: media-worker chutni-worker build/test-visual-queue
	./build/test-visual-queue '$(CURDIR)/build/samosa-sources-media-worker' '$(MEDIA_FIXTURE)' '$(CURDIR)/build/samosa-sources-chutni-worker' '$(CHUTNI_LIBRARY)'
sanitize-visual-queue: build/samosa-sources-media-worker-sanitize build/samosa-sources-chutni-worker-sanitize build/test-visual-queue-sanitize
	./build/test-visual-queue-sanitize '$(CURDIR)/build/samosa-sources-media-worker-sanitize' '$(MEDIA_FIXTURE)' '$(CURDIR)/build/samosa-sources-chutni-worker-sanitize' '$(CHUTNI_LIBRARY)'

.PHONY: test-index-visual sanitize-index-visual
build/test-index-visual: tests/test_index_visual.c tests/visual_fixture.h $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc $< $(SOURCES) $(LIBS) -o $@
build/test-index-visual-sanitize: tests/test_index_visual.c tests/visual_fixture.h $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc $< $(SOURCES) $(LIBS) -o $@
test-index-visual: media-worker build/test-index-visual
	./build/test-index-visual '$(CURDIR)/build/samosa-sources-media-worker' '$(MEDIA_FIXTURE)'
sanitize-index-visual: build/samosa-sources-media-worker-sanitize build/test-index-visual-sanitize
	./build/test-index-visual-sanitize '$(CURDIR)/build/samosa-sources-media-worker-sanitize' '$(MEDIA_FIXTURE)'

.PHONY: test-semantic sanitize-semantic
build/test-semantic: tests/test_semantic.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc $< $(SOURCES) $(LIBS) -o $@
build/test-semantic-sanitize: tests/test_semantic.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -Isrc $< $(SOURCES) $(LIBS) -o $@
test-semantic: chutni-worker build/test-semantic build/test-semantic-bounds
	./build/test-semantic-bounds
	./build/test-semantic '$(CHUTNI_LIBRARY)' '$(CURDIR)/build/samosa-sources-chutni-worker'
sanitize-semantic: build/samosa-sources-chutni-worker-sanitize build/test-semantic-sanitize build/test-semantic-bounds-sanitize
	./build/test-semantic-bounds-sanitize
	./build/test-semantic-sanitize '$(CHUTNI_LIBRARY)' '$(CURDIR)/build/samosa-sources-chutni-worker-sanitize'

build/test-semantic-bounds: tests/test_semantic_bounds.c src/semantic.c src/semantic.h src/json.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_semantic_bounds.c src/semantic.c -lm -o $@
build/test-semantic-bounds-sanitize: tests/test_semantic_bounds.c src/semantic.c src/semantic.h src/json.h | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -Isrc tests/test_semantic_bounds.c src/semantic.c -lm -o $@
.PHONY: test-summary sanitize-summary
build/test-summary: tests/test_summary.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_summary.c $(SOURCES) $(LIBS) -o $@
test-summary: build/test-summary
	./build/test-summary
sanitize-summary: $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_summary.c $(SOURCES) $(LIBS) -o build/test-summary-sanitize
	./build/test-summary-sanitize
build/test-summary-real: tests/test_summary_real.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_summary_real.c $(SOURCES) $(LIBS) -o $@
build/test-summary-real-sanitize: tests/test_summary_real.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/test_summary_real.c $(SOURCES) $(LIBS) -o $@

.PHONY: test-summary-publication sanitize-summary-publication
test-summary-publication: chutni-worker build/test-summary
	./build/test-summary '$(CURDIR)/build/samosa-sources-chutni-worker' '$(CHUTNI_LIBRARY)'
sanitize-summary-publication: sanitize-summary build/samosa-sources-chutni-worker-sanitize
	./build/test-summary-sanitize '$(CURDIR)/build/samosa-sources-chutni-worker-sanitize' '$(CHUTNI_LIBRARY)'

.PHONY: test-enrichment sanitize-enrichment
build/test-enrichment: tests/test_enrichment.c tests/docx_fixture.h tests/pdf_fixture.h tests/ocr_pdf_fixture.h $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_enrichment.c $(SOURCES) $(LIBS) -o $@
build/test-enrichment-sanitize: tests/test_enrichment.c tests/docx_fixture.h tests/pdf_fixture.h tests/ocr_pdf_fixture.h $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc tests/test_enrichment.c $(SOURCES) $(LIBS) -o $@
test-enrichment: chutni-worker build/test-enrichment
	./build/test-enrichment '$(CURDIR)/build/samosa-sources-chutni-worker' '$(CHUTNI_LIBRARY)'
sanitize-enrichment: build/samosa-sources-chutni-worker-sanitize build/test-enrichment-sanitize
	./build/test-enrichment-sanitize '$(CURDIR)/build/samosa-sources-chutni-worker-sanitize' '$(CHUTNI_LIBRARY)'

.PHONY: test-embedding sanitize-embedding
build/test-embedding: tests/test_embedding.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc $< $(SOURCES) $(LIBS) -o $@
build/test-embedding-sanitize: tests/test_embedding.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -DSAMOSA_SOURCES_TEST_FAULTS -Isrc $< $(SOURCES) $(LIBS) -o $@
test-embedding: chutni-worker build/test-embedding
	./build/test-embedding '$(CHUTNI_LIBRARY)' '$(CURDIR)/build/samosa-sources-chutni-worker'
sanitize-embedding: build/samosa-sources-chutni-worker-sanitize build/test-embedding-sanitize
	./build/test-embedding-sanitize '$(CHUTNI_LIBRARY)' '$(CURDIR)/build/samosa-sources-chutni-worker-sanitize'
build/test-embedding-real: tests/test_embedding_real.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc $< $(SOURCES) $(LIBS) -o $@
build/test-embedding-real-sanitize: tests/test_embedding_real.c $(SOURCES) $(HEADERS) $(SQLITE_BUILD_DEP) | build
	$(CC) $(CPPFLAGS) $(FETCH_SANITIZE) -Isrc $< $(SOURCES) $(LIBS) -o $@
