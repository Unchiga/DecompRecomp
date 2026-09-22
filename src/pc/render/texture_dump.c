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
uint32_t *TextureDump_Tags;
uint16_t *TextureDump_Shadow;
void (*TextureDump_Paint)(int x, int y, int w, int h);
int (*TextureDump_Prepare)(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v);
static char directory[1024];
static FILE *index_file, *assets_file;
static int (*disc_file_info)(const char *path, int *lba, unsigned *size);

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
    snprintf(name, sizeof(name), "%s/assets.txt", directory);
    assets_file = fopen(name, "a");
    TextureDump_EnableTags();
    TextureDump_Enabled = 1;
    fprintf(stderr, "memories-pc: dumping textures to %s\n", directory);
}

/* --- provenance ---------------------------------------------------------- */

int TextureDump_EnableTags(void)
{
    if (!TextureDump_Tags) {
        TextureDump_Tags = calloc((size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT, sizeof(*TextureDump_Tags));
    }
    return TextureDump_Tags != NULL;
}

int TextureDump_EnableShadow(void)
{
    if (!TextureDump_EnableTags()) return 0;
    if (!TextureDump_Shadow) {
        TextureDump_Shadow = calloc((size_t)TEXTURE_SHADOW_WIDTH * SOFT_GPU_HEIGHT, sizeof(*TextureDump_Shadow));
    }
    return TextureDump_Shadow != NULL;
}

static uint64_t fnv(uint64_t hash, const void *data, size_t length);

typedef struct Delivery {
    uintptr_t destination;
    unsigned bytes;
    uint32_t disc_offset; /* byte offset on the disc of the first byte */
} Delivery;
#define DELIVERIES 4096
static Delivery deliveries[DELIVERIES];
static unsigned delivery_head;

void TextureDump_SetDiscFiles(int (*file_info)(const char *path, int *lba, unsigned *size))
{
    disc_file_info = file_info;
}

int TextureDump_DiscFile(const char *path, int *lba, unsigned *size)
{
    return disc_file_info ? disc_file_info(path, lba, size) : -2; /* -2: no disc yet */
}

void TextureDump_Delivered(const void *destination, unsigned bytes, int lba, unsigned offset_in_sector)
{
    Delivery *delivery;
    if (!TextureDump_Tags || lba < 0) return;
    delivery = &deliveries[delivery_head++ % DELIVERIES];
    delivery->destination = (uintptr_t)destination;
    delivery->bytes = bytes;
    delivery->disc_offset = (uint32_t)lba * 2048u + offset_in_sector;
}

/* Disc offset + 1 of the byte at address, 0 if no delivery covers it. */
static uint32_t provenance(uintptr_t address)
{
    static unsigned last;
    const Delivery *delivery = &deliveries[last];
    unsigned i;
    if (address >= delivery->destination && address < delivery->destination + delivery->bytes) {
        return delivery->disc_offset + (uint32_t)(address - delivery->destination) + 1;
    }
    for (i = 1; i <= DELIVERIES; i++) { /* newest first: a buffer is reused */
        unsigned at = (delivery_head + DELIVERIES - i) % DELIVERIES;
        delivery = &deliveries[at];
        if (delivery->bytes && address >= delivery->destination && address < delivery->destination + delivery->bytes) {
            last = at;
            return delivery->disc_offset + (uint32_t)(address - delivery->destination) + 1;
        }
    }
    return 0;
}

static uint32_t *tag_at(int x, int y)
{
    return &TextureDump_Tags[(y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (x & (SOFT_GPU_WIDTH - 1))];
}

/* Does any delivery overlap [first, last)? Uploads of pixels the game made
 * itself (a movie frame, a command buffer's data) would otherwise search the
 * whole ring once per word. */
static int delivered(uintptr_t first, uintptr_t last)
{
    unsigned i;
    for (i = 0; i < DELIVERIES; i++) {
        const Delivery *delivery = &deliveries[i];
        if (delivery->bytes && delivery->destination < last && delivery->destination + delivery->bytes > first) return 1;
    }
    return 0;
}

void TextureDump_Loaded(int x, int y, int w, int h, const uint16_t *pixels)
{
    int i, j;
    if (!TextureDump_Tags) return;
    if (!delivered((uintptr_t)pixels, (uintptr_t)(pixels + (size_t)w * h))) {
        TextureDump_Cleared(x, y, w, h);
        return;
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) *tag_at(x + i, y + j) = provenance((uintptr_t)&pixels[j * w + i]);
    }
    if (TextureDump_Shadow) {
        for (j = 0; j < h; j++) {
            for (i = 0; i < w; i++) memset(TextureDump_Cell(x + i, y + j, 0), 0, 4 * sizeof(uint16_t));
        }
        if (TextureDump_Paint) TextureDump_Paint(x, y, w, h);
    }
}

void TextureDump_Moved(int sx, int sy, int dx, int dy, int w, int h)
{
    int i, j;
    if (!TextureDump_Tags) return;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            *tag_at(dx + i, dy + j) = *tag_at(sx + i, sy + j);
            if (TextureDump_Shadow) {
                memcpy(TextureDump_Cell(dx + i, dy + j, 0), TextureDump_Cell(sx + i, sy + j, 0), 4 * sizeof(uint16_t));
            }
        }
    }
}

void TextureDump_Cleared(int x, int y, int w, int h)
{
    int i, j;
    if (!TextureDump_Tags) return;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            *tag_at(x + i, y + j) = 0;
            if (TextureDump_Shadow) memset(TextureDump_Cell(x + i, y + j, 0), 0, 4 * sizeof(uint16_t));
        }
    }
    /* The pack forgets which image was there (its own map of the words). */
    if (TextureDump_Shadow && TextureDump_Paint) TextureDump_Paint(x, y, w, h);
}

static void write_archives_once(void)
{
    static int done;
    static const char *const paths[] = {"\\DATA\\WA_MRG.MRG;1", "\\DATA\\SU.MRG;1", "\\DATA\\MODEL.MRG;1"};
    unsigned i;
    if (done) return;
    done = 1;
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int lba;
        unsigned size;
        if (disc_file_info && disc_file_info(paths[i], &lba, &size) == 0) {
            fprintf(assets_file, "archive %s lba=%d bytes=%u\n", paths[i] + 6, lba, size);
        }
    }
}

/* The asset a primitive draws, if its texels and palette are all from the
 * disc: offset of the first word, the upload's row stride, the rectangle,
 * depth and palette. Deduplicated. */
static void note_asset(int page_x, int page_y, int depth, int clut_x, int clut_y, int u0, int v0, int u1, int v1,
                       int entries, uint64_t image)
{
    int per_word = depth == 0 ? 4 : depth == 1 ? 2 : 1;
    int x = page_x + u0 / per_word, y = page_y + v0, words = u1 / per_word - u0 / per_word + 1, rows = v1 - v0 + 1;
    uint32_t first = *tag_at(x, y), palette = entries ? *tag_at(clut_x, clut_y) : 0, key[6], stride = 0;
    int32_t row_offsets[256];
    int j, linear = 1;
    if (!TextureDump_Tags || !assets_file || !first || (entries && !palette) || rows > 256) return;
    /* Each row must be one run of consecutive bytes, and so must the palette;
     * the rows themselves may lie anywhere (the sector streamer places 64x16
     * blocks in columns or side by side). */
    if (entries && *tag_at(clut_x + entries - 1, clut_y) != palette + (uint32_t)(entries - 1) * 2) return;
    for (j = 0; j < rows; j++) {
        uint32_t start = *tag_at(x, y + j);
        if (!start || *tag_at(x + words - 1, y + j) != start + (uint32_t)(words - 1) * 2) return;
        row_offsets[j] = (int32_t)(start - first);
        if (j == 1) stride = (uint32_t)row_offsets[1] / 2;
        if (j >= 1 && (row_offsets[j] != (int32_t)(stride * 2 * (uint32_t)j) || row_offsets[j] <= 0)) linear = 0;
    }
    if (rows == 1) stride = (uint32_t)words;
    key[0] = first - 1; key[1] = linear ? stride : 0; key[2] = (uint32_t)words; key[3] = (uint32_t)rows;
    key[4] = (uint32_t)depth; key[5] = entries ? palette - 1 : 0;
    if (!remember(fnv(fnv(0x9e3779b97f4a7c15ull, key, sizeof(key)), row_offsets, (size_t)rows * sizeof(row_offsets[0])))) {
        return;
    }
    write_archives_once();
    fprintf(assets_file, "asset offset=%u words=%d rows=%d bpp=%d clut=%u entries=%d png=%016llx px=%d,%d", key[0],
            words, rows, depth == 0 ? 4 : depth == 1 ? 8 : 16, key[5], entries, (unsigned long long)image,
            u0 % per_word, u1 - u0 + 1);
    if (linear) {
        fprintf(assets_file, " stride=%u\n", stride);
    } else {
        fprintf(assets_file, " rowofs=");
        for (j = 0; j < rows; j++) fprintf(assets_file, "%s%ld", j ? "," : "", (long)row_offsets[j]);
        fputc('\n', assets_file);
    }
    fflush(assets_file);
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
    if (source == SoftGpu_Vram()) note_asset(page_x, page_y, depth, clut_x, clut_y, u0, v0, u1, v1, entries, hash);
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
