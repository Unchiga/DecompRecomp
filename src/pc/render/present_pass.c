/* The present pass (present_pass.h).
 *
 * One fragment program over the picture's quad, in the compatibility
 * profile the presenter already draws with (immediate mode, glOrtho), so the
 * quad's own texture coordinates and the bound picture texture are what it
 * samples. Effects, each a setting that is off at its default:
 *   xBR pixel smoothing (Video > Effects): the picture is read through xBR
 *   (XBR_SOURCE below) instead of one texel at a time. It works on the
 *   texels of the texture shown, so it does most at internal resolution 1x;
 *   at 4x the picture's texels are already small.
 *   Reduce flashes (Video > Effects): when the picture's average brightness
 *   would rise faster than FLASH_RISE per second, the whole picture is
 *   darkened to that rise, as a camera's exposure would follow it. A flash
 *   is a large area changing, so a card or the cursor moving over a dark
 *   board barely moves the average and is never dimmed, and darkening is
 *   never held back. (Darkening only the blocks that brighten leaves blotches
 *   on the picture.) Applied first, to the picture as the game drew it.
 *   Colour (Video > Color): gamma, then contrast about mid grey, then
 *   brightness, then saturation against Rec. 601 luma.
 *   CRT (Video > Effects): a scanline per line of the console's picture
 *   (240: the source's height over its nearest multiple of 240) darkened
 *   towards its edges, and an aperture grille of window pixels, with the
 *   brightness they take given back. */
#include "present_pass.h"
#include "pc/platform/settings.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdio.h>

#define PASS_FUNCTIONS(X) \
    X(PFNGLCREATESHADERPROC, CreateShader) \
    X(PFNGLSHADERSOURCEPROC, ShaderSource) \
    X(PFNGLCOMPILESHADERPROC, CompileShader) \
    X(PFNGLGETSHADERIVPROC, GetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram) \
    X(PFNGLATTACHSHADERPROC, AttachShader) \
    X(PFNGLLINKPROGRAMPROC, LinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog) \
    X(PFNGLDELETESHADERPROC, DeleteShader) \
    X(PFNGLUSEPROGRAMPROC, UseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, Uniform1i) \
    X(PFNGLUNIFORM1FPROC, Uniform1f) \
    X(PFNGLUNIFORM2FPROC, Uniform2f) \
    X(PFNGLUNIFORM4FPROC, Uniform4f)

/* Only the flash reduction needs these; without them the other effects
 * still work. */
#define FLASH_FUNCTIONS(X) \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus) \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture)

#define DECLARE(type, name) static type pp_##name;
PASS_FUNCTIONS(DECLARE)
FLASH_FUNCTIONS(DECLARE)
#undef DECLARE

/* How fast (in Rec. 601 luma, 0 to 1, per second) the picture may brighten:
 * black to white takes half a second, about twice as fast as the game's own
 * fades, and one white frame over a dark scene at 60 frames a second rises
 * by 0.03. */
#define FLASH_RISE 2.0f

static const char *vertex_source =
    "#version 120\n"
    "void main() {\n"
    "    gl_Position = ftransform();\n"
    "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    "}\n";

/* xBR, level 2, written from the published rules: for each corner of a
 * texel E (here the one towards F, H and I; the others are mirrors), an edge
 * across that corner is found when the colours along the anti-diagonal
 * differ less than along the diagonal, weighted as xBR does, and a 2:1
 * shallow or steep edge when the next texels continue it. The corner is then
 * cut by that edge's line and filled with F or H, whichever is closer to E;
 * the line is antialiased over one window pixel. Colour distance is in YUV
 * weighted 48:7:6, and colours closer than EQUAL count as the same. Texels
 * outside the picture's rectangle repeat its edge. The neighbourhood (x
 * right, y down):
 *        B  C
 *     D  E  F  F4
 *     G  H  I  I4
 *        H5 I5 */
#define XBR_SOURCE \
    "uniform vec2 size;\n" \
    "uniform vec4 rect;\n" \
    "vec3 texel(vec2 p) {\n" \
    "    return texture2D(picture, (clamp(p, rect.xy, rect.zw - 1.0) + 0.5) / size).rgb;\n" \
    "}\n" \
    "float dist(vec3 a, vec3 b) {\n" \
    "    vec3 k = a - b;\n" \
    "    vec3 yuv = abs(vec3(dot(k, vec3(0.299, 0.587, 0.114)), dot(k, vec3(-0.169, -0.331, 0.5)),\n" \
    "                        dot(k, vec3(0.5, -0.419, -0.081))));\n" \
    "    return dot(yuv, vec3(48.0, 7.0, 6.0)) / 48.0;\n" \
    "}\n" \
    "bool same(vec3 a, vec3 b) { return dist(a, b) < 0.06; }\n" \
    "float cover(float f, float slope, float w) { return clamp(f / (slope * w) + 0.5, 0.0, 1.0); }\n" \
    "vec4 corner(vec2 e, vec2 d, vec2 q, float w) {\n" \
    "    vec3 E = texel(e), B = texel(e + vec2(0, -1) * d), C = texel(e + vec2(1, -1) * d);\n" \
    "    vec3 D = texel(e + vec2(-1, 0) * d), F = texel(e + vec2(1, 0) * d), G = texel(e + vec2(-1, 1) * d);\n" \
    "    vec3 H = texel(e + vec2(0, 1) * d), I = texel(e + d), F4 = texel(e + vec2(2, 0) * d);\n" \
    "    vec3 I4 = texel(e + vec2(2, 1) * d), H5 = texel(e + vec2(0, 2) * d), I5 = texel(e + vec2(1, 2) * d);\n" \
    "    bool may = !same(E, F) && !same(E, H) &&\n" \
    "               (!same(F, B) && !same(H, D) || same(E, I) && !same(F, I4) && !same(H, I5) ||\n" \
    "                same(E, G) || same(E, C));\n" \
    "    float across = dist(E, C) + dist(E, G) + dist(I, F4) + dist(I, H5) + 4.0 * dist(H, F);\n" \
    "    float along = dist(H, D) + dist(H, I5) + dist(F, I4) + dist(F, B) + 4.0 * dist(E, I);\n" \
    "    if (!may || across >= along) return vec4(E, 0.0);\n" \
    "    float cut = cover(q.x + q.y - 1.5, 1.4142, w);\n" \
    "    if (2.0 * dist(F, G) <= dist(H, C) && !same(E, G) && !same(D, G))\n" \
    "        cut = max(cut, cover(0.5 * q.x + q.y - 1.0, 1.118, w));\n" \
    "    if (dist(F, G) >= 2.0 * dist(H, C) && !same(E, C) && !same(B, C))\n" \
    "        cut = max(cut, cover(q.x + 0.5 * q.y - 1.0, 1.118, w));\n" \
    "    return vec4(dist(E, F) <= dist(E, H) ? F : H, cut);\n" \
    "}\n" \
    "vec3 xbr(vec2 uv) {\n" \
    "    vec2 p = uv * size, e = floor(p), f = p - e;\n" \
    "    float w = max(fwidth(p.x), fwidth(p.y));\n" \
    "    vec4 best = corner(e, vec2(1, 1), f, w);\n" \
    "    vec4 k = corner(e, vec2(-1, 1), vec2(1.0 - f.x, f.y), w);\n" \
    "    if (k.a > best.a) best = k;\n" \
    "    k = corner(e, vec2(1, -1), vec2(f.x, 1.0 - f.y), w);\n" \
    "    if (k.a > best.a) best = k;\n" \
    "    k = corner(e, vec2(-1, -1), 1.0 - f, w);\n" \
    "    if (k.a > best.a) best = k;\n" \
    "    return mix(texel(e), best.rgb, best.a);\n" \
    "}\n"

static const char *fragment_source =
    "#version 120\n"
    "uniform sampler2D picture, level;\n"
    "uniform float brightness, contrast, saturation, gamma;\n"
    "uniform int crt, flash, scaler;\n"
    "uniform float lines, t0, t1;\n"
    XBR_SOURCE
    "void main() {\n"
    "    vec3 c = scaler != 0 ? xbr(gl_TexCoord[0].xy) : texture2D(picture, gl_TexCoord[0].xy).rgb;\n"
    "    if (flash != 0) c *= texture2D(level, vec2(0.5)).g;\n"
    "    c = pow(c, vec3(1.0 / gamma));\n"
    "    c = (c - 0.5) * contrast + 0.5;\n"
    "    c *= brightness;\n"
    "    c = mix(vec3(dot(c, vec3(0.299, 0.587, 0.114))), c, saturation);\n"
    "    if (crt != 0) {\n"
    "        float s = sin(3.14159265 * (gl_TexCoord[0].y - t0) / (t1 - t0) * lines);\n"
    "        float m = mod(gl_FragCoord.x, 3.0);\n"
    "        c *= mix(0.55, 1.0, s * s);\n"
    "        c *= (m < 1.0 ? vec3(1.0, 0.8, 0.8) : m < 2.0 ? vec3(0.8, 1.0, 0.8) : vec3(0.8, 0.8, 1.0)) * 1.3;\n"
    "    }\n"
    "    gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
    "}\n";

/* One fragment: the picture's average brightness now (32 x 24 samples), the
 * brightness it was shown at last time (history's red), and from those the
 * brightness it may show (red) and the gain that gives it (green). */
static const char *measure_source =
    "#version 120\n"
    "uniform sampler2D picture, history;\n"
    "uniform vec4 area;\n"
    "uniform float rise;\n"
    "void main() {\n"
    "    float now = 0.0;\n"
    "    for (int y = 0; y < 24; y++)\n"
    "        for (int x = 0; x < 32; x++) {\n"
    "            vec2 at = (vec2(x, y) + 0.5) / vec2(32.0, 24.0);\n"
    "            now += dot(texture2D(picture, mix(area.xy, area.zw, at)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        }\n"
    "    now /= 768.0;\n"
    "    float shown = min(now, texture2D(history, vec2(0.5)).r + rise);\n"
    "    gl_FragColor = vec4(shown, now > shown ? shown / now : 1.0, 0.0, 1.0);\n"
    "}\n";

static int state; /* 0 not tried, 1 ready, -1 unavailable */
static GLuint program;
static GLint u_picture, u_level, u_brightness, u_contrast, u_saturation, u_gamma, u_crt, u_flash, u_scaler, u_size, u_rect, u_lines, u_t0,
    u_t1;

static int flash_state; /* as state, for the flash reduction */
static GLuint measure, levels[2], level_fbo[2];
static GLint m_picture, m_history, m_area, m_rise;
static int current, primed, flash_on; /* levels[current] holds the last level; primed: it is this run's */
static Uint64 last_ns;

static GLuint compile(GLenum kind, const char *source)
{
    GLuint shader = pp_CreateShader(kind);
    GLint ok = 0;
    pp_ShaderSource(shader, 1, &source, NULL);
    pp_CompileShader(shader);
    pp_GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        pp_GetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "memories-pc: present pass: shader: %s\n", log);
        pp_DeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link(const char *fragment_text)
{
    GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_text);
    GLuint linked;
    GLint ok = 0;
    if (!vertex || !fragment) return 0;
    linked = pp_CreateProgram();
    pp_AttachShader(linked, vertex);
    pp_AttachShader(linked, fragment);
    pp_LinkProgram(linked);
    pp_DeleteShader(vertex);
    pp_DeleteShader(fragment);
    pp_GetProgramiv(linked, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        pp_GetProgramInfoLog(linked, sizeof(log), NULL, log);
        fprintf(stderr, "memories-pc: present pass: program: %s\n", log);
        return 0;
    }
    return linked;
}

static int build(void)
{
#define LOAD(type, name) \
    pp_##name = (type)SDL_GL_GetProcAddress("gl" #name); \
    if (!pp_##name) return 0;
    PASS_FUNCTIONS(LOAD)
#undef LOAD
    program = link(fragment_source);
    if (!program) return 0;
    u_picture = pp_GetUniformLocation(program, "picture");
    u_level = pp_GetUniformLocation(program, "level");
    u_brightness = pp_GetUniformLocation(program, "brightness");
    u_contrast = pp_GetUniformLocation(program, "contrast");
    u_saturation = pp_GetUniformLocation(program, "saturation");
    u_gamma = pp_GetUniformLocation(program, "gamma");
    u_crt = pp_GetUniformLocation(program, "crt");
    u_flash = pp_GetUniformLocation(program, "flash");
    u_scaler = pp_GetUniformLocation(program, "scaler");
    u_size = pp_GetUniformLocation(program, "size");
    u_rect = pp_GetUniformLocation(program, "rect");
    u_lines = pp_GetUniformLocation(program, "lines");
    u_t0 = pp_GetUniformLocation(program, "t0");
    u_t1 = pp_GetUniformLocation(program, "t1");
    return 1;
}

/* The measuring program and two 1 x 1 level textures with their
 * framebuffers, one written while the other is read. */
static int build_flash(void)
{
    int i;
#define LOAD(type, name) \
    pp_##name = (type)SDL_GL_GetProcAddress("gl" #name); \
    if (!pp_##name) return 0;
    FLASH_FUNCTIONS(LOAD)
#undef LOAD
    measure = link(measure_source);
    if (!measure) return 0;
    m_picture = pp_GetUniformLocation(measure, "picture");
    m_history = pp_GetUniformLocation(measure, "history");
    m_area = pp_GetUniformLocation(measure, "area");
    m_rise = pp_GetUniformLocation(measure, "rise");
    glGenTextures(2, levels);
    pp_GenFramebuffers(2, level_fbo);
    for (i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, levels[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* 16 bits, so that a small rise between two close presents is not
         * rounded away. */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, 1, 1, 0, GL_RGBA, GL_UNSIGNED_SHORT, NULL);
        pp_BindFramebuffer(GL_FRAMEBUFFER, level_fbo[i]);
        pp_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, levels[i], 0);
        if (pp_CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            pp_BindFramebuffer(GL_FRAMEBUFFER, 0);
            fprintf(stderr, "memories-pc: present pass: flash framebuffer incomplete\n");
            return 0;
        }
    }
    pp_BindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

/* Measure the picture into the other level texture, which becomes the
 * current one. The picture is bound on the active unit. */
static void measure_level(GLuint picture, GLint unit, float s0, float t0, float s1, float t1)
{
    Uint64 now = SDL_GetTicksNS();
    float seconds = primed ? (float)(now - last_ns) / 1e9f : 0.0f;
    GLint framebuffer = 0, viewport[4];
    int next = current ^ 1;
    if (seconds > 0.1f) seconds = 0.1f; /* a stall is not a licence to flash */
    last_ns = now;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    pp_BindFramebuffer(GL_FRAMEBUFFER, level_fbo[next]);
    glViewport(0, 0, 1, 1);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    pp_UseProgram(measure);
    glBindTexture(GL_TEXTURE_2D, picture);
    pp_ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, levels[current]);
    pp_ActiveTexture((GLenum)unit);
    pp_Uniform1i(m_picture, unit - GL_TEXTURE0);
    pp_Uniform1i(m_history, 1);
    pp_Uniform4f(m_area, s0, t0, s1, t1);
    /* First measure of this run: nothing was shown to rise from. */
    pp_Uniform1f(m_rise, primed ? FLASH_RISE * seconds : 2.0f);
    glBegin(GL_QUADS);
    glVertex2f(-1, -1);
    glVertex2f(1, -1);
    glVertex2f(1, 1);
    glVertex2f(-1, 1);
    glEnd();
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    pp_BindFramebuffer(GL_FRAMEBUFFER, (GLuint)framebuffer);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    current = next;
    primed = 1;
}

/* Whether an effect other than the flash reduction is on. */
static int others_wanted(void)
{
    return Settings_Get(SET_BRIGHTNESS) != 100 || Settings_Get(SET_CONTRAST) != 100 ||
           Settings_Get(SET_SATURATION) != 100 || Settings_Get(SET_GAMMA) != 100 || Settings_Get(SET_CRT) ||
           Settings_Get(SET_XBR);
}

int PresentPass_Wanted(void)
{
    if (!Settings_Get(SET_FLASH)) primed = 0; /* what was shown since is not in the level */
    return others_wanted() || Settings_Get(SET_FLASH);
}

int PresentPass_Begin(unsigned texture, int source_h, float s0, float t0, float s1, float t1)
{
    GLint unit = 0;
    int multiple = (source_h + 120) / 240;
    if (!state) {
        state = build() ? 1 : -1;
        if (state < 0) fprintf(stderr, "memories-pc: present pass unavailable; colour settings do nothing\n");
    }
    if (state < 0) return 0;
    /* The picture is bound on whichever unit is active (the presenter's
     * fixed-function quad samples that one too). */
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    flash_on = 0;
    if (Settings_Get(SET_FLASH)) {
        if (!flash_state) {
            flash_state = build_flash() ? 1 : -1;
            if (flash_state < 0) fprintf(stderr, "memories-pc: flash reduction unavailable\n");
        }
        if (flash_state > 0) {
            measure_level((GLuint)texture, unit, s0, t0, s1, t1);
            pp_ActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, levels[current]);
            pp_ActiveTexture((GLenum)unit);
            flash_on = 1;
        }
    }
    if (!flash_on && !others_wanted()) return 0; /* nothing to do: the plain quad, exactly */
    pp_UseProgram(program);
    pp_Uniform1i(u_picture, unit - GL_TEXTURE0);
    pp_Uniform1i(u_level, 1);
    pp_Uniform1i(u_flash, flash_on);
    pp_Uniform1f(u_brightness, (float)Settings_Get(SET_BRIGHTNESS) / 100.0f);
    pp_Uniform1f(u_contrast, (float)Settings_Get(SET_CONTRAST) / 100.0f);
    pp_Uniform1f(u_saturation, (float)Settings_Get(SET_SATURATION) / 100.0f);
    pp_Uniform1f(u_gamma, (float)Settings_Get(SET_GAMMA) / 100.0f);
    pp_Uniform1i(u_crt, Settings_Get(SET_CRT));
    pp_Uniform1i(u_scaler, Settings_Get(SET_XBR));
    if (Settings_Get(SET_XBR)) {
        /* xBR works on the texture's own texels, inside the picture's
         * rectangle of them. */
        GLint w = 1, h = 1;
        glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        pp_Uniform2f(u_size, (float)w, (float)h);
        pp_Uniform4f(u_rect, (float)(int)(s0 * w + 0.5f), (float)(int)(t0 * h + 0.5f), (float)(int)(s1 * w + 0.5f),
                     (float)(int)(t1 * h + 0.5f));
    }
    pp_Uniform1f(u_lines, (float)source_h / (float)(multiple > 0 ? multiple : 1));
    pp_Uniform1f(u_t0, t0);
    pp_Uniform1f(u_t1, t1);
    return 1;
}

void PresentPass_End(void)
{
    if (state <= 0) return;
    pp_UseProgram(0);
    if (flash_on) {
        GLint unit = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
        pp_ActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        pp_ActiveTexture((GLenum)unit);
    }
}
