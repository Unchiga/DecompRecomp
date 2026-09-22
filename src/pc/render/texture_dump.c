#include "texture_dump.h"
#include "soft_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_directory(path) _mkdir(path)
#else
#define make_directory(path) mkdir(path, 0777)
#endif

int TextureDump_Enabled;
static char directory[1024];
static FILE *index_file;

/* Hashes already written, in an open-addressed table that doubles. */
static uint64_t *seen;
static size_t seen_count, seen_capacity;

static int remember(uint64_t hash)
{
    size_t i;
    if (seen_count * 2 >= seen_capacity) {
        size_t capacity = seen_capacity ? seen_capacity * 2 : 4096, j;
        uint64_t *table = calloc(capacity, sizeof(*table));
        if (!table) return 0;
        for (j = 0; j < seen_capacity; j++) {
            if (seen[j]) {
                size_t k = (size_t)seen[j] & (capacity - 1);
                while (table[k]) k = (k + 1) & (capacity - 1);
                table[k] = seen[j];
            }
        }
        free(seen);
        seen = table;
        seen_capacity = capacity;
    }
    if (!hash) hash = 1;
    for (i = (size_t)hash & (seen_capacity - 1); seen[i]; i = (i + 1) & (seen_capacity - 1)) {
        if (seen[i] == hash) return 0;
    }
    seen[i] = hash;
    seen_count++;
    return 1;
}

void TextureDump_Init(void)
{
    const char *path = getenv("MEMORIES_DUMP_TEXTURES");
    char name[1100];
    if (index_file || !path || !*path) return;
    snprintf(directory, sizeof(directory), "%s", path);
    make_directory(directory);
    snprintf(name, sizeof(name), "%s/textures.txt", directory);
    index_file = fopen(name, "a");
    if (!index_file) {
        fprintf(stderr, "memories-pc: cannot write textures to %s\n", directory);
        return;
    }
    TextureDump_Enabled = 1;
    fprintf(stderr, "memories-pc: dumping textures to %s\n", directory);
}

/* --- PNG, RGBA8 with stored (uncompressed) deflate blocks --------------- */

static uint32_t crc_table[256];

static void crc_init(void)
{
    uint32_t n, k, c;
    if (crc_table[1]) return;
    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc32_update(uint32_t crc, const unsigned char *data, size_t length)
{
    size_t i;
    crc ^= 0xffffffffu;
    for (i = 0; i < length; i++) crc = crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

static void put_be32(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static void write_chunk(FILE *file, const char *type, const unsigned char *data, size_t length)
{
    unsigned char header[8];
    uint32_t crc;
    put_be32(header, (uint32_t)length);
    memcpy(header + 4, type, 4);
    fwrite(header, 1, 8, file);
    if (length) fwrite(data, 1, length, file);
    crc = crc32_update(0, header + 4, 4);
    crc = crc32_update(crc, data, length);
    put_be32(header, crc);
    fwrite(header, 1, 4, file);
}

/* rows: h rows of 1 + w*4 bytes, each starting with filter byte 0. */
static int write_png(const char *path, int w, int h, const unsigned char *rows)
{
    static const unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    unsigned char ihdr[13];
    size_t raw = (size_t)h * (1 + (size_t)w * 4), blocks = (raw + 65534) / 65535, at = 0, i;
    size_t length = 2 + raw + blocks * 5 + 4;
    unsigned char *idat = malloc(length), *out;
    uint32_t a = 1, b = 0;
    FILE *file;
    if (!idat) return 0;
    out = idat;
    *out++ = 0x78; /* zlib: deflate, 32 KiB window */
    *out++ = 0x01;
    for (i = 0; i < blocks; i++) {
        size_t piece = raw - at < 65535 ? raw - at : 65535;
        *out++ = (unsigned char)(i + 1 == blocks);
        *out++ = (unsigned char)piece;
        *out++ = (unsigned char)(piece >> 8);
        *out++ = (unsigned char)~piece;
        *out++ = (unsigned char)(~piece >> 8);
        memcpy(out, rows + at, piece);
        out += piece;
        at += piece;
    }
    for (i = 0; i < raw; i++) {
        a = (a + rows[i]) % 65521;
        b = (b + a) % 65521;
    }
    put_be32(out, (b << 16) | a);
    out += 4;
    file = fopen(path, "wb");
    if (!file) {
        free(idat);
        return 0;
    }
    crc_init();
    fwrite(signature, 1, 8, file);
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* RGBA */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    write_chunk(file, "IHDR", ihdr, 13);
    write_chunk(file, "IDAT", idat, (size_t)(out - idat));
    write_chunk(file, "IEND", NULL, 0);
    fclose(file);
    free(idat);
    return 1;
}

/* --- the texture --------------------------------------------------------- */

static uint16_t word_at(const uint16_t *source, int x, int y)
{
    return source[(y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (x & (SOFT_GPU_WIDTH - 1))];
}

static uint64_t fnv(uint64_t hash, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t i;
    for (i = 0; i < length; i++) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

static void expand(unsigned char *out, uint16_t colour)
{
    if (colour == 0) {
        out[0] = out[1] = out[2] = out[3] = 0; /* the transparent colour */
        return;
    }
    out[0] = (unsigned char)(((colour & 0x1f) * 255 + 15) / 31);
    out[1] = (unsigned char)((((colour >> 5) & 0x1f) * 255 + 15) / 31);
    out[2] = (unsigned char)((((colour >> 10) & 0x1f) * 255 + 15) / 31);
    out[3] = 255;
}

void TextureDump_Primitive(const uint16_t *source, int page_x, int page_y, int depth, int clut_x, int clut_y,
                           int u0, int v0, int u1, int v1)
{
    int w, h, u, v, entries = depth == 0 ? 16 : depth == 1 ? 256 : 0;
    uint16_t *indices, palette[256];
    unsigned char *rows, header[8];
    uint64_t hash;
    char path[1100];
    if (!TextureDump_Enabled) return;
    if (u0 < 0) u0 = 0;
    if (v0 < 0) v0 = 0;
    if (u1 > 255) u1 = 255;
    if (v1 > 255) v1 = 255;
    if (u1 < u0 || v1 < v0) return;
    w = u1 - u0 + 1;
    h = v1 - v0 + 1;
    indices = malloc((size_t)w * h * sizeof(*indices));
    rows = malloc((size_t)h * (1 + (size_t)w * 4));
    if (!indices || !rows) {
        free(indices);
        free(rows);
        return;
    }
    for (v = 0; v < h; v++) {
        for (u = 0; u < w; u++) {
            int tu = u0 + u, ty = page_y + v0 + v;
            uint16_t word;
            if (depth == 0) {
                word = (word_at(source, page_x + tu / 4, ty) >> ((tu & 3) * 4)) & 0xf;
            } else if (depth == 1) {
                word = (word_at(source, page_x + tu / 2, ty) >> ((tu & 1) * 8)) & 0xff;
            } else {
                word = word_at(source, page_x + tu, ty);
            }
            indices[v * w + u] = word;
        }
    }
    for (u = 0; u < entries; u++) palette[u] = word_at(source, clut_x + u, clut_y);
    header[0] = (unsigned char)depth;
    header[1] = (unsigned char)w;
    header[2] = (unsigned char)h;
    header[3] = (unsigned char)(w >> 8);
    header[4] = (unsigned char)(h >> 8);
    header[5] = header[6] = header[7] = 0;
    hash = fnv(0xcbf29ce484222325ull, header, sizeof(header));
    hash = fnv(hash, indices, (size_t)w * h * sizeof(*indices));
    hash = fnv(hash, palette, (size_t)entries * sizeof(*palette));
    if (remember(hash)) {
        for (v = 0; v < h; v++) {
            unsigned char *row = rows + (size_t)v * (1 + (size_t)w * 4);
            *row++ = 0;
            for (u = 0; u < w; u++) {
                uint16_t index = indices[v * w + u];
                expand(row + u * 4, entries ? palette[index] : index);
            }
        }
        snprintf(path, sizeof(path), "%s/%016llx.png", directory, (unsigned long long)hash);
        if (write_png(path, w, h, rows)) {
            fprintf(index_file, "%016llx %dx%d depth=%d page=%d,%d uv=%d,%d clut=%d,%d\n",
                    (unsigned long long)hash, w, h, depth, page_x, page_y, u0, v0, clut_x, clut_y);
            fflush(index_file);
        }
    }
    free(indices);
    free(rows);
}
