#include "pc/compat/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/model_graphics_state.h"
#include "game/model_spark_effect.h"
#include "game/model_slot_properties.h"
#include "pc/guest/mips.h"
#include "pc/debug/log.h"

/* Each MODEL.MRG record carries two small MIPS control modules that the
 * loader swaps into fixed slots: a primary module (slot A 0x8013A000, slot B
 * 0x8017A000; in every record seen it is `return 2`) and a variant module
 * (0x8013B000 / 0x8017B000) holding the monster's attack choreography and
 * impact particles. The game calls each through the word at +4. There are
 * 1,181 distinct variant modules, so they are not translated one by one:
 * the default runs the loaded module through the interpreter, which keeps
 * every monster's own attack movement. A module the interpreter cannot run
 * (reported once per loaded module) falls back to the native approximation
 * below, as does MEMORIES_MODEL_MODULES=native. */

typedef struct {
    ModelSparkEffect effect;
    int age;
} MemoriesModelImpact;

static void trace_model_command(const char *kind, int command)
{
    LOG(LOG_MODEL, "%s slot=%u command=%d", kind, (unsigned)D_8009AFA0, command);
}

static int native_primary(void *context, int command)
{
    (void)context;
    (void)command;
    return 2;
}

/* The bring-up stand-in for a variant module: the resident spark burst on
 * the defender in place of the monster's own choreography. Effects follow
 * Model_GetActiveSlotIndex(), so it points at the defender while the burst
 * is initialized and drawn. */
static int native_variant(void *context, int command)
{
    MemoriesModelImpact *impact = context;
    unsigned char attacker = D_8009AFA0;
    int i;
    int result;

    D_8009AFA0 = attacker ^ 1;
    if (command >= 0) {
        impact->age = 0;
        result = func_8006F1B4(&impact->effect, command);
        /* Only the compact spark phase: the full presentation grows white
         * quads until they cover the monster, which is not a contact hit. */
        impact->effect.spark_count = 2;
        impact->effect.flash_count = 0;
        impact->effect.mode = 1;
        for (i = 0; i < impact->effect.spark_count; i++) {
            impact->effect.sizes[i].size = 1;
            impact->effect.sizes[i].grow = 8;
        }
    } else {
        if (impact->age < 6) {
            result = func_8006F1B4(&impact->effect, command);
            impact->age++;
            /* func_8006F1B4 fades its subject to black as part of a longer
             * presentation; an impact must leave the defender visible until
             * its own dying animation. */
            Model_SetSlotTintTarget(D_8009AFA0, 0, 0x80, 0x80, 0x80);
            /* 3 starts the losing monster's dying animation: after the last
             * impact frame, then complete on the next update. */
            if (impact->age == 6) {
                result = 3;
            }
        } else {
            result = 2;
        }
    }
    D_8009AFA0 = attacker;
    return result;
}

/* module: the slot base (the identifier word; the entry is at +4). */
static int run_module(uint32_t module, int variant, void *context, int command)
{
    static int native = -1;
    static uint32_t failed[2][2]; /* [slot][variant]: identifier of a module that failed */
    int slot = module >= 0x8017A000u;
    uint32_t identifier = *(const uint32_t *)(uintptr_t)module;
    uint32_t entry = *(const uint32_t *)(uintptr_t)(module + 4);

    trace_model_command(variant ? "variant" : "primary", command);
    if (native < 0) {
        const char *mode = getenv("MEMORIES_MODEL_MODULES");
        native = mode && strcmp(mode, "native") == 0;
    }
    if (!native && entry != 0 && failed[slot][variant] != identifier) {
        uint32_t args[2], result;
        args[0] = (uint32_t)(uintptr_t)context;
        args[1] = (uint32_t)command;
        if (!Memories_MipsTry(module + 4, args, 2, &result)) {
            return (int)result;
        }
        fprintf(stderr, "memories-pc: model %s module %u (slot %c) cannot run; using the native stand-in\n",
                variant ? "variant" : "primary", (unsigned)identifier, slot ? 'B' : 'A');
        failed[slot][variant] = identifier;
    }
    return variant ? native_variant(context, command) : native_primary(context, command);
}

int Memories_ModelPrimaryControlA(void *context, int command) { return run_module(0x8013A000u, 0, context, command); }
int Memories_ModelVariantControlA(void *context, int command) { return run_module(0x8013B000u, 1, context, command); }
int Memories_ModelPrimaryControlB(void *context, int command) { return run_module(0x8017A000u, 0, context, command); }
int Memories_ModelVariantControlB(void *context, int command) { return run_module(0x8017B000u, 1, context, command); }
