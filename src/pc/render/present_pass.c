/* The present pass (present_pass.h).
 *
 * One fragment program over the picture's quad, in the compatibility
 * profile the presenter already draws with (immediate mode, glOrtho), so the
 * quad's own texture coordinates and the bound picture texture are what it
 * samples. Effects, each a setting that is off at its default:
 *   Colour (Video > Colour): gamma, then contrast about mid grey, then
 *   brightness, then saturation against Rec. 601 luma. */
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
    "void main() {\n"
    "    vec3 c = texture2D(picture, gl_TexCoord[0].xy).rgb;\n"
    "    c = pow(c, vec3(1.0 / gamma));\n"
    "    c = (c - 0.5) * contrast + 0.5;\n"
    "    c *= brightness;\n"
    "    c = mix(vec3(dot(c, vec3(0.299, 0.587, 0.114))), c, saturation);\n"
    "    gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
    "}\n";

static int state; /* 0 not tried, 1 ready, -1 unavailable */
static GLuint program;
static GLint u_picture, u_brightness, u_contrast, u_saturation, u_gamma;

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
    return 1;
}

int PresentPass_Wanted(void)
{
    return Settings_Get(SET_BRIGHTNESS) != 100 || Settings_Get(SET_CONTRAST) != 100 ||
           Settings_Get(SET_SATURATION) != 100 || Settings_Get(SET_GAMMA) != 100;
}

int PresentPass_Begin(int source_w, int source_h, int output_w, int output_h)
{
    GLint unit = 0;
    (void)source_w, (void)source_h, (void)output_w, (void)output_h;
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
    return 1;
}

void PresentPass_End(void)
{
    if (state > 0) pp_UseProgram(0);
}
