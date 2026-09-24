#ifndef MEMORIES_PC_PLATFORM_TITLE_JUMP_H
#define MEMORIES_PC_PLATFORM_TITLE_JUMP_H
/* Back to the title screen from anywhere (title_jump.c). */
void TitleJump_Request(void);
/* Called by Main_Loop between two mode runners; does not return when a jump
 * was requested. */
void TitleJump_Poll(void);
#endif
