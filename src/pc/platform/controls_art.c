#include "controls_art.h"
#include "../assets/ps1_controller_png.h"
#include <png.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *original;
static uint32_t *scaled;
static int source_width, source_height, cached_width, cached_height, attempted;
static int load(void)
{
    if (attempted)
        return original != NULL;
    attempted = 1;
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, ps1_controller_png, sizeof(ps1_controller_png)))
        return 0;
    image.format = PNG_FORMAT_RGBA;
    original = malloc(PNG_IMAGE_SIZE(image));
    if (!original || !png_image_finish_read(&image, NULL, original, 0, NULL)) {
        free(original);
        original = NULL;
        png_image_free(&image);
        return 0;
    }
    source_width = (int)image.width;
    source_height = (int)image.height;
    png_image_free(&image);
    return 1;
}
int ControlsArt_Height(int width) { return width * 1067 / 1474; }
static int resize(int width)
{
    if (cached_width == width)
        return 1;
    int height = ControlsArt_Height(width);
    uint32_t *next = malloc((size_t)width * height * sizeof(*next));
    if (!next)
        return 0;
    /* Area sampling of premultiplied channels keeps transparent edges clean.
     * Cache by display size; the PNG is decoded once, on the main thread. */
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            int x0 = x * source_width / width, x1 = (x + 1) * source_width / width;
            int y0 = y * source_height / height, y1 = (y + 1) * source_height / height;
            if (x1 <= x0)
                x1 = x0 + 1;
            if (y1 <= y0)
                y1 = y0 + 1;
            uint64_t a = 0, r = 0, g = 0, b = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    const unsigned char *p = original + ((size_t)sy * source_width + sx) * 4;
                    a += p[3];
                    r += (unsigned)p[0] * p[3];
                    g += (unsigned)p[1] * p[3];
                    b += (unsigned)p[2] * p[3];
                }
            unsigned count = (unsigned)((x1 - x0) * (y1 - y0));
            next[y * width + x] = a ? ((uint32_t)(a / count) << 24) | ((uint32_t)(r / a) << 16) |
                                          ((uint32_t)(g / a) << 8) | (uint32_t)(b / a)
                                    : 0;
        }
    free(scaled);
    scaled = next;
    cached_width = width;
    cached_height = height;
    return 1;
}
int ControlsArt_Draw(MenuCanvas *canvas, int x, int y, int width)
{
    if (width <= 0 || width > 8192 || !load() || !resize(width))
        return 0;
    for (int j = 0; j < cached_height; j++)
        for (int i = 0; i < cached_width; i++) {
            int dx = x + i, dy = y + j;
            if (dx < 0 || dy < 0 || dx >= canvas->width || dy >= canvas->height)
                continue;
            uint32_t p = scaled[j * cached_width + i], a = p >> 24;
            if (!a)
                continue;
            uint32_t *dst = &canvas->pixels[dy * canvas->stride + dx], q = *dst;
            unsigned r = (((p >> 16) & 255) * a + ((q >> 16) & 255) * (255 - a) + 127) / 255;
            unsigned g = (((p >> 8) & 255) * a + ((q >> 8) & 255) * (255 - a) + 127) / 255;
            unsigned b = ((p & 255) * a + (q & 255) * (255 - a) + 127) / 255;
            *dst = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    return 1;
}
