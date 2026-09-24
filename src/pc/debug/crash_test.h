#ifndef MEMORIES_PC_DEBUG_CRASH_TEST_H
#define MEMORIES_PC_DEBUG_CRASH_TEST_H
/* MEMORIES_CRASH_TEST: failures on purpose (crash_test.c). */
void CrashTest_Init(void);
/* At every VSync: the failure, once its frame has come. */
void CrashTest_Frame(void);
/* Set by the "tickhang" test: the clock's tick spins. */
extern volatile int CrashTest_TickHang;

#endif
