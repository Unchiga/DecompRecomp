/* Yamyi's optional interface mods, sharing one settings entry. */
#include "pc/mods/modapi.h"
int YamyiColors_Init(const MemoriesModHost *, MemoriesMod *);
int YamyiConfirm_Init(const MemoriesModHost *, MemoriesMod *);
static MemoriesMod colors, confirm;
static void frame(void) { colors.frame(); confirm.frame(); }
static void applied(int on) { colors.applied(on); confirm.applied(on); }
static void reset(void) { colors.reset(); confirm.reset(); }
int MemoriesModInit(const MemoriesModHost *host, MemoriesMod *mod)
{
    if (!YamyiColors_Init(host, &colors) || !YamyiConfirm_Init(host, &confirm)) return 0;
    mod->api = MEMORIES_MOD_API;
    mod->frame = frame;
    mod->applied = applied;
    mod->reset = reset;
    mod->overlay = colors.overlay;
    mod->overlay_signature = colors.overlay_signature;
    return 1;
}
