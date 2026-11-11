#include "http-gzip.h"
#include <stdio.h>
#include <stdlib.h>
#include "fixtures/http-gzip-models.h"

static size_t checks;
#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "HTTP gzip check failed at line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)

typedef struct {
    uint8_t *wire, *plain;
    size_t wire_size, plain_size;
    int outcome; /* 0: complete; 1: unfinished at EOF; 2: malformed */
} Sample;

/* Independent stored-DEFLATE encoder and bitwise CRC, without using miniz's
 * compressor/checksum routines or the implementation under test. */
static uint32_t crc32_bytes(const uint8_t *bytes, size_t size) {
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return crc ^ UINT32_C(0xffffffff);
}
static void le32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t) (value >> (i * 8));
}
static Sample make_sample(size_t size, int optional) {
    REQUIRE(size <= 65535);
    Sample sample = {0}; sample.plain_size = size;
    sample.plain = malloc(size ? size : 1); REQUIRE(sample.plain);
    for (size_t i = 0; i < size; ++i) sample.plain[i] = (uint8_t) ((i * 29 + 17) % 251);
    const size_t extra_size = optional == 2 ? 65535 : 4;
    const size_t name_size = optional == 2 ? 32768 : 9;
    const size_t header_size = optional ? 10 + 2 + extra_size + name_size + 8 + 2 : 10;
    sample.wire_size = header_size + 5 + size + 8;
    sample.wire = calloc(1, sample.wire_size); REQUIRE(sample.wire);
    uint8_t *wire = sample.wire;
    wire[0] = 31; wire[1] = 139; wire[2] = 8; wire[3] = optional ? 30 : 0; wire[9] = 255;
    size_t at = 10;
    if (optional) {
        wire[at++] = (uint8_t) extra_size; wire[at++] = (uint8_t) (extra_size >> 8);
        for (size_t i = 0; i < extra_size; ++i) wire[at++] = (uint8_t) i;
        memset(wire + at, 'n', name_size - 1); at += name_size;
        memcpy(wire + at, "comment", 7); at += 8;
        uint32_t crc = crc32_bytes(wire, at);
        wire[at++] = (uint8_t) crc; wire[at++] = (uint8_t) (crc >> 8);
    }
    REQUIRE(at == header_size);
    wire[at++] = 1; /* BFINAL=1, stored block, padding to the next byte. */
    wire[at++] = (uint8_t) size; wire[at++] = (uint8_t) (size >> 8);
    uint16_t complement = (uint16_t) ~size;
    wire[at++] = (uint8_t) complement; wire[at++] = (uint8_t) (complement >> 8);
    memcpy(wire + at, sample.plain, size); at += size;
    le32(wire + at, crc32_bytes(sample.plain, size)); at += 4;
    le32(wire + at, (uint32_t) size); at += 4;
    REQUIRE(at == sample.wire_size);
    return sample;
}
static Sample join(const Sample *a, const Sample *b) {
    Sample sample = {0}; sample.wire_size = a->wire_size + b->wire_size;
    sample.plain_size = a->plain_size + b->plain_size;
    sample.wire = malloc(sample.wire_size); sample.plain = malloc(sample.plain_size ? sample.plain_size : 1);
    REQUIRE(sample.wire && sample.plain);
    memcpy(sample.wire, a->wire, a->wire_size); memcpy(sample.wire + a->wire_size, b->wire, b->wire_size);
    memcpy(sample.plain, a->plain, a->plain_size); memcpy(sample.plain + a->plain_size, b->plain, b->plain_size);
    return sample;
}
static void release(Sample *sample) {free(sample->wire); free(sample->plain);}
static Sample from_model(const uint8_t *wire, size_t wire_size, size_t plain_size, bool entropy) {
    Sample sample = make_sample(plain_size, 0);
    free(sample.wire); sample.wire = malloc(wire_size); REQUIRE(sample.wire);
    sample.wire_size = wire_size; memcpy(sample.wire, wire, wire_size);
    if (entropy) {
        uint32_t value = UINT32_C(0x12345678);
        for (size_t i = 0; i < plain_size; ++i) {
            value ^= value << 13; value ^= value >> 17; value ^= value << 5;
            sample.plain[i] = (uint8_t) value;
        }
    }
    return sample;
}

static void run(JSContext *ctx, const Sample *sample, size_t split, size_t stride) {
    TJSHttpGzip *decoder = http_gzip_create(ctx); REQUIRE(decoder);
    size_t position = 0, output = 0, iterations = 0;
    int status = 0;
    while (position < sample->wire_size && status >= 0) {
        size_t end = stride ? position + stride : position < split ? split : sample->wire_size;
        if (end > sample->wire_size) end = sample->wire_size;
        REQUIRE(end > position);
        do {
            TBuf out; tbuf_init(ctx, &out); size_t consumed = SIZE_MAX;
            status = http_gzip_step(decoder, sample->wire + position, end - position, &consumed, &out);
            REQUIRE(consumed <= end - position); REQUIRE(out.size <= 4096);
            REQUIRE(out.allocated_size <= 6144);
            if (status >= 0) {
                REQUIRE(output + out.size <= sample->plain_size);
                REQUIRE(!out.size || !memcmp(sample->plain + output, out.buf, out.size));
                output += out.size;
            }
            position += consumed; tbuf_free(&out);
            REQUIRE(++iterations <= sample->wire_size + sample->plain_size + 10);
        } while (status == 2 || (status == 1 && position < end));
        if (status >= 0) REQUIRE(position == end);
    }
    if (sample->outcome == 2) {
        REQUIRE(status == -1); REQUIRE(decoder->state == HTTP_GZIP_FAILED);
        TBuf out; tbuf_init(ctx, &out); size_t consumed = SIZE_MAX;
        REQUIRE(http_gzip_step(decoder, sample->wire, sample->wire_size, &consumed, &out) == -1);
        REQUIRE(consumed == 0 && out.size == 0); tbuf_free(&out);
    } else if (sample->outcome == 1) {
        REQUIRE(status >= 0); REQUIRE(decoder->state != HTTP_GZIP_DONE);
    } else {
        REQUIRE(status >= 0); REQUIRE(decoder->state == HTTP_GZIP_DONE);
        REQUIRE(output == sample->plain_size); REQUIRE(position == sample->wire_size);
    }
    http_gzip_destroy(decoder, JS_GetRuntime(ctx)); ++checks;
}
static void splits(JSContext *ctx, const Sample *sample) {
    run(ctx, sample, 0, 0);
    for (size_t split = 1; split < sample->wire_size; ++split) run(ctx, sample, split, 0);
    for (size_t stride = 1; stride <= 7; ++stride) run(ctx, sample, 0, stride);
}
int main(void) {
    JSRuntime *runtime = JS_NewRuntime(); REQUIRE(runtime);
    JSContext *ctx = JS_NewContext(runtime); REQUIRE(ctx);
    JS_SetMemoryLimit(runtime, 32 * 1024 * 1024);
    Sample empty = make_sample(0, 0), short_payload = make_sample(97, 0), optional = make_sample(97, 1);
    Sample aligned = make_sample(4096, 0), large = make_sample(8197, 0), wide = make_sample(16389, 0);
    Sample fixed = from_model(gzip_fixed, sizeof(gzip_fixed), 8197, false);
    Sample dynamic = from_model(gzip_dynamic, sizeof(gzip_dynamic), 16389, false);
    Sample entropy = from_model(gzip_entropy, sizeof(gzip_entropy), 16389, true);
    Sample pair = join(&short_payload, &aligned), leading = join(&empty, &short_payload), trailing = join(&short_payload, &empty), both = join(&empty, &empty);
    Sample *samples[] = {&empty, &short_payload, &optional, &aligned, &large, &wide, &fixed, &dynamic, &entropy, &pair, &leading, &trailing, &both};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) splits(ctx, samples[i]);
    Sample crc = make_sample(4097, 0); crc.wire[crc.wire_size - 8] ^= 1; crc.outcome = 2; splits(ctx, &crc);
    Sample size = make_sample(97, 0); size.wire[size.wire_size - 4] ^= 1; size.outcome = 2; splits(ctx, &size);
    Sample header_crc = make_sample(97, 1); header_crc.wire[33] ^= 1; header_crc.outcome = 2; splits(ctx, &header_crc);
    for (size_t byte = 0; byte < 4; ++byte) {
        Sample invalid = make_sample(97, 0); invalid.wire[byte] = byte == 3 ? 32 : 0; invalid.outcome = 2;
        splits(ctx, &invalid); release(&invalid);
    }
    Sample invalid_deflate = make_sample(97, 0); invalid_deflate.wire[10] = 7; invalid_deflate.outcome = 2; splits(ctx, &invalid_deflate);
    for (size_t cut = 0; cut < short_payload.wire_size; ++cut) {
        Sample truncated = short_payload; truncated.wire_size = cut; truncated.outcome = 1;
        run(ctx, &truncated, 0, 0); run(ctx, &truncated, 0, 1);
    }
    Sample huge_header = make_sample(17, 2);
    for (size_t stride = 1; stride <= 4096; stride *= 2) run(ctx, &huge_header, 0, stride);
    Sample suffix = join(&short_payload, &empty); suffix.wire[suffix.wire_size - 8] ^= 1; suffix.outcome = 2; splits(ctx, &suffix);
    Sample *remaining[] = {&crc, &size, &header_crc, &invalid_deflate, &huge_header, &suffix};
    for (size_t i = 0; i < sizeof(remaining) / sizeof(remaining[0]); ++i) release(remaining[i]);
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) release(samples[i]);
    JS_FreeContext(ctx); JS_FreeRuntime(runtime);
    printf("HTTP gzip parser: %zu independent fragmentation/error/member checks passed\n", checks);
    return 0;
}
