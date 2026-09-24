#ifndef MEMORIES_PC_PLATFORM_AI_TRACE_H
#define MEMORIES_PC_PLATFORM_AI_TRACE_H
/* MEMORIES_AI_TRACE / MEMORIES_AI_YIELD (ai_trace.c), from Memories_VSync. */
void AiTrace_Frame(unsigned frame);
int AiTrace_VSync1(int elapsed, void *caller);
#endif
