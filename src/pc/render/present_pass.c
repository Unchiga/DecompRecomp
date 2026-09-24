/* The present pass (present_pass.h).
 *
 * One fragment program over the picture's quad, in the compatibility
 * profile the presenter already draws with (immediate mode, glOrtho), so the
 * quad's own texture coordinates and the bound picture texture are what it
 * samples. Effects, each a setting that is off at its default:
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
    X(PFNGLUNIFORM2FPROC, Uniform2f)

#define DECLARE(type, name) static type pp_##name;
PASS_FUNCTIONS(DECLARE)
#undef DECLARE

static const char *vertex_source =
    "#version 120\n"
    "void main() {\n"
    "    gl_Position = ftransform();\n"
    "    gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    "}\n";

static const char *fragment_source =
    "#version 120\n"
    "uniform sampler2D picture;\n"
    "uniform float brightness, contrast, saturation, gamma;\n"
    "uniform int crt;\n"
    "uniform float lines, t0, t1;\n"
    "void main() {\n"
    "    vec3 c = texture2D(picture, gl_TexCoord[0].xy).rgb;\n"
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

static int state; /* 0 not tried, 1 ready, -1 unavailable */
static GLuint program;
static GLint u_picture, u_brightness, u_contrast, u_saturation, u_gamma, u_crt, u_lines, u_t0, u_t1;

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

static int build(void)
{
    GLuint vertex, fragment;
    GLint ok = 0;
#define LOAD(type, name) \
    pp_##name = (type)SDL_GL_GetProcAddress("gl" #name); \
    if (!pp_##name) return 0;
    PASS_FUNCTIONS(LOAD)
#undef LOAD
    vertex = compile(GL_VERTEX_SHADER, vertex_source);
    fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) return 0;
    program = pp_CreateProgram();
    pp_AttachShader(program, vertex);
    pp_AttachShader(program, fragment);
    pp_LinkProgram(program);
    pp_DeleteShader(vertex);
    pp_DeleteShader(fragment);
    pp_GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        pp_GetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "memories-pc: present pass: program: %s\n", log);
        return 0;
    }
    u_picture = pp_GetUniformLocation(program, "picture");
    u_brightness = pp_GetUniformLocation(program, "brightness");
    u_contrast = pp_GetUniformLocation(program, "contrast");
    u_saturation = pp_GetUniformLocation(program, "saturation");
    u_gamma = pp_GetUniformLocation(program, "gamma");
    u_crt = pp_GetUniformLocation(program, "crt");
    u_lines = pp_GetUniformLocation(program, "lines");
    u_t0 = pp_GetUniformLocation(program, "t0");
    u_t1 = pp_GetUniformLocation(program, "t1");
    return 1;
}

int PresentPass_Wanted(void)
{
    return Settings_Get(SET_BRIGHTNESS) != 100 || Settings_Get(SET_CONTRAST) != 100 ||
           Settings_Get(SET_SATURATION) != 100 || Settings_Get(SET_GAMMA) != 100 || Settings_Get(SET_CRT);
}

int PresentPass_Begin(int source_h, float t0, float t1)
{
    GLint unit = 0;
    int multiple = (source_h + 120) / 240;
    if (!state) {
        state = build() ? 1 : -1;
        if (state < 0) fprintf(stderr, "memories-pc: present pass unavailable; colour settings do nothing\n");
    }
    if (state < 0) return 0;
    pp_UseProgram(program);
    /* The picture is bound on whichever unit is active (the presenter's
     * fixed-function quad samples that one too). */
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    pp_Uniform1i(u_picture, unit - GL_TEXTURE0);
    pp_Uniform1f(u_brightness, (float)Settings_Get(SET_BRIGHTNESS) / 100.0f);
    pp_Uniform1f(u_contrast, (float)Settings_Get(SET_CONTRAST) / 100.0f);
    pp_Uniform1f(u_saturation, (float)Settings_Get(SET_SATURATION) / 100.0f);
    pp_Uniform1f(u_gamma, (float)Settings_Get(SET_GAMMA) / 100.0f);
    pp_Uniform1i(u_crt, Settings_Get(SET_CRT));
    pp_Uniform1f(u_lines, (float)source_h / (float)(multiple > 0 ? multiple : 1));
    pp_Uniform1f(u_t0, t0);
    pp_Uniform1f(u_t1, t1);
    return 1;
}

void PresentPass_End(void)
{
    if (state > 0) pp_UseProgram(0);
}
