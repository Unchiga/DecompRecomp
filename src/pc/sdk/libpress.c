/* LIBPRESS: PlayStation bitstream (v2) VLC decoding and a software MDEC.
 * DecDCTvlc2 produces genuine MDEC run-level words, DecDCTin selects that
 * buffer, and DecDCTout requests are decoded on the next interrupt tick,
 * each followed by the game's callback in interrupt context as on hardware.
 * The IDCT is floating point, so pixels can differ from the MDEC by a step. */
#include "types.h"
#include "pc/sdk/disc.h"
#include <math.h>
#include <string.h>
#include "pc/guest/state.h"

typedef struct Vlc { u16 code; u8 bits, run, level; } Vlc;

/* MPEG-1 table B.14 without the sign bit; EOB and escape are handled first. */
static const Vlc vlc_table[] = {
    {0x3, 2, 0, 1}, {0x3, 3, 1, 1}, {0x4, 4, 0, 2}, {0x5, 4, 2, 1}, {0x5, 5, 0, 3}, {0x7, 5, 3, 1},
    {0x6, 5, 4, 1}, {0x6, 6, 1, 2}, {0x7, 6, 5, 1}, {0x5, 6, 6, 1}, {0x4, 6, 7, 1}, {0x6, 7, 0, 4},
    {0x4, 7, 2, 2}, {0x7, 7, 8, 1}, {0x5, 7, 9, 1}, {0x26, 8, 0, 5}, {0x21, 8, 0, 6}, {0x25, 8, 1, 3},
    {0x24, 8, 3, 2}, {0x27, 8, 10, 1}, {0x23, 8, 11, 1}, {0x22, 8, 12, 1}, {0x20, 8, 13, 1},
    {0xa, 10, 0, 7}, {0xc, 10, 1, 4}, {0xb, 10, 2, 3}, {0xf, 10, 4, 2}, {0x9, 10, 5, 2},
    {0xe, 10, 14, 1}, {0xd, 10, 15, 1}, {0x8, 10, 16, 1},
    {0x1d, 12, 0, 8}, {0x18, 12, 0, 9}, {0x13, 12, 0, 10}, {0x10, 12, 0, 11}, {0x1b, 12, 1, 5},
    {0x14, 12, 2, 4}, {0x1c, 12, 3, 3}, {0x12, 12, 4, 3}, {0x1e, 12, 6, 2}, {0x15, 12, 7, 2},
    {0x11, 12, 8, 2}, {0x1f, 12, 17, 1}, {0x1a, 12, 18, 1}, {0x19, 12, 19, 1}, {0x17, 12, 20, 1},
    {0x16, 12, 21, 1},
    {0x1a, 13, 0, 12}, {0x19, 13, 0, 13}, {0x18, 13, 0, 14}, {0x17, 13, 0, 15}, {0x16, 13, 1, 6},
    {0x15, 13, 1, 7}, {0x14, 13, 2, 5}, {0x13, 13, 3, 4}, {0x12, 13, 5, 3}, {0x11, 13, 9, 2},
    {0x10, 13, 10, 2}, {0x1f, 13, 22, 1}, {0x1e, 13, 23, 1}, {0x1d, 13, 24, 1}, {0x1c, 13, 25, 1},
    {0x1b, 13, 26, 1},
    {0x1f, 14, 0, 16}, {0x1e, 14, 0, 17}, {0x1d, 14, 0, 18}, {0x1c, 14, 0, 19}, {0x1b, 14, 0, 20},
    {0x1a, 14, 0, 21}, {0x19, 14, 0, 22}, {0x18, 14, 0, 23}, {0x17, 14, 0, 24}, {0x16, 14, 0, 25},
    {0x15, 14, 0, 26}, {0x14, 14, 0, 27}, {0x13, 14, 0, 28}, {0x12, 14, 0, 29}, {0x11, 14, 0, 30},
    {0x10, 14, 0, 31},
    {0x18, 15, 0, 32}, {0x17, 15, 0, 33}, {0x16, 15, 0, 34}, {0x15, 15, 0, 35}, {0x14, 15, 0, 36},
    {0x13, 15, 0, 37}, {0x12, 15, 0, 38}, {0x11, 15, 0, 39}, {0x10, 15, 0, 40}, {0x1f, 15, 1, 8},
    {0x1e, 15, 1, 9}, {0x1d, 15, 1, 10}, {0x1c, 15, 1, 11}, {0x1b, 15, 1, 12}, {0x1a, 15, 1, 13},
    {0x19, 15, 1, 14},
    {0x13, 16, 1, 15}, {0x12, 16, 1, 16}, {0x11, 16, 1, 17}, {0x10, 16, 1, 18}, {0x14, 16, 6, 3},
    {0x1a, 16, 11, 2}, {0x19, 16, 12, 2}, {0x18, 16, 13, 2}, {0x17, 16, 14, 2}, {0x16, 16, 15, 2},
    {0x15, 16, 16, 2}, {0x1f, 16, 27, 1}, {0x1e, 16, 28, 1}, {0x1d, 16, 29, 1}, {0x1c, 16, 30, 1},
    {0x1b, 16, 31, 1}};

static const u8 zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const u8 quant[64] = {
    2, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

typedef struct Bits { const u16 *words; size_t at; } Bits;

static unsigned peek(const Bits *bits, unsigned count)
{
    size_t word = bits->at / 16;
    unsigned offset = (unsigned)(bits->at % 16);
    u64 window = ((u64)bits->words[word] << 32) | ((u64)bits->words[word + 1] << 16) | bits->words[word + 2];
    return (unsigned)((window >> (48 - offset - count)) & ((1u << count) - 1));
}

void DecDCTReset(int mode) { (void)mode; }
void DecDCTvlcBuild(u16 *table) { (void)table; }

/* Output: [0] = 0x3800_0000 | word count, then 16-bit codes packed in pairs:
 * per block (qscale << 10 | DC), (run << 10 | level)..., 0xFE00. */
int DecDCTvlc2(u32 *bitstream, u32 *out, void *table)
{
    const u16 *header = (const u16 *)bitstream;
    Bits bits = {header + 4, 0};
    unsigned qscale = header[2], produced = 0, i;
    u16 *codes = (u16 *)(out + 1);
    (void)table;
    /* A frame ends with the marker 0111111111 where a block would start. */
    for (;;) {
        unsigned dc;
        if (peek(&bits, 10) == 0x1ff) {
            break;
        }
        dc = peek(&bits, 10);
        bits.at += 10;
        codes[produced++] = (u16)((qscale << 10) | dc);
        for (;;) {
            unsigned window = peek(&bits, 17);
            if ((window >> 15) == 2) { /* '10' end of block */
                bits.at += 2;
                codes[produced++] = 0xfe00;
                break;
            }
            if ((window >> 11) == 1) { /* '000001' escape: 6-bit run, 10-bit level */
                bits.at += 6;
                codes[produced++] = (u16)peek(&bits, 16);
                bits.at += 16;
                continue;
            }
            for (i = 0; i < sizeof(vlc_table) / sizeof(vlc_table[0]); i++) {
                const Vlc *entry = &vlc_table[i];
                if ((window >> (17 - entry->bits)) == entry->code) {
                    int negative = (window >> (16 - entry->bits)) & 1;
                    int level = negative ? -(int)entry->level : entry->level;
                    codes[produced++] = (u16)((entry->run << 10) | (level & 0x3ff));
                    bits.at += entry->bits + 1u;
                    break;
                }
            }
            if (i == sizeof(vlc_table) / sizeof(vlc_table[0])) {
                goto done; /* corrupt stream: stop with what decoded cleanly */
            }
        }
        if (produced > 0x6f00) {
            break; /* the game's buffer is 0xE000 bytes */
        }
    }
done:
    if (produced & 1) {
        codes[produced++] = 0xfe00;
    }
    out[0] = 0x38000000u | (produced / 2);
    return 0;
}

static const u16 *input, *input_end;
static int depth24;
static u32 *output;
static size_t output_words;
static void (*out_callback)(void);
static float cosines[8][8];

void DecDCTin(u32 *buffer, int mode)
{
    input = (const u16 *)(buffer + 1);
    input_end = input + (buffer[0] & 0xffff) * 2;
    depth24 = mode & 1;
}

void DecDCTout(u32 *buffer, int size)
{
    output = buffer;
    output_words = (size_t)size;
}

int DecDCToutCallback(void (*callback)(void))
{
    out_callback = callback;
    return 0;
}

static int decode_block(float *block)
{
    int coefficients[64] = {0}, index = 0, qscale, value, x, y, u, v;
    float rows[64];
    while (input < input_end && *input == 0xfe00) {
        input++;
    }
    if (input >= input_end) {
        return 0;
    }
    qscale = *input >> 10;
    value = ((int)(*input++ << 22)) >> 22;
    coefficients[0] = value * quant[0];
    while (input < input_end) {
        unsigned code = *input++;
        if (code == 0xfe00) {
            break;
        }
        index += (int)(code >> 10) + 1;
        if (index > 63) {
            break;
        }
        value = ((int)(code << 22)) >> 22;
        value = qscale ? (value * quant[index] * qscale + 4) / 8 : value * 2;
        coefficients[zigzag[index]] = value < -0x400 ? -0x400 : value > 0x3ff ? 0x3ff : value;
    }
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            float sum = 0;
            for (u = 0; u < 8; u++) {
                sum += cosines[u][x] * (float)coefficients[y * 8 + u];
            }
            rows[y * 8 + x] = sum;
        }
    }
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            float sum = 0;
            for (v = 0; v < 8; v++) {
                sum += cosines[v][y] * rows[v * 8 + x];
            }
            block[y * 8 + x] = sum / 4.0f;
        }
    }
    return 1;
}

static int clamp255(float value) { return value < 0 ? 0 : value > 255 ? 255 : (int)(value + 0.5f); }

static void complete_request(void);

/* Interrupt context. The game's callback queues each frame's next strip with
 * DecDCTout, and the whole chain is completed in one call: the movie player
 * waits for the frame with a spin count (0x800000) that lasts seconds on the
 * console but milliseconds here, so a strip per tick let it give up and start
 * the next frame with the right-hand strips never written. */
void Memories_MdecService(void)
{
    int strips = 64; /* a 640-wide frame is 27 strips */
    while (output && input && strips--) {
        complete_request();
    }
}

static void complete_request(void)
{
    u8 *bytes;
    size_t produced = 0, wanted;
    void (*callback)(void);
    if (cosines[0][0] == 0) {
        int u, x;
        for (u = 0; u < 8; u++) {
            for (x = 0; x < 8; x++) {
                cosines[u][x] = (float)((u ? 1.0 : 0.70710678) * cos((2 * x + 1) * u * 3.14159265358979 / 16.0));
            }
        }
    }
    bytes = (u8 *)output;
    wanted = output_words * 4;
    while (produced < wanted) {
        float cr[64], cb[64], luma[4][64];
        size_t macroblock = depth24 ? 16 * 16 * 3 : 16 * 16 * 2;
        int x, y;
        if (!decode_block(cr) || !decode_block(cb) || !decode_block(luma[0]) || !decode_block(luma[1]) ||
            !decode_block(luma[2]) || !decode_block(luma[3])) {
            break;
        }
        for (y = 0; y < 16; y++) {
            for (x = 0; x < 16; x++) {
                const float *block = luma[(y / 8) * 2 + x / 8];
                float l = block[(y & 7) * 8 + (x & 7)] + 128.0f;
                float r_ = cr[(y / 2) * 8 + x / 2], b_ = cb[(y / 2) * 8 + x / 2];
                int r = clamp255(l + 1.402f * r_), g = clamp255(l - 0.3437f * b_ - 0.7143f * r_),
                    b = clamp255(l + 1.772f * b_);
                if (depth24) {
                    size_t at = produced + ((size_t)y * 16 + (size_t)x) * 3;
                    if (at + 3 <= wanted) { bytes[at] = (u8)r; bytes[at + 1] = (u8)g; bytes[at + 2] = (u8)b; }
                } else {
                    size_t at = produced + ((size_t)y * 16 + (size_t)x) * 2;
                    u16 pixel = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
                    if (at + 2 <= wanted) { memcpy(bytes + at, &pixel, 2); }
                }
            }
        }
        produced += macroblock;
    }
    output = NULL;
    callback = out_callback;
    if (callback) {
        callback();
    }
}

/* The decoder's buffers are the game's (guest addresses) and its callback is
 * a game function, so they hold across builds. */
void LibPress_State(MemoriesState *state)
{
    const MemoriesStateField fields[] = {{&input, sizeof(input)}, {&input_end, sizeof(input_end)},
                                         {&depth24, sizeof(depth24)}, {&output, sizeof(output)},
                                         {&output_words, sizeof(output_words)}, {&out_callback, sizeof(out_callback)}};
    Memories_StateChunk(state, "libpress", fields, sizeof(fields) / sizeof(fields[0]));
}
