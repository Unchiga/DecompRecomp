/* Integration test for SDL's secondary-window context ownership. Build with
 * function/data sections and --gc-sections to omit unrelated game entrypoints.
 * Uses real SDL renderers and OpenGL; SDL_VIDEODRIVER=offscreen is supported. */
#include "../../src/pc/platform/sdl.c"
#include <assert.h>

void ModsWindow_Init(void) {}
void ModsWindow_Size(int *w, int *h) { *w = 780; *h = 294; }
void ModsWindow_Draw(MenuCanvas *c)
{
    memset(c->pixels, 0x55, (size_t)c->stride * c->height * 4);
}

int main(void)
{
    int i;
    GLint unpack;
    assert(SDL_Init(SDL_INIT_VIDEO));
    window = SDL_CreateWindow("Mods context regression", 320, 240, SDL_WINDOW_OPENGL);
    gl_context = SDL_GL_CreateContext(window);
    use_gl = gl_context != NULL;
    assert(window && use_gl && gl_context);
    for (i = 0; i < 3; ++i) {
        /* A distinct game stride makes renderer-state leakage observable. */
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 320);
        Platform_OpenMods();
        assert(mods_window && mods_renderer && mods_texture);
        assert(SDL_GL_GetCurrentContext() == gl_context);
        assert(SDL_GL_GetCurrentWindow() == window);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack);
        assert(unpack == 320);
        draw_mods();
        assert(SDL_GL_GetCurrentContext() == gl_context);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack);
        assert(unpack == 320);
        close_mods();
        assert(SDL_GL_GetCurrentContext() == gl_context);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack);
        assert(unpack == 320);
        assert(!quit);
    }
    destroy_window();
    SDL_Quit();
    puts("mods context: open, redraw, close, and reopen passed");
    return 0;
}
