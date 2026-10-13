# Native dependencies

Optional timed visual decoder additionally uses the declared FFmpeg libswscale
and libavfilter SDK/runtime for bounded color conversion and PQ/HLG tone mapping.
Their actual mapped images join worker artifact fingerprints, including optional
libzimg used by zscale. Minimal builds still require no FFmpeg SDK/runtime.

C11 compiler and make for builds; libc/POSIX/pthreads and pinned bundled SQLite3.53.4
for runtime. Linux owned workers use glibc>=2.34 closefrom spawn actions.
Native C clients, shell and standard POSIX tools run acceptance checks, including
publication and fixture generation. Builds, tests and production require no
Python runtime. The dedicated Linux service uses child
subreaper support and /proc child enumeration for owned descendant cleanup.
Initial supported build hosts: macOS and Linux; Linux requires
its actual independent qualification before a release claim. SHA-256 primitive
is owned source derived from the existing low-level C implementation.

Core Sources and durable read-operation stores use a retained directory-bound
SQLite VFS by default. Existing `sources.sqlite3` and `read-operations.sqlite3`
names and schema are preserved; the old pathname reopen branches are removed.
Adapter and integrated tests cover private-file admission, rollback recovery,
writer exclusion, capacity, directory replacement and immutable evidence.

The default `SQLITE_MODE=bundled` compiles unmodified SQLite3.53.4 from
`vendor/sqlite` into a local static archive. Origin, public-domain dedication,
official archive SHA3-256 and source SHA-256 manifest accompany the source.
The native `verify-sqlite` gate streams64KiB buffers to verify exact source
hashes before compilation. Builds need no network or Homebrew runtime.

Optional existing-store retrieval dynamically loads the external Chutni0.3.0
public SDK, spec0.2. No Chutni private sources or headers are incorporated in
Sources. Hosts select its shared library and store. The isolated optional
worker uses the documented four-function JSON ABI. Indexed-text publication
additionally binds the public chutni_hash_bytes/chutni_hash_file/chutni_root_add/
chutni_source_find functions and requires the SDK put_file_outputs operation.
The previously qualified macOS0.3.0 library predates that operation; publication
uses an isolated fresh build from the existing vendored SDK source. Artifact content reads require the SDK source_context artifact_id and bounded
UTF-8 byte-range extension. Older replies without explicit range metadata are
refused. The version label alone does not establish operation availability. On Darwin, SDK builds
with POSIX/XSI declarations also need -D_DARWIN_C_SOURCE for SQLite BSD types.
Reference SDK Linux builds
require POSIX/XSI feature declarations: compile with
`-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700`, and reject implicit function
declarations. Without these, the reference C99 shared-library build hides strdup
and realpath prototypes on glibc and can truncate pointers. This is a dependency
build requirement, not an adapter fallback or a replacement SDK implementation.
Thread safety is enabled, loadable extensions disabled, and native Unix locking
preserved. Owned sanitizer gates instrument the adapter/component; the SQLite
archive is presently a normal dependency build.

`SQLITE_MODE=system` with an explicit `SQLITE_LIBS` remains an advanced dependency
selection. Linux system SQLite and macOS upstream/Homebrew SQLite have focused
qualification; macOS system SQLite is incompatible with this VFS. There is no
fallback to pathname-based storage. Common component and adapter targets track
compiler/flags/library selection in a configuration stamp. Independent fixture
rules may still need clean rebuilds after changing flags. Optional-media, service and independent release acceptance passes on macOS and
native ARM64 Linux; see docs/release-acceptance.md.

Optional PDF reader: configured PDFium C shared library, published headers pinned
152.0.7961.0 under vendor/pdfium-headers with license. Loaded only for read_pdf;
no link/startup dependence on the PDFium binary for text reading. libc dlopen on
macOS; libdl on Linux. PDFium build/platform release qualification is separate
from this narrow native extraction proof. PDF fixtures and spy are native C.

DOCX parser work: owned native OOXML reader and vendored miniz ZIP read/Inflate
sources under vendor/miniz, with preserved LICENSE. miniz_export.h disables
compression, ZIP writing, zlib compatibility and unused stdio interfaces. This
copy additionally guards three writer-only array helpers under the existing
MINIZ_NO_ARCHIVE_WRITING_APIS macro so read-only builds pass strict compiler
warnings. No algorithm or ZIP-reading behavior is changed by those guards.
Fixtures generate stored and raw Deflate ZIP packages in native C; no Python
or external archive command. Public DOCX delivery is qualified (S16b).

Optional image/PDF OCR: explicitly configured Tesseract and Leptonica shared
libraries and one traineddata file. The adapter uses their public native C ABI
through dlopen; the minimal build requires neither installed headers nor link
libraries. Plain readers work with OCR absent. Native OCR fixture rendering uses
FreeType with an explicitly configured font; FreeType is a fixture dependency,
not a product runtime dependency. No model directory/environment fallback or
automatic download. Initial actual OCR checks use Tesseract5.5.3/Leptonica1.87.0
on macOS and Debian Tesseract5.3.0/Leptonica1.82.0 on native ARM64 Linux.
See image-ocr-v1.md and pdf-ocr-v1.md for bounds and qualification limitations.

Expanded OCR corpus generation uses Leptonica development headers/libraries
through pkg-config lept only in the native fixture encoder. Encoders must support
BMP/JPEG/PNG/TIFF/GIF/WebP for this qualification target; missing support fails
qualification. Product builds still use only dynamically loaded optional OCR
libraries. Corpus CLI acceptance and its production ASan+UBSan variant check
actual recognition, typed truncated-input errors and a healthy survivor.

Optional compressed-media primitive (S18b in progress): native libavformat,
libavcodec, libavutil and libswresample SDKs through pkg-config. Only optional
media targets link them; the default build and WAV preparation do not. The
initial SDKs are FFmpeg9.0.2 avformat/avcodec63 on macOS and Debian FFmpeg5.1.9
avformat/avcodec59 on ARM64 Linux. Tests generate original WAV in owned C, use
native FFmpeg fixture encoders for MP3/AAC/stereo/video containers, and compare
selected PCM against native per-platform reference decoding. No Python tooling.
Owned worker integration, actual mapped artifact fingerprints, public compressed
audio and timed visual preparation/cache are implemented. Focused audio/visual
control and cache qualification passes on macOS and native ARM64 Linux. The visual
reference corpus additionally uses native FFmpeg CLI encoders libx264, libx265,
MPEG-4 and MJPEG, plus per-platform scale/RGBA reference decoding; these are test
dependencies, not minimal product requirements. Orthogonal display transformation uses native integer RGBA permutations and the
installed FFmpeg frame/stream side-data APIs, with no additional runtime library.
The admitted codec/color/timeline matrix and late-source preparation now have
native reference qualification. Upload, indexing, public Inference integration
and service acceptance is recorded in docs/release-acceptance.md.


The optional visual corpus now also generates VP9/MP4, full-range H.264,
10-bit SDR BT.2020 HEVC, delayed-video/early-audio tracks and variable-frame-rate
clips. It uses explicit `-fps_mode passthrough` for delayed tracks so encoder
frame duplication cannot hide timeline errors, and `-fps_mode vfr` for sparse
frame timestamps. Source decoding/scaling and exact public/cache timestamps are
compared with native platform references. PQ/HLG use the fixed native tone-map
policy in media-v1.md; SDR retains matrix/range scaling without general gamut
rendering. VP8 fixture construction reuses native encoding/muxing, disables
automatic VP9 bitstream filters in its sample-table shell, and proves original
VP8 packet hashes unchanged before testing independent WebM-reference pixels.
Native audio fixtures cover admitted PCM variants, CBR/VBR MP3, AAC with/without
PNS, nonaligned late selections and compact nearly-four-hour inspection. References
use sequential or declared native seek decoding as appropriate. The source archive includes its Apache2 LICENSE
alongside preserved vendored licenses.

Optional HTTP acquisition: libcurl SDK/runtime selected by pkg-config libcurl,
linked only into make fetch-worker and its optional sanitizer worker. The
Sources core and host API do not link libcurl. The worker uses the native
isolation protocol; no Python, browser, subprocess curl tool or sibling
component is a runtime dependency. HTTPS uses libcurl's installed TLS backend
and system trust configuration. Focused actual TLS normal/sanitizer qualification now passes on both platforms;
Public fetching, all six provider mappings and independent release acceptance
also pass; paid-provider availability is separate. Optional native TLS
fixtures require pkg-config openssl (OpenSSL3 SDK), used only in fixture binaries.
Reference fixture SDKs: macOS3.6.4, Linux3.0.22. The Linux qualification container
installed libssl-dev for these fixtures; minimal Sources still links no OpenSSL
SDK. See contracts/fetch-v1.md.

The optional public Inference process adapter uses only POSIX sockets, spawn and
poll through the existing Sources worker supervisor. It does not link Inference
or a model SDK. The host supplies a standalone public Inference executable and
an owner-private JSON argument file; plain readers need neither. Native C tests
qualify the adapter without a model. Actual Whisper engine/model qualification
is separate, using public component executables and installed pinned artifacts.
The native worker's Linux address-space limit cannot support ASan shadow memory;
sanitizer qualification instruments the worker as well as parent and fixture
provider, using the existing sanitizer exemption for that limit.
