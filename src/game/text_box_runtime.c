#include "../types.h"
#include "display_object.h"
#include "duel_effect_update_object_layout.h"
#include "duel_effect.h"
#include "text_box_runtime.h"

/* These three reach the record through the parameter directly. What used to
 * stand at each use was a cast of the pointer to its own type, left from when
 * the parameter was a byte pointer; removing them is byte-identical, and
 * measured.
 *
 * What does change the code is a typed local. Hoisting the record into one
 * changes the prologue register saves under GCC 2.8.1, which is the
 * constraint the old note here was about. */
void TextBox_SetPos(DuelEffectChannel *record, s32 x, s32 y)
{
    DisplayObject *object;

    object = record->field_28;
    record->field_3C = x;
    record->field_40 = y;
    if (object != 0) {
        object->field_30.h.field_30 = x;
        object->field_30.h.field_32 = y;
    }
    object = record->field_2C;
    if (object != 0) {
        if (object->field_1E == 4)
            func_80039140(record);
        else {
            object->field_30.h.field_30 = x;
            object->field_30.h.field_32 = y;
        }
    }
    object = record->field_30;
    if (object != 0) {
        if (object->field_1E == 4)
            DuelEffect_UpdateObjectLayout(record);
        else {
            object->field_30.h.field_30 = record->field_3E + x - 16;
            object->field_30.h.field_32 = record->field_42 + y - 16;
        }
    }
}

#ifdef MEMORIES_PC
/* Both build a menu's text in one go, with no frame between steps for the
   player to press anything. A page that waits for a button (state 4: a
   {page}, or text taller than the box) would wait here forever. The
   console's own menus never do; a translation's may, and there the rest of
   the text is left out, as what is past a box's last line would be. */
static s32 TextBox_BuildAtOnceStep(DuelEffectChannel *object)
{
    TextBox_BuildStep(object);
    if ((object->state_51 & DUEL_EFFECT_STATE_INDEX_MASK) == 4) {
        object->state_51 = 0;
        object->flags_34 |= TEXT_BOX_FLAG_DONE;
    }
    return (object->flags_34 & TEXT_BOX_FLAG_DONE) != 0;
}
#endif

void func_80039A14(DuelEffectChannel *object)
{
    object->flags_34 |= TEXT_BOX_FLAG_BUILD_REQUESTED;
#ifdef MEMORIES_PC
    while (!TextBox_BuildAtOnceStep(object)) {
    }
#else
    do {
        TextBox_BuildStep(object);
    } while (!(object->flags_34 & TEXT_BOX_FLAG_DONE));
#endif
}

void func_80039A60(DuelEffectChannel *object)
{
    object->flags_34 |= 0xA00;
#ifdef MEMORIES_PC
    while (!TextBox_BuildAtOnceStep(object)) {
    }
#else
    do {
        TextBox_BuildStep(object);
    } while (!(object->flags_34 & TEXT_BOX_FLAG_DONE));
#endif
}
