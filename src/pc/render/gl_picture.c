/* The internal resolution drawn by OpenGL: see gl_picture.h.
 *
 * Recording. The software GPU hands over every GP0 batch and every transfer
 * made outside one (soft_gpu.h, SoftGpuRecorder), in order, and they are
 * appended to an arena as they come. A transfer can arrive from the
 * interrupt tick in the middle of a batch being appended, so an append
 * reserves its words with an atomic add and nothing ever moves the arena;
 * when it is full the frame is replayed from VRAM instead (a resync).
 *
 * Replay, at present, with the clock held. VRAM is a 16-bit integer texture
 * that the fragment shader decodes (4, 8 and 16 bits per texel through the
 * palette) exactly as the software GPU samples it; the picture is a colour
 * texture on a framebuffer, scale x scale pixels per word. Loads, fills and
 * copies are applied to both in order; primitives are drawn into the
 * picture only, and after the replay VRAM as the software GPU left it is
 * uploaded whole, so the next frame's textures are what the console's
 * would be (what a primitive draws is not sampled by a later primitive of
 * the same frame: that would be render-to-texture, which the game does not
 * do). Blending: a fragment's colour comes out pre-multiplied and its alpha
 * is the destination's factor, so opaque pixels and modes 0, 1 and 3 share
 * one draw; mode 2 (subtractive) draws its opaque texels first and then
 * its semi-transparent ones with a subtracting equation. No dithering, and
 * no mask checks: the game only ever sets the mask bit. The rules are the
 * software picture pass's (soft_gpu.c), which is this pass's oracle
 * (MEMORIES_GL_PICTURE=0 selects it on the same build). */
#include "pc/compat/fs.h"
#include "gl_picture.h"
#include "soft_gpu.h"
#include "texture_pack.h"
#include "pc/text/hd_text.h"
#include "pc/platform/settings.h"
#include "pc/compat/signal.h"
#include "pc/debug/log.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- the functions past OpenGL 1.1, from the driver ---------------------- */
#define GL_FUNCTIONS(X) \
    X(PFNGLCREATESHADERPROC, CreateShader) \
    X(PFNGLSHADERSOURCEPROC, ShaderSource) \
    X(PFNGLCOMPILESHADERPROC, CompileShader) \
    X(PFNGLGETSHADERIVPROC, GetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram) \
    X(PFNGLATTACHSHADERPROC, AttachShader) \
    X(PFNGLBINDATTRIBLOCATIONPROC, BindAttribLocation) \
    X(PFNGLLINKPROGRAMPROC, LinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog) \
    X(PFNGLDELETESHADERPROC, DeleteShader) \
    X(PFNGLUSEPROGRAMPROC, UseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, Uniform1i) \
    X(PFNGLUNIFORM2IPROC, Uniform2i) \
    X(PFNGLUNIFORM3IPROC, Uniform3i) \
    X(PFNGLUNIFORM4IPROC, Uniform4i) \
    X(PFNGLUNIFORM2FPROC, Uniform2f) \
    X(PFNGLGENBUFFERSPROC, GenBuffers) \
    X(PFNGLBINDBUFFERPROC, BindBuffer) \
    X(PFNGLBUFFERDATAPROC, BufferData) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray) \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, DisableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer) \
    X(PFNGLVERTEXATTRIBIPOINTERPROC, VertexAttribIPointer) \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers) \
    X(PFNGLBLITFRAMEBUFFERPROC, BlitFramebuffer) \
    X(PFNGLGENRENDERBUFFERSPROC, GenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC, BindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, RenderbufferStorageMultisample) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, FramebufferRenderbuffer) \
    X(PFNGLDELETERENDERBUFFERSPROC, DeleteRenderbuffers) \
    X(PFNGLBLENDEQUATIONPROC, BlendEquation) \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture) \
    X(PFNGLCLEARBUFFERUIVPROC, ClearBufferuiv) \
    X(PFNGLTEXIMAGE3DPROC, TexImage3D) \
    X(PFNGLTEXSUBIMAGE3DPROC, TexSubImage3D)

#define DECLARE(type, name) static type gl_##name;
GL_FUNCTIONS(DECLARE)
#undef DECLARE
static PFNGLGENVERTEXARRAYSPROC glGenVertexArrays_; /* optional: a core profile needs one bound */
static PFNGLBINDVERTEXARRAYPROC glBindVertexArray_;

static int load_functions(void)
{
#define LOAD(type, name) \
    gl_##name = (type)SDL_GL_GetProcAddress("gl" #name); \
    if (!gl_##name) { \
        fprintf(stderr, "memories-pc: OpenGL picture: no gl" #name "\n"); \
        return 0; \
    }
    GL_FUNCTIONS(LOAD)
#undef LOAD
    glGenVertexArrays_ = (PFNGLGENVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glGenVertexArrays");
    glBindVertexArray_ = (PFNGLBINDVERTEXARRAYPROC)SDL_GL_GetProcAddress("glBindVertexArray");
    return 1;
}

/* --- the record ---------------------------------------------------------- */
enum { OP_GP0 = 1, OP_LOAD, OP_MOVE, OP_FILL, OP_RESYNC, OP_PRECISE };
#define ARENA_WORDS (8u << 20) /* 32 MiB: a frame's list is at most 2 MiB */
static uint32_t *arena;
static volatile size_t arena_used;
static volatile int arena_overflow, want_resync; /* want_resync: a resync could not be recorded */
static int on; /* the pass is available and the recorder is set */

static uint32_t *reserve(size_t words)
{
    size_t at;
    if (!arena) return NULL;
    at = __sync_fetch_and_add(&arena_used, words);
    if (at + words > ARENA_WORDS) {
        arena_overflow = 1;
        return NULL;
    }
    return arena + at;
}

/* PGXP: the precise vertices of the batch that follows, as they are
 * (index, x, y, w: four words each). */
static void record_precise(const PgxpVertex *vertices, size_t count)
{
    uint32_t *at = reserve(2 + count * 4);
    if (!at) return;
    at[0] = OP_PRECISE;
    at[1] = (uint32_t)count;
    memcpy(at + 2, vertices, count * sizeof(PgxpVertex));
}

static void record_gp0(const uint32_t *words, size_t count)
{
    uint32_t *at = reserve(2 + count);
    if (!at) return;
    at[0] = OP_GP0;
    at[1] = (uint32_t)count;
    memcpy(at + 2, words, count * sizeof(uint32_t));
}

static void record_load(int x, int y, int w, int h, const uint16_t *pixels)
{
    size_t data = ((size_t)w * (size_t)h + 1) / 2;
    uint32_t *at = reserve(5 + data);
    if (!at) return;
    at[0] = OP_LOAD;
    at[1] = (uint32_t)x;
    at[2] = (uint32_t)y;
    at[3] = (uint32_t)w;
    at[4] = (uint32_t)h;
    memcpy(at + 5, pixels, (size_t)w * (size_t)h * 2);
}

static void record_move(int sx, int sy, int dx, int dy, int w, int h)
{
    uint32_t *at = reserve(7);
    if (!at) return;
    at[0] = OP_MOVE;
    at[1] = (uint32_t)sx;
    at[2] = (uint32_t)sy;
    at[3] = (uint32_t)dx;
    at[4] = (uint32_t)dy;
    at[5] = (uint32_t)w;
    at[6] = (uint32_t)h;
}

static void record_fill(int x, int y, int w, int h, uint32_t rgb24)
{
    uint32_t *at = reserve(6);
    if (!at) return;
    at[0] = OP_FILL;
    at[1] = (uint32_t)x;
    at[2] = (uint32_t)y;
    at[3] = (uint32_t)w;
    at[4] = (uint32_t)h;
    at[5] = rgb24;
}

static void record_resync(int scale, const uint32_t state[6])
{
    uint32_t *at = reserve(8);
    if (!at) {
        want_resync = 1;
        return;
    }
    at[0] = OP_RESYNC;
    at[1] = (uint32_t)scale;
    memcpy(at + 2, state, 6 * sizeof(uint32_t));
}

static const SoftGpuRecorder recorder = {record_gp0, record_load, record_move, record_fill, record_resync,
                                          record_precise};

/* --- GL objects ---------------------------------------------------------- */
static int scale;                 /* of the picture in the framebuffer, 0 before the first resync */
static GLuint vram_texture, vram_scratch, vram_fbo, vram_scratch_fbo;
static GLuint picture_texture, picture_scratch, picture_fbo, picture_scratch_fbo;
/* Anti-aliasing (Video > Anti-aliasing, `msaa`): with `samples` above 0 the
 * picture and widescreen's targets are drawn into multisampled renderbuffers
 * (these framebuffers), and resolved into their textures where something
 * reads them: a move's source, a target's centre, and the end of a replay
 * (presenting, frame dumps). Nothing can be copied into a multisampled
 * buffer, so what is copied in is drawn, as a quad. 0: the textures are
 * drawn into straight, as before. */
static int samples, samples_scale;
static GLuint picture_ms_fbo, picture_ms_buffer;
static GLuint program, buffer, vertex_array;
static GLint u_picture_size, u_window, u_op, u_pass, u_scale, u_copy_offset, u_vram, u_scratch, u_banks;
static GLint u_tex_xbr;
/* The texture banks (soft_gpu.h) a primitive can sample instead of VRAM:
 * layers 0..14 of an array texture for banks 1..15, each uploaded again when
 * a replay finds it changed since the copy kept here (a mod writes a bank
 * directly, so nothing announces the change). */
static GLuint banks_texture;
static uint16_t *bank_copy[SOFT_GPU_BANKS];
static int bank_used[SOFT_GPU_BANKS];
/* The texture pack (texture_pack.h): its maps as integer textures,
 * uploaded again when they change, and its images as textures, made as
 * primitives need them, dropped when the entries change. */
static GLuint entry_map_texture, place_map_texture, *entry_textures;
static int entry_texture_count;
static unsigned pack_generation = ~0u, map_generation = ~0u;
static GLint u_entry_map, u_place_map, u_pack, u_pack_entry, u_pack_size;
/* HD text (hd_text.h): whether it is on for this replay, and its atlas of
 * glyph pictures, 8-bit indices, as an integer texture on unit 6; and HD
 * numbers and labels, whose pictures are in the same atlas. */
static int hd_text, hd_hud, opponent_name;
static GLuint glyphs_texture;
static int glyphs_side;
static unsigned glyphs_generation = ~0u;
static GLint u_glyphs;

static const char *vertex_source =
    "#version 130\n"
    "uniform vec2 picture_size;\n"
    "in vec2 position;\n"
    "in vec2 texcoord;\n"
    "in vec4 colour;\n"
    "in ivec4 texture_page;\n"
    "in ivec4 texture_mode;\n"
    "in float persp;\n"
    "noperspective out float q;\n"
    "in ivec4 texture_bounds;\n"
    "noperspective out vec2 uv;\n"
    "noperspective out vec3 rgb;\n"
    "flat out int flags;\n"
    "flat out ivec4 page;\n"
    "flat out ivec4 mode;\n"
    "flat out ivec4 bounds;\n"
    "void main() {\n"
    "    gl_Position = vec4(position.x / picture_size.x * 2.0 - 1.0, position.y / picture_size.y * 2.0 - 1.0, 0.0, 1.0);\n"
    /* PGXP: a triangle with its depths (flag 32) interpolates uv / w and
     * 1 / w across the screen, and divides back, which is perspective. */
    "    uv = (int(colour.a) & 32) != 0 ? texcoord * persp : texcoord;\n"
    "    q = persp;\n"
    "    rgb = colour.rgb;\n"
    "    flags = int(colour.a);\n"
    "    page = texture_page;\n"
    "    mode = texture_mode;\n"
    "    bounds = texture_bounds;\n"
    "}\n";

/* Texture xBR (Video > Effects > xBR at 2x and up): present_pass.c's xBR,
 * level 2, on the texels of each textured primitive instead of the finished
 * picture, so a sprite's edges are smoothed at the internal resolution and
 * against what lies under it. The same rules and neighbourhood (see there),
 * with three changes. A texel is its word: transparent (0) is one colour,
 * as far from every other as black from white, so outlines against
 * transparency round too, and where the fill is transparent the pixel is
 * not drawn. Texels are those of the primitive's rectangle of texture
 * (bounds: first u, v, last u, v); past it the edge repeats, so a picture
 * put together from several rectangles shows no seams. And the colours
 * blend by coverage only between two opaque texels. centre_word and
 * near_word are the texel's word and its fill's. */
#define TEXTURE_XBR_SOURCE \
    "uint centre_word, near_word;\n" \
    "uint nb_word[25];\n" \
    "vec4 nb[25];\n" \
    "int nbi(int x, int y) { return (y + 2) * 5 + x + 2; }\n" \
    "float tdist(vec4 a, vec4 b) {\n" \
    "    if (a.a != b.a) return 1.0;\n" \
    "    vec3 k = a.rgb - b.rgb;\n" \
    "    vec3 yuv = abs(vec3(dot(k, vec3(0.299, 0.587, 0.114)), dot(k, vec3(-0.169, -0.331, 0.5)),\n" \
    "                        dot(k, vec3(0.5, -0.419, -0.081))));\n" \
    "    return dot(yuv, vec3(48.0, 7.0, 6.0)) / 48.0;\n" \
    "}\n" \
    "bool tsame(vec4 a, vec4 b) { return tdist(a, b) < 0.06; }\n" \
    "float tcover(float f, float slope, float w) { return clamp(f / (slope * w) + 0.5, 0.0, 1.0); }\n" \
    "vec2 tcorner(int dx, int dy, vec2 q, float w) {\n" \
    "    vec4 E = nb[12], B = nb[nbi(0, -dy)], C = nb[nbi(dx, -dy)], D = nb[nbi(-dx, 0)], F = nb[nbi(dx, 0)];\n" \
    "    vec4 G = nb[nbi(-dx, dy)], H = nb[nbi(0, dy)], I = nb[nbi(dx, dy)], F4 = nb[nbi(2 * dx, 0)];\n" \
    "    vec4 I4 = nb[nbi(2 * dx, dy)], H5 = nb[nbi(0, 2 * dy)], I5 = nb[nbi(dx, 2 * dy)];\n" \
    "    bool may = !tsame(E, F) && !tsame(E, H) &&\n" \
    "               (!tsame(F, B) && !tsame(H, D) || tsame(E, I) && !tsame(F, I4) && !tsame(H, I5) ||\n" \
    "                tsame(E, G) || tsame(E, C));\n" \
    "    float across = tdist(E, C) + tdist(E, G) + tdist(I, F4) + tdist(I, H5) + 4.0 * tdist(H, F);\n" \
    "    float along = tdist(H, D) + tdist(H, I5) + tdist(F, I4) + tdist(F, B) + 4.0 * tdist(E, I);\n" \
    "    if (!may || across >= along) return vec2(12.0, 0.0);\n" \
    "    float cut = tcover(q.x + q.y - 1.5, 1.4142, w);\n" \
    "    if (2.0 * tdist(F, G) <= tdist(H, C) && !tsame(E, G) && !tsame(D, G))\n" \
    "        cut = max(cut, tcover(0.5 * q.x + q.y - 1.0, 1.118, w));\n" \
    "    if (tdist(F, G) >= 2.0 * tdist(H, C) && !tsame(E, C) && !tsame(B, C))\n" \
    "        cut = max(cut, tcover(q.x + 0.5 * q.y - 1.0, 1.118, w));\n" \
    "    return vec2(float(tdist(E, F) <= tdist(E, H) ? nbi(dx, 0) : nbi(0, dy)), cut);\n" \
    "}\n" \
    "void fetch(ivec2 e, int x, int y, ivec2 lo, ivec2 hi) {\n" \
    "    ivec2 t = clamp(e + ivec2(x, y), lo, hi);\n" \
    "    uint word = texel_word(t.x, t.y);\n" \
    "    nb_word[nbi(x, y)] = word;\n" \
    "    nb[nbi(x, y)] = word == 0u ? vec4(0.0) : vec4(expand(word) / 255.0, 1.0);\n" \
    "}\n" \
    "vec4 texture_xbr(vec2 p, vec2 half_step, float w) {\n" \
    "    ivec2 e = ivec2(floor(p));\n" \
    "    vec2 q = clamp(p + half_step - vec2(e), 0.0, 1.0);\n" \
    "    ivec2 lo = min(bounds.xy, e), hi = max(bounds.zw, e);\n" \
    "    fetch(e, 0, 0, lo, hi);\n" \
    "    fetch(e, 1, 0, lo, hi);\n" \
    "    fetch(e, -1, 0, lo, hi);\n" \
    "    fetch(e, 0, 1, lo, hi);\n" \
    "    fetch(e, 0, -1, lo, hi);\n" \
    "    centre_word = near_word = nb_word[12];\n" \
    /* Like the four beside it (every corner's F and H): no corner is cut. */ \
    "    if (tsame(nb[12], nb[13]) && tsame(nb[12], nb[11]) && tsame(nb[12], nb[17]) && tsame(nb[12], nb[7]))\n" \
    "        return vec4(expand(centre_word), 0.0);\n" \
    "    for (int y = -2; y <= 2; y++) {\n" \
    "        for (int x = -2; x <= 2; x++) {\n" \
    "            if (((x == -2 || x == 2) && (y == -2 || y == 2)) || abs(x) + abs(y) <= 1) continue;\n" \
    "            fetch(e, x, y, lo, hi);\n" \
    "        }\n" \
    "    }\n" \
    "    vec2 best = tcorner(1, 1, q, w), k = tcorner(-1, 1, vec2(1.0 - q.x, q.y), w);\n" \
    "    if (k.y > best.y) best = k;\n" \
    "    k = tcorner(1, -1, vec2(q.x, 1.0 - q.y), w);\n" \
    "    if (k.y > best.y) best = k;\n" \
    "    k = tcorner(-1, -1, 1.0 - q, w);\n" \
    "    if (k.y > best.y) best = k;\n" \
    "    centre_word = nb_word[12];\n" \
    "    near_word = nb_word[int(best.x)];\n" \
    "    return vec4(expand(near_word), best.y);\n" \
    "}\n"

/* op 0: a primitive. flags: 1 raw texture, 2 semi-transparent, 4 textured.
 * page: page x, page y, palette x, palette y. mode: depth, blend mode, bank.
 * window: mask x, mask y, offset x, offset y in texels, as soft_gpu.c has
 * them. pass: 0 every fragment, 1 the opaque ones, 2 the semi-transparent.
 * op 1: VRAM into the picture. op 2: the scratch copy into the picture. */
static const char *fragment_source =
    "#version 130\n"
    "uniform usampler2D vram;\n"
    "uniform usampler2DArray banks;\n"
    "uniform sampler2D scratch;\n"
    "uniform usampler2D entry_map;\n"
    "uniform usampler2D place_map;\n"
    "uniform sampler2D pack;\n"
    "uniform usampler2D glyphs;\n"
    "uniform ivec4 pack_entry;\n" /* entry index + 1, crop left, crop width, rows */
    "uniform ivec3 pack_size;\n"  /* image width, height, texels per word */
    "uniform ivec4 window;\n"
    "uniform int op;\n"
    "uniform int pass;\n"
    "uniform int scale;\n"
    "uniform ivec2 copy_offset;\n"
    "uniform int tex_xbr;\n"
    "noperspective in vec2 uv;\n"
    "noperspective in float q;\n"
    "noperspective in vec3 rgb;\n"
    "flat in int flags;\n"
    "flat in ivec4 page;\n"
    "flat in ivec4 mode;\n"
    "flat in ivec4 bounds;\n"
    "out vec4 fragment;\n"
    "vec3 expand(uint word) {\n"
    "    uint r = word & 31u, g = (word >> 5) & 31u, b = (word >> 10) & 31u;\n"
    "    return vec3(float((r << 3) | (r >> 2)), float((g << 3) | (g >> 2)), float((b << 3) | (b >> 2)));\n"
    "}\n"
    "uint word_at(int x, int y) {\n"
    "    if (mode.z != 0) return texelFetch(banks, ivec3(x & 1023, y & 511, mode.z - 1), 0).r;\n"
    "    return texelFetch(vram, ivec2(x & 1023, y & 511), 0).r;\n"
    "}\n"
    /* Texel u,v (0 to 255) of the primitive's page through the window. */
    "uint texel_word(int u, int v) {\n"
    "    u = ((u & ~window.x) | window.z) & 255;\n"
    "    v = ((v & ~window.y) | window.w) & 255;\n"
    "    int y = page.y + v;\n"
    "    if (mode.x == 0) {\n"
    "        uint w = word_at(page.x + u / 4, y);\n"
    "        return word_at(page.z + int((w >> uint((u & 3) * 4)) & 15u), page.w);\n"
    "    }\n"
    "    if (mode.x == 1) {\n"
    "        uint w = word_at(page.x + u / 2, y);\n"
    "        return word_at(page.z + int((w >> uint((u & 1) * 8)) & 255u), page.w);\n"
    "    }\n"
    "    return word_at(page.x + u, y);\n"
    "}\n"
    TEXTURE_XBR_SOURCE
    "void main() {\n"
    "    if (op == 1) {\n"
    "        ivec2 at = ivec2(gl_FragCoord.xy) / scale;\n"
    "        fragment = vec4(expand(word_at(at.x, at.y)) / 255.0, 0.0);\n"
    "        return;\n"
    "    }\n"
    "    if (op == 2) {\n"
    "        fragment = vec4(texelFetch(scratch, ivec2(gl_FragCoord.xy) - copy_offset, 0).rgb, 0.0);\n"
    "        return;\n"
    "    }\n"
    "    vec3 c = floor(rgb + 1.0 / 256.0);\n"
    "    bool semi = (flags & 2) != 0;\n"
    /* PGXP's perspective triangles (flag 32) carry uv / w; st is the texel.
     * Taken before any discard: where in its texel the pixel's centre lies,
     * and how many texels a pixel spans (texture xBR). */
    "    vec2 st = (flags & 32) != 0 ? uv / q : uv;\n"
    "    vec2 half_step = 0.5 * (dFdx(st) + dFdy(st));\n"
    "    float spread = max(fwidth(st.x), fwidth(st.y));\n"
    "    if ((flags & 4) != 0) {\n"
    "        float ub = st.x + 1.0 / 256.0, vb = st.y + 1.0 / 256.0;\n"
    /* Only the primitive's own texels: a pixel drawn for a sample it
     * covers (anti-aliasing) can have its centre past the edge. */
    "        int u = clamp(int(floor(ub)), bounds.x, bounds.z) & 255, v = clamp(int(floor(vb)), bounds.y, bounds.w) & 255;\n"
    "        uint word;\n"
    "        int y;\n"
    "        vec3 t;\n"
    "        bool replaced = false;\n"
    "        if ((flags & 8) != 0) {\n"
    /* The pack's image, where it paints this texel (texture_pack.c, sample). */
    "            int per = pack_size.z;\n"
    "            int vx = (page.x + u / per) & 1023, vy = (page.y + v) & 511;\n"
    "            if (int(texelFetch(entry_map, ivec2(vx, vy), 0).r) == pack_entry.x) {\n"
    "                uint place = texelFetch(place_map, ivec2(vx, vy), 0).r;\n"
    "                int row = int(place >> 16), word_in = int(place & 0xffffu);\n"
    "                int texel_x = word_in * per + (u - (u / per) * per) - pack_entry.y;\n"
    "                if (texel_x >= 0 && texel_x < pack_entry.z) {\n"
    "                    int px = int(floor((float(texel_x) + fract(ub)) * float(pack_size.x) / float(pack_entry.z)));\n"
    "                    int py = int(floor((float(row) + fract(vb)) * float(pack_size.y) / float(pack_entry.w)));\n"
    "                    vec4 p = texelFetch(pack, ivec2(clamp(px, 0, pack_size.x - 1), clamp(py, 0, pack_size.y - 1)), 0);\n"
    "                    if (p.a < 0.5) discard;\n"
    "                    t = floor(p.rgb * 255.0 + 0.5);\n"
    "                    replaced = true;\n"
    "                }\n"
    "            }\n"
    "        }\n"
    /* The texel's own word: its colour unless replaced, and its
     * semi-transparency bit either way. */
    "        if ((flags & 16) != 0) {\n"
    /* HD text (hd_text.h): the index from the glyph's picture, through the
     * glyph's palette as ever. */
    "            ivec2 at = clamp(ivec2(floor(vec2(ub, vb) * float(scale))), ivec2(0), textureSize(glyphs, 0) - 1);\n"
    "            word = word_at(page.z + int(texelFetch(glyphs, at, 0).r), page.w);\n"
    "        } else if (tex_xbr != 0 && (flags & 8) == 0) {\n"
    "            vec4 k = texture_xbr(vec2(ub, vb), half_step, spread);\n"
    "            word = k.a > 0.5 ? near_word : centre_word;\n"
    "            if (word == 0u) discard;\n"
    "            t = centre_word != 0u && near_word != 0u ? mix(expand(centre_word), k.rgb, k.a) : expand(word);\n"
    "            replaced = true;\n"
    "        } else {\n"
    "            word = texel_word(u, v);\n"
    "        }\n"
    "        semi = semi && (word & 0x8000u) != 0u;\n"
    "        if (!replaced) {\n"
    "            if (word == 0u) discard;\n"
    "            t = expand(word);\n"
    "        }\n"
    "        if ((flags & 1) != 0) c = t;\n"
    "        else c = floor(t * c / 128.0);\n"
    "    }\n"
    "    if (pass == 1 && semi) discard;\n"
    "    if (pass == 2 && !semi) discard;\n"
    "    c = clamp(c, 0.0, 255.0);\n"
    "    float alpha = 0.0;\n"
    "    if (semi) {\n"
    "        if (mode.y == 0) { c *= 0.5; alpha = 0.5; }\n"
    "        else if (mode.y == 3) { c = floor(c * 0.25); alpha = 1.0; }\n"
    "        else alpha = 1.0;\n"
    "    }\n"
    "    fragment = vec4(c / 255.0, alpha);\n"
    "}\n";

static GLuint compile(GLenum kind, const char *source)
{
    GLuint shader = gl_CreateShader(kind);
    GLint ok = 0;
    gl_ShaderSource(shader, 1, &source, NULL);
    gl_CompileShader(shader);
    gl_GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl_GetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "memories-pc: OpenGL picture: shader: %s\n", log);
        gl_DeleteShader(shader);
        return 0;
    }
    return shader;
}

static int make_program(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vertex_source), fs;
    GLint ok = 0;
    if (!vs) return 0;
    fs = compile(GL_FRAGMENT_SHADER, fragment_source);
    if (!fs) {
        gl_DeleteShader(vs);
        return 0;
    }
    program = gl_CreateProgram();
    gl_AttachShader(program, vs);
    gl_AttachShader(program, fs);
    gl_BindAttribLocation(program, 0, "position");
    gl_BindAttribLocation(program, 1, "texcoord");
    gl_BindAttribLocation(program, 2, "colour");
    gl_BindAttribLocation(program, 3, "texture_page");
    gl_BindAttribLocation(program, 4, "texture_mode");
    gl_BindAttribLocation(program, 5, "persp");
    gl_BindAttribLocation(program, 6, "texture_bounds");
    gl_LinkProgram(program);
    gl_DeleteShader(vs);
    gl_DeleteShader(fs);
    gl_GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        gl_GetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "memories-pc: OpenGL picture: program: %s\n", log);
        return 0;
    }
    u_picture_size = gl_GetUniformLocation(program, "picture_size");
    u_window = gl_GetUniformLocation(program, "window");
    u_op = gl_GetUniformLocation(program, "op");
    u_pass = gl_GetUniformLocation(program, "pass");
    u_scale = gl_GetUniformLocation(program, "scale");
    u_copy_offset = gl_GetUniformLocation(program, "copy_offset");
    u_tex_xbr = gl_GetUniformLocation(program, "tex_xbr");
    u_vram = gl_GetUniformLocation(program, "vram");
    u_scratch = gl_GetUniformLocation(program, "scratch");
    u_banks = gl_GetUniformLocation(program, "banks");
    u_entry_map = gl_GetUniformLocation(program, "entry_map");
    u_place_map = gl_GetUniformLocation(program, "place_map");
    u_pack = gl_GetUniformLocation(program, "pack");
    u_glyphs = gl_GetUniformLocation(program, "glyphs");
    u_pack_entry = gl_GetUniformLocation(program, "pack_entry");
    u_pack_size = gl_GetUniformLocation(program, "pack_size");
    return 1;
}

static GLuint make_texture(GLenum internal, int w, int h, GLenum format, GLenum type)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, format, type, NULL);
    return texture;
}

static GLuint make_framebuffer(GLuint texture)
{
    GLuint fbo;
    gl_GenFramebuffers(1, &fbo);
    gl_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (gl_CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "memories-pc: OpenGL picture: framebuffer incomplete\n");
        gl_DeleteFramebuffers(1, &fbo);
        fbo = 0;
    }
    gl_BindFramebuffer(GL_FRAMEBUFFER, 0);
    return fbo;
}

static void wide_free(void); /* widescreen's targets (below) */

/* A multisampled framebuffer w x h pixels of `samples` samples; 0 if none. */
static GLuint make_multisampled(int w, int h, GLuint *buffer_out)
{
    GLuint fbo, buffer;
    gl_GenRenderbuffers(1, &buffer);
    gl_BindRenderbuffer(GL_RENDERBUFFER, buffer);
    while (glGetError() != GL_NO_ERROR) continue;
    gl_RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
    gl_BindRenderbuffer(GL_RENDERBUFFER, 0);
    if (glGetError() != GL_NO_ERROR) { /* out of video memory, say: none */
        fprintf(stderr, "memories-pc: OpenGL picture: no room for %dx anti-aliasing at this resolution\n", samples);
        gl_DeleteRenderbuffers(1, &buffer);
        return 0;
    }
    gl_GenFramebuffers(1, &fbo);
    gl_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, buffer);
    if (gl_CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "memories-pc: OpenGL picture: %dx anti-aliasing framebuffer incomplete\n", samples);
        gl_BindFramebuffer(GL_FRAMEBUFFER, 0);
        gl_DeleteFramebuffers(1, &fbo);
        gl_DeleteRenderbuffers(1, &buffer);
        return 0;
    }
    gl_BindFramebuffer(GL_FRAMEBUFFER, 0);
    *buffer_out = buffer;
    return fbo;
}

static void free_multisampled(GLuint *fbo, GLuint *buffer)
{
    if (*fbo) gl_DeleteFramebuffers(1, fbo);
    if (*buffer) gl_DeleteRenderbuffers(1, buffer);
    *fbo = *buffer = 0;
}

/* Pixels x,y,w,h of a multisampled framebuffer into its texture's. */
static void resolve(GLuint from, GLuint to, int x, int y, int w, int h)
{
    glDisable(GL_SCISSOR_TEST);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, from);
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER, to);
    gl_BlitFramebuffer(x, y, x + w, y + h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl_BindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Words x,y,w,h of the picture into its texture, before they are read. */
static void picture_resolve(int x, int y, int w, int h)
{
    if (picture_ms_fbo) resolve(picture_ms_fbo, picture_fbo, x * scale, y * scale, w * scale, h * scale);
}

/* Where the picture is drawn. */
static GLuint picture_draw(void)
{
    return picture_ms_fbo ? picture_ms_fbo : picture_fbo;
}

/* The picture and its scratch copy at the scale, made or remade. */
static int make_picture(int wanted)
{
    int w = SOFT_GPU_WIDTH * wanted, h = SOFT_GPU_HEIGHT * wanted;
    GLint largest = 0;
    if (wanted == scale) return 1;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &largest);
    if (w > largest || h > largest) {
        fprintf(stderr, "memories-pc: OpenGL picture: %dx is beyond the largest texture (%d)\n", wanted, largest);
        return 0;
    }
    wide_free(); /* their textures are at the old scale */
    free_multisampled(&picture_ms_fbo, &picture_ms_buffer);
    if (picture_fbo) gl_DeleteFramebuffers(1, &picture_fbo);
    if (picture_scratch_fbo) gl_DeleteFramebuffers(1, &picture_scratch_fbo);
    if (picture_texture) glDeleteTextures(1, &picture_texture);
    if (picture_scratch) glDeleteTextures(1, &picture_scratch);
    picture_texture = make_texture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
    picture_scratch = make_texture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
    picture_fbo = make_framebuffer(picture_texture);
    picture_scratch_fbo = make_framebuffer(picture_scratch);
    if (!picture_fbo || !picture_scratch_fbo) {
        scale = 0;
        return 0;
    }
    if (samples && !(picture_ms_fbo = make_multisampled(w, h, &picture_ms_buffer)))
        samples = 0; /* no room: drawn without, until the setting or the scale changes */
    scale = wanted;
    samples_scale = wanted; /* made at this scale: set_samples has nothing to redo */
    return 1;
}

int GlPicture_Init(void)
{
    const char *version = (const char *)glGetString(GL_VERSION), *choice = getenv("MEMORIES_GL_PICTURE");
    int major = 0;
    if (choice && !strcmp(choice, "0")) return 0;
    if (!version || sscanf(version, "%d", &major) != 1 || major < 3) {
        fprintf(stderr, "memories-pc: OpenGL picture: needs OpenGL 3.0, have %s\n", version ? version : "none");
        return 0;
    }
    if (!load_functions() || !make_program()) return 0;
    vram_texture = make_texture(GL_R16UI, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
    vram_scratch = make_texture(GL_R16UI, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
    vram_fbo = make_framebuffer(vram_texture);
    vram_scratch_fbo = make_framebuffer(vram_scratch);
    if (!vram_fbo || !vram_scratch_fbo) return 0;
    gl_GenBuffers(1, &buffer);
    if (glGenVertexArrays_ && glBindVertexArray_) glGenVertexArrays_(1, &vertex_array);
    arena = malloc(ARENA_WORDS * sizeof(uint32_t));
    if (!arena) return 0;
    glBindTexture(GL_TEXTURE_2D, 0);
    on = 1;
    SoftGpu_SetRecorder(&recorder); /* records the first resync */
    fprintf(stderr, "memories-pc: OpenGL picture pass on (%s)\n", version);
    return 1;
}

/* --- replay: the primitives ---------------------------------------------- */
typedef struct GlVertex {
    float x, y, u, v;
    uint8_t r, g, b, flags;
    uint16_t page_x, page_y, clut_x, clut_y;
    uint16_t depth, blend, bank, unused;
    float q; /* PGXP: 1 / depth, with flag 32; else 1 */
    uint16_t bounds[4]; /* the primitive's texels: first u, v, last u, v */
} GlVertex;

/* A run of vertices drawn under one scissor and texture window, all of one
 * blending equation. */
typedef struct Run {
    size_t first, count;
    int clip[4], window[4], subtractive, pack; /* pack: the entry (index + 1) sampled, 0 none */
    int wide; /* the widescreen target it is drawn into, -1 the picture */
} Run;
/* The widescreen target the primitive being read is drawn into, -1 the
 * picture (see "widescreen" below). */
static int wide_now = -1;

static GlVertex *vertices;
static size_t vertex_count, vertex_room;
static Run *runs;
static size_t run_count, run_room;

/* The drawing state, as soft_gpu.c keeps it, across batches and frames. */
static struct {
    int clip_x1, clip_y1, clip_x2, clip_y2;
    int offset_x, offset_y;
    int page_x, page_y, blend, depth, bank;
    int window_mask_x, window_mask_y, window_x, window_y;
    int clut_x, clut_y;
    int pack; /* the pack entry the primitive being read samples, 0 none */
    int glyph; /* the page word has HD text's mark (hd_text.h) */
} state;

typedef struct Vertex {
    int x, y, r, g, b, u, v;
    int precise;     /* PGXP: fx, fy and q hold where the vertex really is */
    float fx, fy, q;
} Vertex;

/* PGXP (pc/compat/pgxp.h): the batch being read, and the precise vertices
 * recorded for it, by word index, in order. */
static const uint32_t *batch_words;
static const PgxpVertex *batch_precise;
static size_t batch_precise_count;

/* The precise position of the vertex whose position word is `word`, if the
 * batch has one for it. */
static void find_precise(Vertex *vertex, const uint32_t *word)
{
    size_t low = 0, high = batch_precise_count;
    uint32_t index;
    vertex->precise = 0;
    if (!batch_precise_count || !batch_words || word < batch_words) return;
    index = (uint32_t)(word - batch_words);
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (batch_precise[middle].index < index) low = middle + 1;
        else high = middle;
    }
    if (low < batch_precise_count && batch_precise[low].index == index && batch_precise[low].w > 0) {
        vertex->precise = 1;
        vertex->fx = batch_precise[low].x + (float)state.offset_x;
        vertex->fy = batch_precise[low].y + (float)state.offset_y;
        vertex->q = 1.0f / batch_precise[low].w;
    }
}
/* The texels of the primitive being read (GlVertex's bounds). */
static int bounds_now[4];

/* A subtractive primitive is a run of its own: its two passes (opaque
 * texels, then the semi-transparent ones) must not straddle a later
 * primitive that overlaps it. */
static int run_matches(const Run *run, int subtractive)
{
    return !subtractive && run->clip[0] == state.clip_x1 && run->clip[1] == state.clip_y1 && run->clip[2] == state.clip_x2 &&
           run->clip[3] == state.clip_y2 && run->window[0] == state.window_mask_x &&
           run->window[1] == state.window_mask_y && run->window[2] == state.window_x &&
           run->window[3] == state.window_y && run->subtractive == subtractive && run->pack == state.pack &&
           run->wide == wide_now;
}

static GlVertex *push_vertices(size_t n, int subtractive)
{
    GlVertex *out;
    if (vertex_count + n > vertex_room) {
        size_t room = vertex_room ? vertex_room * 2 : 4096;
        GlVertex *more;
        while (room < vertex_count + n) room *= 2;
        more = realloc(vertices, room * sizeof(*vertices));
        if (!more) return NULL;
        vertices = more;
        vertex_room = room;
    }
    if (!run_count || !run_matches(&runs[run_count - 1], subtractive)) {
        Run *run;
        if (run_count == run_room) {
            size_t room = run_room ? run_room * 2 : 64;
            Run *more = realloc(runs, room * sizeof(*runs));
            if (!more) return NULL;
            runs = more;
            run_room = room;
        }
        run = &runs[run_count++];
        run->first = vertex_count;
        run->count = 0;
        run->clip[0] = state.clip_x1;
        run->clip[1] = state.clip_y1;
        run->clip[2] = state.clip_x2;
        run->clip[3] = state.clip_y2;
        run->window[0] = state.window_mask_x;
        run->window[1] = state.window_mask_y;
        run->window[2] = state.window_x;
        run->window[3] = state.window_y;
        run->subtractive = subtractive;
        run->pack = state.pack;
        run->wide = wide_now;
    }
    runs[run_count - 1].count += n;
    out = vertices + vertex_count;
    vertex_count += n;
    memset(out, 0, n * sizeof(*out));
    return out;
}

static void set_vertex(GlVertex *out, float x, float y, float u, float v, const Vertex *from, int flags)
{
    out->x = x;
    out->y = y;
    out->u = u;
    out->v = v;
    out->r = (uint8_t)from->r;
    out->g = (uint8_t)from->g;
    out->b = (uint8_t)from->b;
    out->flags = (uint8_t)(flags | (state.pack ? 8 : 0));
    out->page_x = (uint16_t)state.page_x;
    out->page_y = (uint16_t)state.page_y;
    out->clut_x = (uint16_t)state.clut_x;
    out->clut_y = (uint16_t)state.clut_y;
    out->depth = (uint16_t)state.depth;
    out->blend = (uint16_t)state.blend;
    out->bank = (uint16_t)state.bank;
    out->q = (flags & 32) ? from->q : 1.0f;
    out->bounds[0] = (uint16_t)bounds_now[0];
    out->bounds[1] = (uint16_t)bounds_now[1];
    out->bounds[2] = (uint16_t)bounds_now[2];
    out->bounds[3] = (uint16_t)bounds_now[3];
}

/* A triangle as the software pass rasterizes it: its edges are tested at
 * the picture pixels' integer corners, GL tests at their centres, so the
 * vertices move by half a pixel. Attributes move with them. */
static void triangle(const Vertex *a, const Vertex *b, const Vertex *c, int flags)
{
    const Vertex *v[3] = {a, b, c};
    int min_x, max_x, min_y, max_y, i;
    GlVertex *out;
    int64_t area = (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(b->y - a->y) * (c->x - a->x);
    if (area == 0) return;
    min_x = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    max_x = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    min_y = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    max_y = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);
    if (max_x - min_x > 1023 || max_y - min_y > 511) return;
    out = push_vertices(3, (flags & 2) && state.blend == 2);
    if (!out) return;
    /* PGXP: a textured polygon with all its depths is drawn in perspective
     * (flag 32, set by polygon()); a precise vertex stands where it really is. */
    if (!(a->precise && b->precise && c->precise)) flags &= ~32;
    for (i = 0; i < 3; i++) {
        float x = v[i]->precise ? v[i]->fx * (float)scale + 0.5f : (float)(v[i]->x * scale) + 0.5f;
        float y = v[i]->precise ? v[i]->fy * (float)scale + 0.5f : (float)(v[i]->y * scale) + 0.5f;
        set_vertex(&out[i], x, y, (float)v[i]->u, (float)v[i]->v, v[i], flags);
    }
}

/* Whether the first triangle's texels (u, or v with `axis` 1) grow across
 * the screen: rightwards, or downwards where they do not change across.
 * A flat or degenerate triangle counts as growing. */
static int texels_grow(const Vertex *v, int axis)
{
    long long dx1 = v[1].x - v[0].x, dy1 = v[1].y - v[0].y, dx2 = v[2].x - v[0].x, dy2 = v[2].y - v[0].y;
    long long dt1 = axis ? v[1].v - v[0].v : v[1].u - v[0].u, dt2 = axis ? v[2].v - v[0].v : v[2].u - v[0].u;
    long long area = dx1 * dy2 - dx2 * dy1;
    long long across = dt1 * dy2 - dt2 * dy1, down = dx1 * dt2 - dx2 * dt1; /* times the area */
    long long slope = across ? across : down;
    return !area || !slope || (slope > 0) == (area > 0);
}

/* A block of pixels x,y,w,h in picture units with the given corners' texels. */
/* A block of pixels x,y,w,h in picture units, texels u0,v0 to u1,v1 across
 * it. The software pass takes a rectangle's texel at each pixel's corner
 * (i / scale from the first), GL interpolates at the centre: the texels
 * move back by half a pixel so the two agree, which shows where a pack's
 * image is sampled between texels. */
static void block(int x, int y, int w, int h, int u0, int v0, int u1, int v1, const Vertex *colour, int flags)
{
    GlVertex *out = push_vertices(6, (flags & 2) && state.blend == 2);
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    float half = 0.5f / (float)scale;
    float s0 = (float)u0 - half, t0 = (float)v0 - half, s1 = (float)u1 - half, t1 = (float)v1 - half;
    if (!out) return;
    bounds_now[0] = u0;
    bounds_now[1] = v0;
    bounds_now[2] = u1 - 1;
    bounds_now[3] = v1 - 1;
    set_vertex(&out[0], x0, y0, s0, t0, colour, flags);
    set_vertex(&out[1], x1, y0, s1, t0, colour, flags);
    set_vertex(&out[2], x1, y1, s1, t1, colour, flags);
    set_vertex(&out[3], x0, y0, s0, t0, colour, flags);
    set_vertex(&out[4], x1, y1, s1, t1, colour, flags);
    set_vertex(&out[5], x0, y1, s0, t1, colour, flags);
}

static void set_colour(Vertex *vertex, uint32_t word)
{
    vertex->r = word & 0xff;
    vertex->g = (word >> 8) & 0xff;
    vertex->b = (word >> 16) & 0xff;
}

static void set_position(Vertex *vertex, uint32_t word)
{
    vertex->x = (((int32_t)(word << 21)) >> 21) + state.offset_x;
    vertex->y = (((int32_t)(word << 5)) >> 21) + state.offset_y;
}

static void set_page(uint32_t value)
{
    /* Bits 11-14 name a texture bank (soft_gpu.h); one never made is VRAM. */
    int bank = (int)((value >> 11) & (SOFT_GPU_BANKS - 1));
    state.bank = bank && SoftGpu_BankPixels(bank) ? bank : 0;
    if (state.bank) bank_used[state.bank] = 1;
    state.page_x = (value & 0xf) * 64;
    state.page_y = ((value >> 4) & 1) * 256;
    state.blend = (value >> 5) & 3;
    state.depth = (value >> 7) & 3;
    if (state.depth == 3) state.depth = 2;
    state.glyph = (value & HD_TEXT_MARK) != 0;
}

static size_t polygon(const uint32_t *words, size_t count)
{
    uint32_t command = words[0] >> 24;
    int quad = command & 8, textured = command & 4, shaded = command & 0x10;
    int vertices_n = quad ? 4 : 3, i;
    size_t need = (size_t)vertices_n * (1 + (textured ? 1 : 0)) + (shaded ? (size_t)vertices_n : 1);
    size_t at = 0;
    int flags = (command & 3) | (textured ? 4 : 0);
    Vertex v[4];
    if (count < need) return 0;
    memset(v, 0, sizeof(v));
    for (i = 0; i < vertices_n; i++) {
        if (i == 0 || shaded) {
            set_colour(&v[i], words[at++]);
        } else {
            v[i].r = v[0].r;
            v[i].g = v[0].g;
            v[i].b = v[0].b;
        }
        set_position(&v[i], words[at]);
        find_precise(&v[i], words + at);
        at++;
        if (textured) {
            uint32_t word = words[at++];
            v[i].u = word & 0xff;
            v[i].v = (word >> 8) & 0xff;
            if (i == 0) {
                state.clut_x = ((word >> 16) & 0x3f) * 16;
                state.clut_y = (word >> 22) & 0x1ff;
            } else if (i == 1) {
                set_page(word >> 16);
            }
        }
    }
    state.pack = textured && !state.bank
                     ? TexturePack_EntryFor(state.page_x, state.page_y, state.depth, state.clut_x, state.clut_y,
                                            v[0].u, v[0].v)
                     : 0;
    if (quad && textured && hd_hud && !state.pack && !state.bank) {
        /* A digit or the panel drawn as a quad (a clip-tested field card):
         * a texture pack's image, where one paints it, comes first. */
        int u0 = v[0].u, v0 = v[0].v, u1 = v[0].u, v1 = v[0].v, atlas_u, atlas_v;
        for (i = 1; i < 4; i++) {
            if (v[i].u < u0) u0 = v[i].u;
            if (v[i].v < v0) v0 = v[i].v;
            if (v[i].u > u1) u1 = v[i].u;
            if (v[i].v > v1) v1 = v[i].v;
        }
        if (HdText_Hud(state.depth, state.page_x, state.page_y, state.clut_x, state.clut_y, u0, v0, u1 - u0, v1 - v0,
                       scale, &atlas_u, &atlas_v)) {
            for (i = 0; i < 4; i++) {
                v[i].u = atlas_u + v[i].u - u0;
                v[i].v = atlas_v + v[i].v - v0;
            }
            flags |= 16;
        }
    }
    if (!(flags & 16) && quad && textured && state.glyph && hd_text && state.depth == 0) {
        /* A turned or leaning glyph: 16 texels across for the large font. */
        int atlas_u, atlas_v, u0 = v[0].u, v0 = v[0].v;
        if (HdText_Cell(state.bank, state.page_x, state.page_y, v[1].u - u0 > 8, u0, v0, scale, &atlas_u, &atlas_v)) {
            for (i = 0; i < 4; i++) {
                v[i].u = atlas_u + v[i].u - u0;
                v[i].v = atlas_v + v[i].v - v0;
            }
            state.pack = 0;
            flags |= 16;
        }
    } else if (!(flags & 16) && textured && hd_text && state.depth == 0 && !state.bank) {
        /* A card's title plate, whole or a piece of the turning card. */
        int atlas_u, atlas_v, title_u, title_v;
        if (HdText_Title(state.page_x, state.page_y, v[0].u, v[0].v, scale, &atlas_u, &atlas_v, &title_u, &title_v)) {
            for (i = 0; i < vertices_n; i++) {
                v[i].u = atlas_u + v[i].u - title_u;
                v[i].v = atlas_v + v[i].v - title_v;
            }
            state.pack = 0;
            flags |= 16;
        }
    }
    bounds_now[0] = bounds_now[2] = v[0].u;
    bounds_now[1] = bounds_now[3] = v[0].v;
    for (i = 1; i < vertices_n; i++) {
        if (v[i].u < bounds_now[0]) bounds_now[0] = v[i].u;
        if (v[i].v < bounds_now[1]) bounds_now[1] = v[i].v;
        if (v[i].u > bounds_now[2]) bounds_now[2] = v[i].u;
        if (v[i].v > bounds_now[3]) bounds_now[3] = v[i].v;
    }
    /* The rasterizer stops short of the right and bottom edges (as block()
     * does), so where the texels grow towards them the last one is never
     * drawn: past it, an atlas holds the next picture. A mirrored sprite
     * starts from that texel and draws it. */
    if (bounds_now[2] > bounds_now[0] && texels_grow(v, 0)) bounds_now[2]--;
    if (bounds_now[3] > bounds_now[1] && texels_grow(v, 1)) bounds_now[3]--;
    /* Both halves of a quad or neither, or its diagonal would show. */
    if (textured && v[0].precise && v[1].precise && v[2].precise && (!quad || v[3].precise)) flags |= 32;
    triangle(&v[0], &v[1], &v[2], flags);
    if (quad) triangle(&v[1], &v[2], &v[3], flags);
    return need;
}

/* The opponent's name over the life-point panel just drawn, and You for
 * YOU (hd_text.h): in the panel's colour, drawn from the atlas whatever
 * drew the panel. */
static void name_over_panel(const Vertex *base, int w, int h, int flags)
{
    int atlas_u, atlas_v, x, y, width, height, which;
    if (!opponent_name || state.bank || state.depth != 0 || state.page_x != 704 || state.page_y != 0 ||
        (state.clut_x != 736 && state.clut_x != 752) || state.clut_y != 252 || base->u != 128 || base->v != 128 || w != 64 || h != 40) {
        return;
    }
    for (which = 0; which < 2; which++) {
        if (!HdText_NameBox(scale, which, &atlas_u, &atlas_v, &x, &y, &width, &height)) return;
        state.pack = 0;
        block((base->x + x) * scale, (base->y + y) * scale, width * scale, height * scale, atlas_u, atlas_v,
              atlas_u + width, atlas_v + height, base, flags | 16);
    }
}

static size_t rectangle(const uint32_t *words, size_t count)
{
    static const int sizes[4] = {0, 1, 8, 16};
    uint32_t command = words[0] >> 24;
    int textured = command & 4, kind = (command >> 3) & 3, w, h;
    size_t need = 2 + (textured ? 1u : 0u) + (kind == 0 ? 1u : 0u), at = 2;
    int flags = (command & 3) | (textured ? 4 : 0);
    Vertex base;
    if (count < need) return 0;
    memset(&base, 0, sizeof(base));
    set_colour(&base, words[0]);
    set_position(&base, words[1]);
    if (textured) {
        base.u = words[at] & 0xff;
        base.v = (words[at] >> 8) & 0xff;
        state.clut_x = ((words[at] >> 16) & 0x3f) * 16;
        state.clut_y = (words[at] >> 22) & 0x1ff;
        at++;
    }
    w = h = sizes[kind];
    if (kind == 0) {
        w = words[at] & 0x3ff;
        h = (words[at] >> 16) & 0x1ff;
    }
    state.pack = textured && !state.bank
                     ? TexturePack_EntryFor(state.page_x, state.page_y, state.depth, state.clut_x, state.clut_y,
                                            base.u, base.v)
                     : 0;
    if (w && h && textured && hd_hud && !state.pack && !state.bank) {
        /* HD numbers and labels (a texture pack's image comes first). */
        int atlas_u, atlas_v;
        if (HdText_Hud(state.depth, state.page_x, state.page_y, state.clut_x, state.clut_y, base.u, base.v, w, h, scale,
                       &atlas_u, &atlas_v)) {
            block(base.x * scale, base.y * scale, w * scale, h * scale, atlas_u, atlas_v, atlas_u + w, atlas_v + h,
                  &base, flags | 16);
            name_over_panel(&base, w, h, flags);
            return need;
        }
    }
    if (w && h && textured && state.glyph && hd_text && state.depth == 0 && (w == 16 ? h == 16 : w == 8 && h == 12)) {
        int atlas_u, atlas_v;
        if (HdText_Cell(state.bank, state.page_x, state.page_y, w == 16, base.u, base.v, scale, &atlas_u, &atlas_v)) {
            state.pack = 0;
            block(base.x * scale, base.y * scale, w * scale, h * scale, atlas_u, atlas_v, atlas_u + w, atlas_v + h,
                  &base, flags | 16);
            return need;
        }
    }
    if (w && h && textured && hd_text && state.depth == 0 && !state.bank) {
        int atlas_u, atlas_v, title_u, title_v;
        if (HdText_Title(state.page_x, state.page_y, base.u, base.v, scale, &atlas_u, &atlas_v, &title_u, &title_v)) {
            state.pack = 0;
            block(base.x * scale, base.y * scale, w * scale, h * scale, atlas_u + base.u - title_u,
                  atlas_v + base.v - title_v, atlas_u + base.u - title_u + w, atlas_v + base.v - title_v + h, &base,
                  flags | 16);
            return need;
        }
    }
    if (w && h) {
        block(base.x * scale, base.y * scale, w * scale, h * scale, base.u, base.v, base.u + w, base.v + h, &base,
              flags);
        if (textured) name_over_panel(&base, w, h, flags);
    }
    return need;
}

/* A line in the picture: the console's one-pixel line made scale times
 * thicker, as a quad in words ([0] and [1] one end, [2] and [3] the other,
 * in a polygon's order) that covers each picture pixel once, so a
 * semi-transparent line blends as often as on the console at any scale.
 * Along the major axis it runs from the first word's leading edge to the
 * last word's trailing edge. */
static void line_quad(const Vertex *a, const Vertex *b, Vertex quad[4])
{
    int dx = b->x - a->x, dy = b->y - a->y;
    int x_major = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
    const Vertex *first = (x_major ? dx : dy) < 0 ? b : a, *last = first == a ? b : a;
    quad[0] = quad[1] = *first;
    quad[2] = quad[3] = *last;
    if (x_major) {
        quad[1].y++;
        quad[2].x++;
        quad[3].x++;
        quad[3].y++;
    } else {
        quad[1].x++;
        quad[2].y++;
        quad[3].x++;
        quad[3].y++;
    }
}

/* A line as the software pass draws it (soft_gpu.c, line_quad). */
static void line(const Vertex *a, const Vertex *b, int flags)
{
    int dx = b->x - a->x, dy = b->y - a->y;
    Vertex quad[4];
    if ((dx < 0 ? -dx : dx) > 1023 || (dy < 0 ? -dy : dy) > 511) return;
    state.pack = 0;
    line_quad(a, b, quad);
    triangle(&quad[0], &quad[1], &quad[2], flags);
    triangle(&quad[1], &quad[2], &quad[3], flags);
}

static size_t lines(const uint32_t *words, size_t count)
{
    uint32_t command = words[0] >> 24;
    int shaded = command & 0x10, poly = command & 8, flags = command & 2;
    size_t at = 0;
    Vertex previous, next;
    memset(&previous, 0, sizeof(previous));
    if (count < (shaded ? 4u : 3u)) return 0;
    set_colour(&previous, words[at++]);
    set_position(&previous, words[at++]);
    for (;;) {
        if (poly && at < count && (words[at] & 0xf000f000u) == 0x50005000u) return at + 1;
        next = previous;
        if (shaded) {
            if (at >= count) return 0;
            set_colour(&next, words[at++]);
        }
        if (at >= count) return 0;
        set_position(&next, words[at++]);
        line(&previous, &next, flags);
        previous = next;
        if (!poly) return at;
        if (at >= count) return 0;
    }
}

static void flush_runs(void);
static void apply_load(int x, int y, int w, int h, const uint16_t *pixels);
static void apply_move(int sx, int sy, int dx, int dy, int w, int h);
static void apply_fill(int x, int y, int w, int h, uint32_t rgb24);

/* --- replay: widescreen -------------------------------------------------- */

/* Widescreen's targets (soft_gpu.c), drawn here instead of by the software
 * GPU. A full-screen drawing area gets, by the software GPU's rule
 * (SoftGpu_WideMargin), a picture of its own `margin` words wider on each
 * side; every primitive drawn to the area is drawn again into it, shifted
 * right by the margin, with the clip the software GPU gives it there, and
 * transfers into the area are copied into its centre. Up to WIDE_TARGETS,
 * the least recently used one made anew first, as there. A target's texture
 * holds the widened area only: (w + 2 margin) x h words at the scale, row 0
 * the area's top. */
#define WIDE_TARGETS 4
typedef struct GlWide {
    int x1, y1, x2, y2, margin;
    int drawn;         /* primitives since it was last shown */
    unsigned stamp;    /* last use */
    int width, height; /* the texture's pixels */
    GLuint texture, fbo; /* fbo 0: a free slot */
    GLuint ms_fbo, ms_buffer; /* drawn into, with anti-aliasing */
} GlWide;
static GlWide wide[WIDE_TARGETS];
static unsigned wide_clock;

static void wide_free(void)
{
    int t;
    for (t = 0; t < WIDE_TARGETS; t++) {
        if (wide[t].fbo) gl_DeleteFramebuffers(1, &wide[t].fbo);
        if (wide[t].texture) glDeleteTextures(1, &wide[t].texture);
        free_multisampled(&wide[t].ms_fbo, &wide[t].ms_buffer);
        memset(&wide[t], 0, sizeof(wide[t]));
    }
}

static void copy_quad_into(GLuint fbo, int origin_x, int origin_y, int op, int x, int y, int w, int h);

/* Picture words x,y,w,h (inside the target's area) into its centre. */
static void wide_copy(const GlWide *wt, int x, int y, int w, int h)
{
    int to_x = x - wt->x1 + wt->margin, to_y = y - wt->y1;
    picture_resolve(x, y, w, h);
    if (wt->ms_fbo) {
        /* Drawn: the picture's texture as the scratch copy, read where the
         * target's pixel came from. */
        gl_ActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, picture_texture);
        gl_ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, vram_texture);
        gl_UseProgram(program);
        gl_Uniform2i(u_copy_offset, (wt->margin - wt->x1) * scale, -wt->y1 * scale);
        copy_quad_into(wt->ms_fbo, wt->x1 * scale, wt->y1 * scale, 2, (x + wt->margin) * scale, y * scale, w * scale,
                       h * scale);
        gl_ActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, picture_scratch);
        gl_ActiveTexture(GL_TEXTURE0);
        return;
    }
    glDisable(GL_SCISSOR_TEST);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, picture_fbo);
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER, wt->fbo);
    gl_BlitFramebuffer(x * scale, y * scale, (x + w) * scale, (y + h) * scale, to_x * scale, to_y * scale,
                       (to_x + w) * scale, (to_y + h) * scale, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl_BindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* The target's sides in rows y to y + h, in a colour. */
static void wide_sides(const GlWide *wt, int y, int h, float r, float g, float b)
{
    gl_BindFramebuffer(GL_FRAMEBUFFER, wt->ms_fbo ? wt->ms_fbo : wt->fbo);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, 0.0f);
    glScissor(0, (y - wt->y1) * scale, wt->margin * scale, h * scale);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor((wt->x2 - wt->x1 + 1 + wt->margin) * scale, (y - wt->y1) * scale, wt->margin * scale, h * scale);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    gl_BindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* A transfer into words x,y,w,h, now in the picture, into the centre of
 * every target it overlaps; a fill across a target's whole width fills its
 * sides too (soft_gpu.c, wide_mirror). */
static void wide_mirror(int x, int y, int w, int h, int fill, float r, float g, float b)
{
    int t;
    for (t = 0; t < WIDE_TARGETS; t++) {
        const GlWide *wt = &wide[t];
        int x1 = x > wt->x1 ? x : wt->x1, x2 = x + w - 1 < wt->x2 ? x + w - 1 : wt->x2;
        int y1 = y > wt->y1 ? y : wt->y1, y2 = y + h - 1 < wt->y2 ? y + h - 1 : wt->y2;
        if (!wt->fbo || x1 > x2 || y1 > y2) continue;
        wide_copy(wt, x1, y1, x2 - x1 + 1, y2 - y1 + 1);
        if (fill && x <= wt->x1 && x + w - 1 >= wt->x2) wide_sides(wt, y1, y2 - y1 + 1, r, g, b);
    }
}

/* The target for the current drawing area, made on first use from what the
 * picture holds, its sides black; -1 when it gets none. */
static int wide_target(void)
{
    int margin = SoftGpu_WideMargin(state.clip_x1, state.clip_y1, state.clip_x2, state.clip_y2);
    int t, oldest = 0, width, height;
    GlWide *wt;
    if (!margin) return -1;
    for (t = 0; t < WIDE_TARGETS; t++) {
        wt = &wide[t];
        if (wt->fbo && wt->x1 == state.clip_x1 && wt->y1 == state.clip_y1 && wt->x2 == state.clip_x2 &&
            wt->y2 == state.clip_y2) {
            wt->stamp = ++wide_clock;
            return t;
        }
        if (wide[t].stamp < wide[oldest].stamp) oldest = t;
    }
    flush_runs(); /* what is drawn so far, into the picture it starts from */
    wt = &wide[oldest];
    width = (state.clip_x2 - state.clip_x1 + 1 + 2 * margin) * scale;
    height = (state.clip_y2 - state.clip_y1 + 1) * scale;
    if (!wt->fbo || wt->width != width || wt->height != height) {
        free_multisampled(&wt->ms_fbo, &wt->ms_buffer); /* at the old size */
        if (wt->fbo) gl_DeleteFramebuffers(1, &wt->fbo);
        if (wt->texture) glDeleteTextures(1, &wt->texture);
        wt->texture = make_texture(GL_RGBA8, width, height, GL_RGBA, GL_UNSIGNED_BYTE);
        wt->fbo = make_framebuffer(wt->texture);
        if (!wt->fbo) {
            glDeleteTextures(1, &wt->texture);
            memset(wt, 0, sizeof(*wt));
            return -1;
        }
        wt->width = width;
        wt->height = height;
    }
    if (samples && !wt->ms_fbo) wt->ms_fbo = make_multisampled(width, height, &wt->ms_buffer);
    if (samples && !wt->ms_fbo) samples = 0; /* no room: the picture's is dropped too, below */
    if (!samples && picture_ms_fbo) {
        flush_runs();
        picture_resolve(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
        free_multisampled(&picture_ms_fbo, &picture_ms_buffer);
    }
    wt->x1 = state.clip_x1;
    wt->y1 = state.clip_y1;
    wt->x2 = state.clip_x2;
    wt->y2 = state.clip_y2;
    wt->margin = margin;
    wt->drawn = 0;
    wt->stamp = ++wide_clock;
    gl_BindFramebuffer(GL_FRAMEBUFFER, wt->ms_fbo ? wt->ms_fbo : wt->fbo);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    wide_copy(wt, wt->x1, wt->y1, wt->x2 - wt->x1 + 1, wt->y2 - wt->y1 + 1);
    return oldest;
}

/* The batch, as SoftGpu_Gp0 reads it: the same commands, the same cuts. */
static void gp0(const uint32_t *words, size_t count)
{
    size_t at = 0;
    while (at < count) {
        uint32_t word = words[at], command = word >> 24;
        size_t used = 1;
        if (command >= 0x20 && command < 0x80) {
            size_t (*draw)(const uint32_t *, size_t) = command < 0x40 ? polygon : command < 0x60 ? lines : rectangle;
            int t;
            used = draw(words + at, count - at);
            if (used && scale >= 2 && (t = wide_target()) >= 0) {
                /* Again into the widescreen target, shifted, clipped as
                 * SoftGpu_Gp0 clips it there: polygons and lines past the
                 * 4:3 edges, sprites to them. */
                int clip_x1 = state.clip_x1, clip_x2 = state.clip_x2, offset_x = state.offset_x;
                int margin = wide[t].margin;
                if (command < 0x60) {
                    state.clip_x2 += 2 * margin;
                } else {
                    state.clip_x1 += margin;
                    state.clip_x2 += margin;
                }
                state.offset_x += margin;
                wide_now = t;
                draw(words + at, count - at);
                wide_now = -1;
                state.clip_x1 = clip_x1;
                state.clip_x2 = clip_x2;
                state.offset_x = offset_x;
                wide[t].drawn++;
            }
        } else if (command == 0x02) {
            used = count - at >= 3 ? 3 : 0;
            if (used) {
                apply_fill(words[at + 1] & 0x3f0, (words[at + 1] >> 16) & 0x1ff, ((words[at + 2] & 0x3ff) + 15) & ~15,
                           (words[at + 2] >> 16) & 0x1ff, word);
            }
        } else if (command >= 0x80 && command < 0xa0) {
            used = count - at >= 4 ? 4 : 0;
            if (used) {
                apply_move(words[at + 1] & 0x3ff, (words[at + 1] >> 16) & 0x1ff, words[at + 2] & 0x3ff,
                           (words[at + 2] >> 16) & 0x1ff, ((words[at + 3] - 1) & 0x3ff) + 1,
                           (((words[at + 3] >> 16) - 1) & 0x1ff) + 1);
            }
        } else if (command >= 0xa0 && command < 0xc0) {
            if (count - at < 3) {
                used = 0;
            } else {
                int w = (int)((words[at + 2] - 1) & 0x3ff) + 1;
                int h = (int)(((words[at + 2] >> 16) - 1) & 0x1ff) + 1;
                size_t data = ((size_t)w * (size_t)h + 1) / 2;
                used = count - at >= 3 + data ? 3 + data : 0;
                if (used) {
                    apply_load(words[at + 1] & 0x3ff, (words[at + 1] >> 16) & 0x1ff, w, h,
                               (const uint16_t *)(words + at + 3));
                }
            }
        } else if (command >= 0xc0 && command < 0xe0) {
            used = count - at >= 3 ? 3 : 0;
        } else if (command == 0xe1) {
            set_page(word);
        } else if (command == 0xe2) {
            state.window_mask_x = word & 0x1f;
            state.window_mask_y = (word >> 5) & 0x1f;
            state.window_x = (word >> 10) & 0x1f;
            state.window_y = (word >> 15) & 0x1f;
        } else if (command == 0xe3) {
            state.clip_x1 = word & 0x3ff;
            state.clip_y1 = (word >> 10) & 0x3ff;
        } else if (command == 0xe4) {
            state.clip_x2 = word & 0x3ff;
            state.clip_y2 = (word >> 10) & 0x3ff;
        } else if (command == 0xe5) {
            state.offset_x = ((int32_t)(word << 21)) >> 21;
            state.offset_y = ((int32_t)(word << 10)) >> 21;
        }
        if (!used) break;
        at += used;
    }
}

/* --- replay: GL ---------------------------------------------------------- */
static void bind_attributes(void)
{
    const GlVertex *base = NULL;
    gl_BindBuffer(GL_ARRAY_BUFFER, buffer);
    gl_EnableVertexAttribArray(0);
    gl_EnableVertexAttribArray(1);
    gl_EnableVertexAttribArray(2);
    gl_EnableVertexAttribArray(3);
    gl_EnableVertexAttribArray(4);
    gl_EnableVertexAttribArray(5);
    gl_EnableVertexAttribArray(6);
    gl_VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GlVertex), &base->x);
    gl_VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GlVertex), &base->u);
    gl_VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GlVertex), &base->r);
    gl_VertexAttribIPointer(3, 4, GL_UNSIGNED_SHORT, sizeof(GlVertex), &base->page_x);
    gl_VertexAttribIPointer(4, 4, GL_UNSIGNED_SHORT, sizeof(GlVertex), &base->depth);
    gl_VertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(GlVertex), &base->q);
    gl_VertexAttribIPointer(6, 4, GL_UNSIGNED_SHORT, sizeof(GlVertex), &base->bounds[0]);
}

/* The banks this flush samples, uploaded where they changed. */
static void sync_banks(void)
{
    int bank;
    for (bank = 1; bank < SOFT_GPU_BANKS; bank++) {
        const uint16_t *pixels;
        if (!bank_used[bank]) continue;
        bank_used[bank] = 0;
        pixels = SoftGpu_BankPixels(bank);
        if (!pixels) continue;
        if (!banks_texture) {
            glGenTextures(1, &banks_texture);
            gl_ActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D_ARRAY, banks_texture);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl_TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R16UI, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT, SOFT_GPU_BANKS - 1, 0,
                          GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
            gl_ActiveTexture(GL_TEXTURE0);
        }
        if (!bank_copy[bank]) bank_copy[bank] = malloc(sizeof(uint16_t) * SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT);
        if (bank_copy[bank] && !memcmp(bank_copy[bank], pixels, sizeof(uint16_t) * SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT)) {
            continue;
        }
        if (bank_copy[bank]) memcpy(bank_copy[bank], pixels, sizeof(uint16_t) * SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT);
        gl_ActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, banks_texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        gl_TexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, bank - 1, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT, 1, GL_RED_INTEGER,
                         GL_UNSIGNED_SHORT, pixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl_ActiveTexture(GL_TEXTURE0);
    }
}

static void unbind_attributes(void)
{
    int i;
    for (i = 0; i < 5; i++) gl_DisableVertexAttribArray(i);
    gl_DisableVertexAttribArray(6);
    gl_BindBuffer(GL_ARRAY_BUFFER, 0);
}

/* Words x1..x2, y1..y2 of the picture, or of a widescreen target, whose
 * texture starts at word origin_x, origin_y. */
static void scissor_words(int x1, int y1, int x2, int y2, int origin_x, int origin_y)
{
    int w, h;
    x1 -= origin_x;
    x2 -= origin_x;
    y1 -= origin_y;
    y2 -= origin_y;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    w = x2 - x1 + 1;
    h = y2 - y1 + 1;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    glScissor(x1 * scale, y1 * scale, w * scale, h * scale);
}

static GLuint make_map_texture(GLenum internal, GLenum type)
{
    return make_texture(internal, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT, GL_RED_INTEGER, type);
}

/* The pack's maps, uploaded again when they changed since; its image
 * textures are dropped when the entries changed, to be made again as
 * needed. */
/* HD text's atlas: made again when it is (another factor), else the rows
 * that changed. */
static void sync_glyphs(void)
{
    int side, first, last;
    unsigned generation;
    const uint8_t *atlas = HdText_Atlas(&side, &first, &last, &generation);
    if (!atlas || !side) return;
    gl_ActiveTexture(GL_TEXTURE6);
    if (!glyphs_texture) glGenTextures(1, &glyphs_texture);
    glBindTexture(GL_TEXTURE_2D, glyphs_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (generation != glyphs_generation || side != glyphs_side) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, side, side, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, atlas);
        glyphs_generation = generation;
        glyphs_side = side;
    } else if (last >= first) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, first, side, last - first + 1, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                        atlas + (size_t)first * side);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl_ActiveTexture(GL_TEXTURE0);
}

static void sync_pack(void)
{
    unsigned generation = TexturePack_Generation(), maps = TexturePack_MapGeneration();
    const uint16_t *entry_map = TexturePack_EntryMap();
    const uint32_t *place_map = TexturePack_PlaceMap();
    int i;
    if (!entry_map || !place_map) return;
    if (generation != pack_generation) {
        pack_generation = generation;
        for (i = 0; i < entry_texture_count; i++) {
            if (entry_textures[i]) glDeleteTextures(1, &entry_textures[i]);
        }
        free(entry_textures);
        entry_textures = NULL;
        entry_texture_count = 0;
        map_generation = maps - 1; /* the maps with them */
    }
    if (maps == map_generation) return;
    map_generation = maps;
    if (!entry_map_texture) entry_map_texture = make_map_texture(GL_R16UI, GL_UNSIGNED_SHORT);
    if (!place_map_texture) place_map_texture = make_map_texture(GL_R32UI, GL_UNSIGNED_INT);
    gl_ActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, entry_map_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_SHORT,
                    entry_map);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl_ActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, place_map_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_INT,
                    place_map);
    gl_ActiveTexture(GL_TEXTURE0);
}

/* An image side as the texture has it: the driver's largest at most. */
static int fitted(int side)
{
    static GLint largest;
    if (!largest) glGetIntegerv(GL_MAX_TEXTURE_SIZE, &largest);
    return largest > 0 && side > largest ? largest : side;
}

/* A pack image larger than the driver's textures, box-filtered down to
 * fit, as the pack itself averages an image onto the texels (texture_pack.c,
 * load_pixels): a texture that big would not be made and would sample
 * black. The shader finds its texels by proportion, so only the sizes it
 * is told change. NULL when out of memory. */
static unsigned char *shrunk(const unsigned char *rgba, int width, int height, int to_width, int to_height)
{
    unsigned char *out = malloc((size_t)to_width * to_height * 4);
    int x, y;
    if (!out) return NULL;
    for (y = 0; y < to_height; y++) {
        int y0 = (int)((long long)y * height / to_height), y1 = (int)((long long)(y + 1) * height / to_height);
        for (x = 0; x < to_width; x++) {
            int x0 = (int)((long long)x * width / to_width), x1 = (int)((long long)(x + 1) * width / to_width);
            unsigned long sum[4] = {0, 0, 0, 0}, n = 0;
            int sx, sy, k;
            for (sy = y0; sy < y1; sy++) {
                for (sx = x0; sx < x1; sx++, n++) {
                    for (k = 0; k < 4; k++) sum[k] += rgba[((size_t)sy * width + sx) * 4 + k];
                }
            }
            for (k = 0; k < 4; k++) out[((size_t)y * to_width + x) * 4 + k] = (unsigned char)(n ? sum[k] / n : 0);
        }
    }
    return out;
}

/* The entry's image on texture unit 5 and its measures in the uniforms;
 * 0 when the image is not there (the run then samples VRAM). */
static int bind_pack_entry(int entry)
{
    const unsigned char *rgba;
    int width, height, crop_left, crop_width, rows, per, texture_width, texture_height;
    if (!TexturePack_EntryImage(entry, &rgba, &width, &height, &crop_left, &crop_width, &rows, &per)) return 0;
    texture_width = fitted(width);
    texture_height = fitted(height);
    if (entry > entry_texture_count) {
        GLuint *more = realloc(entry_textures, (size_t)entry * sizeof(*entry_textures));
        if (!more) return 0;
        memset(more + entry_texture_count, 0, (size_t)(entry - entry_texture_count) * sizeof(*more));
        entry_textures = more;
        entry_texture_count = entry;
    }
    gl_ActiveTexture(GL_TEXTURE5);
    if (!entry_textures[entry - 1]) {
        unsigned char *smaller = NULL;
        if (texture_width != width || texture_height != height) {
            fprintf(stderr, "memories-pc: OpenGL picture: a %dx%d pack image is beyond the largest texture; drawn at %dx%d\n",
                    width, height, texture_width, texture_height);
            smaller = shrunk(rgba, width, height, texture_width, texture_height);
            if (!smaller) {
                gl_ActiveTexture(GL_TEXTURE0);
                return 0;
            }
        }
        entry_textures[entry - 1] = make_texture(GL_RGBA8, texture_width, texture_height, GL_RGBA, GL_UNSIGNED_BYTE);
        glBindTexture(GL_TEXTURE_2D, entry_textures[entry - 1]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture_width, texture_height, GL_RGBA, GL_UNSIGNED_BYTE,
                        smaller ? smaller : rgba);
        free(smaller);
    } else {
        glBindTexture(GL_TEXTURE_2D, entry_textures[entry - 1]);
    }
    gl_ActiveTexture(GL_TEXTURE0);
    /* The maps name the head of the entry's readings; this entry's image. */
    gl_Uniform4i(u_pack_entry, TexturePack_EntryHead(entry), crop_left, crop_width, rows);
    gl_Uniform3i(u_pack_size, texture_width, texture_height, per);
    return 1;
}

/* The primitives gathered so far, in order, into the picture. */
static void flush_runs(void)
{
    size_t i;
    int origin_x = 0, origin_y = 0;
    if (!vertex_count || scale < 2) {
        vertex_count = 0;
        run_count = 0;
        return;
    }
    sync_banks();
    sync_pack();
    if (hd_text || hd_hud || opponent_name) sync_glyphs();
    gl_ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vram_texture);
    gl_UseProgram(program);
    gl_Uniform1i(u_op, 0);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    bind_attributes();
    gl_BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertex_count * sizeof(GlVertex)), vertices, GL_STREAM_DRAW);
    for (i = 0; i < run_count; i++) {
        const Run *run = &runs[i];
        if (i == 0 || run->wide != runs[i - 1].wide) {
            /* The picture, or a widescreen target: the whole picture's
             * coordinates, moved so the target's area lands on its texture. */
            if (run->wide < 0) {
                gl_BindFramebuffer(GL_FRAMEBUFFER, picture_draw());
                glViewport(0, 0, SOFT_GPU_WIDTH * scale, SOFT_GPU_HEIGHT * scale);
                origin_x = origin_y = 0;
            } else {
                gl_BindFramebuffer(GL_FRAMEBUFFER, wide[run->wide].ms_fbo ? wide[run->wide].ms_fbo : wide[run->wide].fbo);
                origin_x = wide[run->wide].x1;
                origin_y = wide[run->wide].y1;
                glViewport(-origin_x * scale, -origin_y * scale, SOFT_GPU_WIDTH * scale, SOFT_GPU_HEIGHT * scale);
            }
        }
        scissor_words(run->clip[0], run->clip[1], run->clip[2], run->clip[3], origin_x, origin_y);
        gl_Uniform4i(u_window, run->window[0] * 8, run->window[1] * 8, (run->window[2] & run->window[0]) * 8,
                    (run->window[3] & run->window[1]) * 8);
        if (!run->pack || !bind_pack_entry(run->pack)) gl_Uniform4i(u_pack_entry, 0, 0, 0, 0);
        if (!run->subtractive) {
            gl_Uniform1i(u_pass, 0);
            glDrawArrays(GL_TRIANGLES, (GLint)run->first, (GLsizei)run->count);
        } else {
            /* Its opaque texels, then its semi-transparent ones subtracted. */
            gl_Uniform1i(u_pass, 1);
            glDrawArrays(GL_TRIANGLES, (GLint)run->first, (GLsizei)run->count);
            gl_Uniform1i(u_pass, 2);
            gl_BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            glBlendFunc(GL_ONE, GL_ONE);
            glDrawArrays(GL_TRIANGLES, (GLint)run->first, (GLsizei)run->count);
            gl_BlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_ONE, GL_SRC_ALPHA);
        }
    }
    unbind_attributes();
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    vertex_count = 0;
    run_count = 0;
}

/* A quad over picture pixels x,y,w,h drawn with the program's op 1 or 2
 * into a framebuffer whose pixel 0,0 is picture pixel origin_x, origin_y. */
static void copy_quad_into(GLuint fbo, int origin_x, int origin_y, int op, int x, int y, int w, int h)
{
    GlVertex quad[6];
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    memset(quad, 0, sizeof(quad));
    quad[0].x = x0; quad[0].y = y0;
    quad[1].x = x1; quad[1].y = y0;
    quad[2].x = x1; quad[2].y = y1;
    quad[3].x = x0; quad[3].y = y0;
    quad[4].x = x1; quad[4].y = y1;
    quad[5].x = x0; quad[5].y = y1;
    gl_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(-origin_x, -origin_y, SOFT_GPU_WIDTH * scale, SOFT_GPU_HEIGHT * scale);
    gl_UseProgram(program);
    gl_Uniform1i(u_op, op);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    bind_attributes();
    gl_BufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    unbind_attributes();
}

/* The same into the picture. */
static void copy_quad(int op, int x, int y, int w, int h)
{
    copy_quad_into(picture_draw(), 0, 0, op, x, y, w, h);
}

/* Words x,y,w,h of VRAM into the picture (VRAM's texture already holds them). */
static void picture_from_words(int x, int y, int w, int h)
{
    gl_ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vram_texture);
    copy_quad(1, x * scale, y * scale, w * scale, h * scale);
}

static void clip_rect(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > SOFT_GPU_WIDTH) *w = SOFT_GPU_WIDTH - *x;
    if (*y + *h > SOFT_GPU_HEIGHT) *h = SOFT_GPU_HEIGHT - *y;
}

static void apply_load(int x, int y, int w, int h, const uint16_t *pixels)
{
    int cw = w, ch = h;
    flush_runs();
    if (scale < 2) return;
    x &= SOFT_GPU_WIDTH - 1;
    y &= SOFT_GPU_HEIGHT - 1;
    clip_rect(&x, &y, &cw, &ch); /* an upload past the edge wraps on the console; here it is cut */
    if (cw <= 0 || ch <= 0) return;
    glBindTexture(GL_TEXTURE_2D, vram_texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, cw, ch, GL_RED_INTEGER, GL_UNSIGNED_SHORT, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    picture_from_words(x, y, cw, ch);
    wide_mirror(x, y, cw, ch, 0, 0, 0, 0);
}

static void apply_fill(int x, int y, int w, int h, uint32_t rgb24)
{
    /* The colour VRAM gets: 15 bits, expanded as the picture expands them. */
    uint32_t r = (rgb24 >> 3) & 0x1f, g = (rgb24 >> 11) & 0x1f, b = (rgb24 >> 19) & 0x1f;
    uint16_t word = (uint16_t)(r | (g << 5) | (b << 10));
    flush_runs();
    if (scale < 2) return;
    x &= SOFT_GPU_WIDTH - 1;
    y &= SOFT_GPU_HEIGHT - 1;
    clip_rect(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    gl_BindFramebuffer(GL_FRAMEBUFFER, vram_fbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    {
        GLuint value[4] = {word, 0, 0, 0};
        gl_ClearBufferuiv(GL_COLOR, 0, value);
    }
    gl_BindFramebuffer(GL_FRAMEBUFFER, picture_draw());
    glScissor(x * scale, y * scale, w * scale, h * scale);
    glClearColor((float)((r << 3) | (r >> 2)) / 255.0f, (float)((g << 3) | (g >> 2)) / 255.0f,
                 (float)((b << 3) | (b >> 2)) / 255.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    wide_mirror(x, y, w, h, 1, (float)((r << 3) | (r >> 2)) / 255.0f, (float)((g << 3) | (g >> 2)) / 255.0f,
                (float)((b << 3) | (b >> 2)) / 255.0f);
}

/* Through the scratch copies, as a texture cannot be copied onto itself. */
static void apply_move(int sx, int sy, int dx, int dy, int w, int h)
{
    int sw = w, sh = h, dw = w, dh = h;
    flush_runs();
    if (scale < 2) return;
    sx &= SOFT_GPU_WIDTH - 1;
    sy &= SOFT_GPU_HEIGHT - 1;
    dx &= SOFT_GPU_WIDTH - 1;
    dy &= SOFT_GPU_HEIGHT - 1;
    clip_rect(&sx, &sy, &sw, &sh);
    clip_rect(&dx, &dy, &dw, &dh);
    w = sw < dw ? sw : dw;
    h = sh < dh ? sh : dh;
    if (w <= 0 || h <= 0) return;
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, vram_fbo);
    glBindTexture(GL_TEXTURE_2D, vram_scratch);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sx, sy, w, h);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, vram_scratch_fbo);
    glBindTexture(GL_TEXTURE_2D, vram_texture);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, dx, dy, 0, 0, w, h);
    picture_resolve(sx, sy, w, h);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, picture_fbo);
    glBindTexture(GL_TEXTURE_2D, picture_scratch);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sx * scale, sy * scale, w * scale, h * scale);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, vram_texture); /* unit 0 samples VRAM again */
    gl_UseProgram(program);
    gl_Uniform2i(u_copy_offset, dx * scale, dy * scale);
    copy_quad(2, dx * scale, dy * scale, w * scale, h * scale);
    wide_mirror(dx, dy, w, h, 0, 0, 0, 0);
}

/* VRAM as the software GPU has it, whole, into its texture. */
static void upload_vram(void)
{
    glBindTexture(GL_TEXTURE_2D, vram_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_SHORT,
                    SoftGpu_Vram());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

/* The state words, then VRAM whole into its texture and the picture. At a
 * scale of 1 there is no picture: the state words still count. */
static void resync(int wanted, const uint32_t words[6])
{
    vertex_count = 0;
    run_count = 0;
    if (wanted < 2) {
        wide_free();
        scale = 0;
        gp0(words, 6);
        return;
    }
    /* The targets are drawn again from the next primitive on (the picture
     * alone is rebuilt from VRAM): their sides are black until then. */
    wide_free();
    if (!make_picture(wanted)) return;
    gl_Uniform1i(u_scale, scale);
    gl_Uniform2f(u_picture_size, (float)(SOFT_GPU_WIDTH * scale), (float)(SOFT_GPU_HEIGHT * scale));
    gp0(words, 6);
    upload_vram();
    picture_from_words(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
}

/* The anti-aliasing wanted (0, 2, 4 or 8 samples, as the driver allows):
 * made or dropped between replays. The picture carries on from its texture;
 * the widescreen targets are made again from the next primitive. */
static void set_samples(int wanted)
{
    static int asked = -1, clamped_to = -1;
    GLint most = 0;
    int given;
    wanted = wanted >= 8 ? 8 : wanted >= 4 ? 4 : wanted >= 2 ? 2 : 0;
    if (wanted == asked && scale == samples_scale) return; /* as last time: made, or not to be had */
    asked = wanted;
    samples_scale = scale;
    glGetIntegerv(GL_MAX_SAMPLES, &most);
    given = wanted;
    while (given > most) given /= 2;
    if (given < 2) given = 0;
    if (given != wanted && given != clamped_to) {
        fprintf(stderr, "memories-pc: OpenGL picture: %dx anti-aliasing asked, the driver gives %dx\n", wanted, given);
        clamped_to = given;
    }
    wide_free();
    free_multisampled(&picture_ms_fbo, &picture_ms_buffer);
    samples = given;
    if (!samples || scale < 2) return;
    picture_ms_fbo = make_multisampled(SOFT_GPU_WIDTH * scale, SOFT_GPU_HEIGHT * scale, &picture_ms_buffer);
    if (!picture_ms_fbo) {
        samples = 0; /* no room: drawn without, until the setting or the scale changes */
        return;
    }
    /* What the picture shows now, drawn in. */
    gl_ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, picture_texture);
    gl_ActiveTexture(GL_TEXTURE0);
    gl_UseProgram(program);
    gl_Uniform2i(u_copy_offset, 0, 0);
    copy_quad(2, 0, 0, SOFT_GPU_WIDTH * scale, SOFT_GPU_HEIGHT * scale);
    gl_ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, picture_scratch);
    gl_ActiveTexture(GL_TEXTURE0);
}

int GlPicture_Replay(void)
{
    static uint32_t *taken;
    sigset_t held, previous;
    size_t count, at;
    int overflow, wanted_resync;
    struct timespec t0;
    if (!on) return 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* Take the record: what a tick appends from now on is the next one's. */
    sigemptyset(&held);
    sigaddset(&held, SIGALRM);
    sigprocmask(SIG_BLOCK, &held, &previous);
    count = arena_used;
    if (count > ARENA_WORDS) count = ARENA_WORDS;
    overflow = arena_overflow;
    wanted_resync = want_resync;
    if (!taken) taken = malloc(ARENA_WORDS * sizeof(uint32_t));
    if (taken) memcpy(taken, arena, count * sizeof(uint32_t));
    arena_used = 0;
    arena_overflow = 0;
    want_resync = 0;
    sigprocmask(SIG_SETMASK, &previous, NULL);
    if (!taken) return 0;
    if (!count && !overflow && !wanted_resync && scale < 2) return 0; /* 1x: nothing recorded, nothing to draw */
    if (vertex_array) glBindVertexArray_(vertex_array);
    gl_ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vram_texture);
    gl_ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, picture_scratch);
    gl_ActiveTexture(GL_TEXTURE0);
    gl_UseProgram(program);
    gl_Uniform1i(u_vram, 0);
    gl_Uniform1i(u_scratch, 1);
    gl_Uniform1i(u_banks, 2);
    gl_Uniform1i(u_entry_map, 3);
    gl_Uniform1i(u_place_map, 4);
    gl_Uniform1i(u_pack, 5);
    gl_Uniform1i(u_glyphs, 6);
    gl_Uniform1i(u_tex_xbr, Settings_Get(SET_XBR));
    hd_text = HdText_Enabled();
    set_samples(Settings_Get(SET_MSAA));
    hd_hud = HdText_HudEnabled();
    opponent_name = HdText_NameEnabled();
    if (overflow || wanted_resync) {
        /* Too much for the arena: from VRAM as it is now, with the state
         * as it is now; the record is superseded. */
        uint32_t words[6];
        SoftGpu_StateWords(words);
        resync(SoftGpu_Scale(), words);
        count = 0;
    }
    batch_precise_count = 0; /* a list left from a record cut short */
    for (at = 0; at + 1 < count;) {
        const uint32_t *op = taken + at;
        size_t used = 1;
        switch (op[0]) {
        case OP_PRECISE:
            used = 2 + (size_t)op[1] * 4;
            if (at + used > count) { at = count; break; }
            batch_precise = (const PgxpVertex *)(op + 2);
            batch_precise_count = op[1];
            break;
        case OP_GP0:
            used = 2 + op[1];
            if (at + used > count) { at = count; break; }
            batch_words = op + 2;
            gp0(op + 2, op[1]);
            batch_words = NULL;
            batch_precise_count = 0;
            if (scale < 2) vertex_count = run_count = 0; /* the state words still count */
            break;
        case OP_LOAD:
            used = 5 + ((size_t)op[3] * op[4] + 1) / 2;
            if (at + used > count) { at = count; break; }
            apply_load((int)op[1], (int)op[2], (int)op[3], (int)op[4], (const uint16_t *)(op + 5));
            break;
        case OP_MOVE:
            used = 7;
            apply_move((int)op[1], (int)op[2], (int)op[3], (int)op[4], (int)op[5], (int)op[6]);
            break;
        case OP_FILL:
            used = 6;
            apply_fill((int)op[1], (int)op[2], (int)op[3], (int)op[4], op[5]);
            break;
        case OP_RESYNC:
            used = 8;
            flush_runs();
            resync((int)op[1], op + 2);
            break;
        default:
            at = count; /* not a record: stop */
            continue;
        }
        if (op[0] != OP_PRECISE) batch_precise_count = 0; /* only for the batch right after it */
        at += used;
    }
    if (scale >= 2) {
        int t;
        flush_runs();
        upload_vram();
        picture_resolve(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
        for (t = 0; t < WIDE_TARGETS; t++) {
            if (wide[t].ms_fbo) resolve(wide[t].ms_fbo, wide[t].fbo, 0, 0, wide[t].width, wide[t].height);
        }
    }
    if (!SoftGpu_Widescreen() && wide[0].fbo + wide[1].fbo + wide[2].fbo + wide[3].fbo) wide_free();
    {
        static unsigned replays, total_us;
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        total_us += (unsigned)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
        if (++replays == 120) {
            LOG(LOG_FRAMES, "OpenGL picture: %u us per replay at %dx", total_us / 120, scale);
            replays = total_us = 0;
        }
    }
    /* Back to the fixed-function state the window's own drawing expects. */
    gl_UseProgram(0);
    gl_BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    gl_ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    if (vertex_array) glBindVertexArray_(0);
    return scale >= 2;
}

unsigned GlPicture_Texture(int *picture_w, int *picture_h)
{
    if (picture_w) *picture_w = SOFT_GPU_WIDTH * scale;
    if (picture_h) *picture_h = SOFT_GPU_HEIGHT * scale;
    return scale >= 2 ? picture_texture : 0;
}

/* The shown area in a texture of its own (see gl_picture.h). */
static GLuint shown_texture, shown_fbo;
static int shown_w, shown_h;

unsigned GlPicture_ShownTexture(int x, int y, int w, int h)
{
    if (scale < 2 || w <= 0 || h <= 0) return 0;
    if (w != shown_w || h != shown_h) {
        if (shown_fbo) gl_DeleteFramebuffers(1, &shown_fbo);
        if (shown_texture) glDeleteTextures(1, &shown_texture);
        shown_texture = make_texture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
        shown_fbo = make_framebuffer(shown_texture);
        shown_w = shown_fbo ? w : 0;
        shown_h = shown_fbo ? h : 0;
        if (!shown_fbo) return 0;
    }
    glDisable(GL_SCISSOR_TEST);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, picture_fbo);
    gl_BindFramebuffer(GL_DRAW_FRAMEBUFFER, shown_fbo);
    gl_BlitFramebuffer(x, y, x + w, y + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl_BindFramebuffer(GL_FRAMEBUFFER, 0);
    return shown_texture;
}

int GlPicture_Scale(void) { return on ? scale : 0; }

static GlWide *wide_for(int x, int y, int w, int h)
{
    int t;
    if (!on || scale < 2) return NULL;
    for (t = 0; t < WIDE_TARGETS; t++) {
        GlWide *wt = &wide[t];
        if (wt->fbo && wt->x1 == x && wt->y1 == y && wt->x2 - wt->x1 + 1 == w && wt->y2 - wt->y1 + 1 >= h) return wt;
    }
    return NULL;
}

unsigned GlPicture_WideTexture(int x, int y, int w, int h, int *picture_w, int *picture_h)
{
    GlWide *wt = wide_for(x, y, w, h);
    if (!wt) return 0;
    if (!wt->drawn) { /* nothing drew them since: stale (SoftGpu_WideFrame) */
        wide_sides(wt, y, h, 0, 0, 0);
        if (wt->ms_fbo) resolve(wt->ms_fbo, wt->fbo, 0, 0, wt->width, wt->height);
    }
    wt->drawn = 0;
    *picture_w = wt->width;
    *picture_h = wt->height;
    return wt->texture;
}

int GlPicture_ReadWide(int x, int y, int w, int h, int wide_w, int want_scale, uint32_t *out)
{
    const GlWide *wt = wide_for(x, y, w, h);
    int i, j, width, height;
    uint8_t *rgba;
    if (!wt || scale != want_scale || wt->width != wide_w * scale) return 0;
    width = wt->width;
    height = h * scale;
    rgba = malloc((size_t)width * (size_t)height * 4);
    if (!rgba) return 0;
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, wt->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, (y - wt->y1) * scale, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    for (j = 0; j < height; j++) {
        for (i = 0; i < width; i++) {
            const uint8_t *p = rgba + ((size_t)j * width + i) * 4;
            out[(size_t)j * width + i] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    free(rgba);
    return 1;
}

int GlPicture_Behind(void) { return on && arena_used > ARENA_WORDS / 2; }

int GlPicture_Read(int x, int y, int w, int h, uint32_t *out)
{
    int i, j;
    uint8_t *rgba;
    if (!on || scale < 2 || w <= 0 || h <= 0) return 0;
    rgba = malloc((size_t)w * (size_t)h * 4);
    if (!rgba) return 0;
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, picture_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gl_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            const uint8_t *p = rgba + ((size_t)j * w + i) * 4;
            out[(size_t)j * w + i] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    free(rgba);
    return 1;
}
