#ifndef MEMORIES_PC_PLATFORM_TITLE_JUMP_H
#define MEMORIES_PC_PLATFORM_TITLE_JUMP_H
/* Back to the title screen from anywhere (title_jump.c). */
void TitleJump_Request(void);
/* Main_RunFrontendLoop disables requests on every entry to the title. */
void TitleJump_SetActive(int enabled);
/* Consume scripted requests at the actual presented frame, even at the
 * title or inside a fade. Execution still waits for TitleJump_Poll. */
void TitleJump_Frame(unsigned frame);
/* Called by Main_Loop between two mode runners; does not return when a jump
 * was requested. */
void TitleJump_Poll(void);
/* Game-ABI implementation; called with requests disabled at a safe point. */
void TitleJump_Execute(void);
#endif
