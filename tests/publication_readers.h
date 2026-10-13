/* Included by the standalone CLI acceptance client, never production. */
#include "sha256.h"
static const char *configured(const char *name) {
    const char *s = getenv(name);
    return s && *s ? s : NULL;
}
static void version(const char *path, char digest[65]) {
    size_t n;
    char *s = file_bytes(path, &n);
    hash_bytes(s, n, digest);
    free(s);
}
static void generate(const char *exe, const char *path) {
    CHECK(exe && exe[0] == '/');
    pid_t p = fork();
    CHECK(p >= 0);
    if (!p) {
        execl(exe, exe, path, (char *)NULL);
        _exit(2);
    }
    own_child(p);
    int st = wait_child(p, 10);
    CHECK(WIFEXITED(st) && !WEXITSTATUS(st));
}
static char *search_job(const Host *h, const char *job, const char *query) {
    char q[1024];
    snprintf(q, sizeof(q),
             "{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%"
             "s\",\"query\":\"%s\"}",
             job, query);
    return run(h, q, 0, NULL);
}
static void finish_job(const Host *h, const char *job, const char *id,
                       size_t expected) {
    char *s = job_call(h, "index_get", job, NULL, 0, NULL);
    Json j;
    parse(s, &j);
    if (!eq(&j, 0, "status", "completed")) {
        free(s);
        free(job_call(h, "index_step", job, id, 0, NULL));
        s = job_call(h, "index_get", job, NULL, 0, NULL);
        parse(s, &j);
    }
    if (num(&j, 0, "failed_items"))
        fprintf(stderr, "index response: %s\n", s);
    CHECK(!num(&j, 0, "failed_items"));
    if (expected)
        CHECK(num(&j, 0, "indexed_items") == expected);
    free(s);
}
static char *read_artifact(const Host *h, const Json *j, int pub,
                           const char *id, int offline) {
    char *aid = str(j, pub, "external_artifact_id"),
         *sid = str(j, pub, "external_source_id");
    char q[1024];
    snprintf(q, sizeof(q),
             "{\"contract_version\":1,\"op\":\"memory_read\",\"store_id\":\"%"
             "s\",\"external_source_id\":\"%s\",\"external_artifact_id\":\"%"
             "s\",\"operation_id\":\"%s\"}",
             h->store_id, sid, aid, id);
    free(aid);
    free(sid);
    return run(h, q, offline, NULL);
}
static void docx_comment(const char *path) {
    size_t n;
    char *s = file_bytes(path, &n);
    CHECK(n >= 22 && !memcmp(s + n - 22, "PK\005\006", 4) && !s[n - 1] &&
          !s[n - 2]);
    const char comment[] = "reindexed version";
    s[n - 2] = (char)(sizeof(comment) - 1);
    write_bytes(path, s, n);
    free(s);
    FILE *f = fopen(path, "ab");
    CHECK(f &&
          fwrite(comment, 1, sizeof(comment) - 1, f) == sizeof(comment) - 1 &&
          !fclose(f));
}
static void reader_acceptance(Sdk *sdk, Host *h) {
    h->pdfium = configured("SAMOSA_PUBLICATION_PDFIUM");
    if (!h->pdfium)
        return;
    char paths[3][600];
    const char *names[] = {"reader.html", "reader.docx", "reader.pdf"},
               *readers[] = {"html-readable-v1", "docx-readable-v1",
                             "pdfium-text-v1"};
    for (size_t i = 0; i < 3; i++)
        snprintf(paths[i], sizeof(paths[i]), "%s/%s", h->files, names[i]);
    const char *html = "<html><body><p>Code &amp; café 7319</p><script>hidden "
                       "script secret</script></body></html>";
    write_bytes(paths[0], html, strlen(html));
    generate(configured("SAMOSA_PUBLICATION_DOCX_FIXTURE"), paths[1]);
    generate(configured("SAMOSA_PUBLICATION_PDF_FIXTURE"), paths[2]);
    char *job = index_job(h, "reader-index");
    finish_job(h, job, "reader-index-finish", 5);
    char *selected = search_job(h, job, "7319");
    Json j;
    parse(selected, &j);
    int a = at(&j, 0, "candidates");
    CHECK(j.tokens[a].count == 2);
    unsigned seen = 0;
    for (int t = a + 1; t < j.tokens[a].next; t = j.tokens[t].next) {
        char *name = str(&j, t, "locator");
        size_t k = !strcmp(name, names[0])   ? 0
                   : !strcmp(name, names[1]) ? 1
                                             : 3;
        CHECK(k < 2 && !(seen & (1u << k)));
        seen |= 1u << k;
        free(name);
        char digest[65];
        version(paths[k], digest);
        CHECK(
            eq(&j, t, "version", digest) &&
            eq(&j, at(&j, t, "range"), "coordinate_space", "extracted_utf8") &&
            eq(&j, at(&j, t, "coverage"), "status", "partial"));
        char *preview = str(&j, t, "literal_preview");
        CHECK(!strstr(preview, "hidden script secret"));
        free(preview);
    }
    CHECK(seen == 3);
    free(selected);
    selected = search_job(h, job, "absolute page");
    parse(selected, &j);
    a = at(&j, 0, "candidates");
    CHECK(j.tokens[a].count == 1);
    int range = at(&j, a + 1, "range");
    CHECK(num(&j, range, "start_page") == 1 &&
          num(&j, range, "end_page") == 4 &&
          eq(&j, at(&j, a + 1, "coverage"), "reason", "text_layer_only"));
    free(selected);
    size_t before = artifacts(sdk, h);
    char *publications[6] = {0};
    size_t published = 0;
    for (size_t i = 0; i < 6; i++) {
        char id[64];
        snprintf(id, sizeof(id), "publish-reader-%zu", i);
        char *s = publish(h, job, id, NULL);
        parse(s, &j);
        if (!num(&j, 0, "published_items")) {
            free(s);
            break;
        }
        CHECK(published < 6);
        publications[published++] = s;
    }
    CHECK(published == 5 && artifacts(sdk, h) == before + 3);
    for (size_t k = 0; k < 3; k++) {
        char *ctx = context(sdk, h, paths[k]);
        Json c;
        parse(ctx, &c);
        int art = artifact(&c, NULL, NULL);
        CHECK(art >= 0);
        int evidence = proof(&c, art);
        CHECK(eq(&c, at(&c, evidence, "provenance"), "reader", readers[k]) &&
              eq(&c, at(&c, evidence, "coverage"), "status", "partial"));
        if (k == 2) {
            int pages = at(&c, evidence, "pages");
            CHECK(c.tokens[pages].count == 4);
            size_t page = 1;
            for (int t = pages + 1; t < c.tokens[pages].next;
                 t = c.tokens[t].next, page++) {
                CHECK(num(&c, t, "page") == page);
                if (page == 2)
                    CHECK(eq(&c, t, "text_status", "empty_text_layer"));
            }
            CHECK(eq(&c, at(&c, art, "selector"), "type", "pages"));
        } else {
            CHECK(eq(&c, at(&c, art, "selector"), "coordinate_space",
                     "extracted_utf8"));
            char *content = str(&c, art, "content");
            CHECK(eq(&c, evidence, "text", content));
            free(content);
        }
        char *aid = str(&c, art, "artifact_id");
        int match = 0;
        for (size_t i = 0; i < published; i++) {
            parse(publications[i], &j);
            int pub = at(&j, 0, "publication");
            if (!eq(&j, pub, "external_artifact_id", aid))
                continue;
            CHECK(!match);
            match = 1;
            char id[64];
            snprintf(id, sizeof(id), "read-reader-%zu", k);
            char *observed = read_artifact(h, &j, pub, id, 0);
            Json r;
            parse(observed, &r);
            int result = at(&r, at(&r, 0, "memory"), "result");
            char *content = str(&c, art, "content");
            CHECK(eq(&r, result, "text", content));
            same(&r, proof(&r, result), &c, evidence);
            free(content);
            free(observed);
        }
        CHECK(match);
        free(aid);
        free(ctx);
    }
    for (size_t i = 0; i < published; i++)
        free(publications[i]);
    char old[3][65];
    for (size_t k = 0; k < 3; k++)
        version(paths[k], old[k]);
    html = "<html><body><p>Updated Code &amp; café 7319</p></body></html>";
    write_bytes(paths[0], html, strlen(html));
    docx_comment(paths[1]);
    FILE *pdf = fopen(paths[2], "ab");
    CHECK(pdf && fputs("\n% reindexed version\n", pdf) >= 0 && !fclose(pdf));
    char q[2048];
    snprintf(q, sizeof(q),
             "{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_"
             "id\":\"observe-reader-changes\",\"locators\":[{\"path\":\"reader."
             "html\",\"expected_version\":\"%s\"},{\"path\":\"reader.docx\","
             "\"expected_version\":\"%s\"},{\"path\":\"reader.pdf\",\"expected_"
             "version\":\"%s\"}]}",
             old[0], old[1], old[2]);
    free(run(h, q, 0, NULL));
    char *changed =
        job_call(h, "index_reindex_step", job, "reindex-readers", 0, NULL);
    parse(changed, &j);
    CHECK(num(&j, at(&j, 0, "result"), "reindexed_items") == 3);
    free(changed);
    selected = search_job(h, job, "7319");
    parse(selected, &j);
    CHECK(j.tokens[at(&j, 0, "candidates")].count == 4);
    free(selected);
    for (size_t i = 0; i < 3; i++) {
        char id[64];
        snprintf(id, sizeof(id), "publish-reindexed-reader-%zu", i);
        changed = publish(h, job, id, NULL);
        parse(changed, &j);
        CHECK(num(&j, 0, "published_items") == 1 &&
              num(&j, 0, "generation") == 1);
        free(changed);
    }
    CHECK(artifacts(sdk, h) == before + 6);
    free(job);
    puts("PASS native actual HTML/DOCX/PDF index/reindex publication/read; "
         "extracted coordinates, absolute pages, blank layers and retained "
         "identity");
}
static void ocr_acceptance(Sdk *sdk, Host *h) {
    const char *fixture = configured("SAMOSA_PUBLICATION_OCR_FIXTURE");
    if (!fixture)
        return;
    h->tesseract = configured("SAMOSA_PUBLICATION_TESSERACT");
    h->leptonica = configured("SAMOSA_PUBLICATION_LEPTONICA");
    h->traineddata = configured("SAMOSA_PUBLICATION_TRAINEDDATA");
    CHECK(h->tesseract && h->leptonica && h->traineddata);
    h->tesseract = realpath(h->tesseract, NULL);
    h->leptonica = realpath(h->leptonica, NULL);
    h->traineddata = realpath(h->traineddata, NULL);
    CHECK(h->tesseract && h->leptonica && h->traineddata);
    char image[600];
    snprintf(image, sizeof(image), "%s/recognized.ppm", h->files);
    size_t n;
    char *bytes = file_bytes(fixture, &n);
    write_bytes(image, bytes, n);
    free(bytes);
    char *job = index_job(h, "image-index");
    finish_job(h, job, "image-index-finish", 0);
    char *matches = search_job(h, job, "7319");
    Json j;
    parse(matches, &j);
    int candidates = at(&j, 0, "candidates"), candidate = -1;
    for (int t = candidates + 1; t < j.tokens[candidates].next;
         t = j.tokens[t].next)
        if (eq(&j, t, "locator", "recognized.ppm")) {
            CHECK(candidate < 0);
            candidate = t;
        }
    CHECK(candidate >= 0);
    char digest[65];
    version(image, digest);
    CHECK(eq(&j, candidate, "version", digest) &&
          eq(&j, at(&j, candidate, "range"), "coordinate_space",
             "extracted_utf8") &&
          eq(&j, at(&j, candidate, "coverage"), "status", "partial") &&
          eq(&j, at(&j, candidate, "coverage"), "reason",
             "ocr_recognition_uncertain") &&
          eq(&j, at(&j, candidate, "provenance"), "reader", "image-ocr-v1"));
    int recognition = at(&j, at(&j, candidate, "provenance"), "recognition");
    CHECK(eq(&j, recognition, "method", "tesseract"));
    int confidence_token = at(&j, recognition, "confidence");
    char *number = slice(&j, confidence_token), *tail = NULL;
    double confidence = strtod(number, &tail);
    CHECK(tail && !*tail && confidence >= 0 && confidence <= 100);
    free(number);
    int raster = at(&j, recognition, "raster");
    CHECK(num(&j, raster, "width") == 680 && num(&j, raster, "height") == 150 &&
          eq(&j, raster, "coordinate_space", "original_pixels"));
    char *recognized = slice(&j, recognition);
    free(matches);
    size_t before = artifacts(sdk, h);
    char *image_publication = NULL;
    for (size_t i = 0; i < 9; i++) {
        char id[64];
        snprintf(id, sizeof(id), "publish-image-job-%zu", i);
        char *s = publish(h, job, id, NULL);
        parse(s, &j);
        if (!num(&j, 0, "published_items")) {
            free(s);
            break;
        }
        if (eq(&j, at(&j, 0, "publication"), "version", digest)) {
            CHECK(!image_publication);
            image_publication = s;
        } else
            free(s);
    }
    size_t image_count = artifacts(sdk, h);
    size_t added = 1;
    if (h->pdfium) {
        char pdf_path[600];
        snprintf(pdf_path, sizeof(pdf_path), "%s/reader.pdf", h->files);
        char *extra = context(sdk, h, pdf_path);
        Json p;
        parse(extra, &p);
        int arts = at(&p, 0, "artifacts");
        size_t fallback = 0, text_versions = 0;
        for (int t = arts + 1; t < p.tokens[arts].next; t = p.tokens[t].next) {
            int evidence = proof(&p, t),
                provenance = at(&p, evidence, "provenance");
            if (eq(&p, provenance, "reader", "pdfium-index-fallback-v1")) {
                int coverage = at(&p, evidence, "coverage");
                CHECK(eq(&p, coverage, "status", "partial") &&
                      eq(&p, coverage, "reason",
                         "text_layer_with_ocr_fallback") &&
                      num(&p, coverage, "ocr_pages") == 1);
                fallback++;
            } else if (eq(&p, provenance, "reader", "pdfium-text-v1"))
                text_versions++;
        }
        CHECK(fallback == 1 && text_versions == 2);
        free(extra);
        added++;
    }
    CHECK(image_publication && image_count == before + added);
    parse(image_publication, &j);
    char *captured =
        read_artifact(h, &j, at(&j, 0, "publication"), "read-recognized", 0);
    char *offline =
        read_artifact(h, &j, at(&j, 0, "publication"), "read-recognized", 1);
    CHECK(!strcmp(captured, offline));
    free(offline);
    Json c;
    parse(captured, &c);
    int result = at(&c, at(&c, 0, "memory"), "result"),
        evidence = proof(&c, result);
    CHECK(eq(&c, at(&c, evidence, "coverage"), "reason",
             "ocr_recognition_uncertain"));
    char *observed =
        slice(&c, at(&c, at(&c, evidence, "provenance"), "recognition"));
    CHECK(!strcmp(observed, recognized));
    free(observed);
    free(recognized);
    char *text = str(&c, result, "text");
    CHECK(strstr(text, "7319") && eq(&c, evidence, "text", text) &&
          eq(&c, at(&c, result, "selector"), "coordinate_space",
             "extracted_utf8"));
    free(text);
    char *derivation =
        str(&c, at(&c, at(&c, result, "provenance"), "derivation"),
            "derivation_id");
    char *saved = catalog(
        sdk, h, "SELECT parameters_json FROM derivations WHERE derivation_id=?",
        derivation, NULL);
    parse(saved, &j);
    int altered_confidence =
        at(&j,
           at(&j, at(&j, at(&j, 0, "sources_evidence"), "provenance"),
              "recognition"),
           "confidence");
    confidence += 1;
    if (confidence >= 101)
        confidence -= 101;
    char *altered = NULL;
    size_t altered_n;
    FILE *out = open_memstream(&altered, &altered_n);
    CHECK(out);
    CHECK(fwrite(saved, 1, j.tokens[altered_confidence].begin, out) ==
          j.tokens[altered_confidence].begin);
    fprintf(out, "%.17g", confidence);
    fputs(saved + j.tokens[altered_confidence].end, out);
    CHECK(!fclose(out));
    free(catalog(
        sdk, h,
        "UPDATE derivations SET parameters_json=? WHERE derivation_id=?",
        altered, derivation));
    free(altered);
    char *repeated = index_job(h, "image-conflict-index");
    free(job_call(h, "index_step", repeated, "image-conflict-index-finish", 0,
                  NULL));
    int refused = 0;
    for (size_t i = 0; i < 9; i++) {
        char id[64];
        snprintf(id, sizeof(id), "publish-recognition-conflict-%zu", i);
        char *s = publish(h, repeated, id, "*");
        parse(s, &j);
        int err = json_get(&j, 0, "error");
        if (err >= 0) {
            CHECK(eq(&j, err, "code", "chutni_publication_conflict"));
            refused = 1;
            free(s);
            break;
        }
        CHECK(num(&j, 0, "published_items") == 1);
        free(s);
    }
    CHECK(refused && artifacts(sdk, h) == image_count);
    free(catalog(
        sdk, h,
        "UPDATE derivations SET parameters_json=? WHERE derivation_id=?", saved,
        derivation));
    free(saved);
    free(derivation);
    pause_ms(2100);
    char *repaired = publish(h, repeated, "publish-recognition-repaired", NULL);
    parse(repaired, &j);
    CHECK(truth(&j, at(&j, 0, "publication"), "recovered"));
    free(repaired);
    free(repeated);
    bytes = file_bytes(image, &n);
    CHECK(n >= 3 && !memcmp(bytes, "P6\n", 3));
    out = fopen(image, "wb");
    CHECK(out && fputs("P6\n# retained OCR version two\n", out) >= 0 &&
          fwrite(bytes + 3, 1, n - 3, out) == n - 3 && !fclose(out));
    free(bytes);
    char q[1024];
    snprintf(q, sizeof(q),
             "{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_"
             "id\":\"observe-image-change\",\"locators\":[{\"path\":"
             "\"recognized.ppm\",\"expected_version\":\"%s\"}]}",
             digest);
    free(run(h, q, 0, NULL));
    char *changed =
        job_call(h, "index_reindex_step", job, "reindex-image", 0, NULL);
    parse(changed, &j);
    CHECK(num(&j, at(&j, 0, "result"), "reindexed_items") == 1);
    free(changed);
    changed = publish(h, job, "publish-image-version-two", NULL);
    parse(changed, &j);
    CHECK(num(&j, 0, "published_items") == 1 && num(&j, 0, "generation") == 1 &&
          !eq(&j, at(&j, 0, "publication"), "version", digest) &&
          artifacts(sdk, h) == image_count + 1);
    free(changed);
    free(captured);
    free(image_publication);
    free(job);
    puts("PASS native actual image OCR index/reindex publication/read/offline; "
         "uncertainty/confidence/raster, retained versions and forged "
         "provenance refusal");
    free((void *)h->tesseract);
    free((void *)h->leptonica);
    free((void *)h->traineddata);
    h->tesseract = h->leptonica = h->traineddata = NULL;
}
