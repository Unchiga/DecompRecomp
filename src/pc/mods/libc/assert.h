/* A mod's <assert.h> (README.md): a failed assertion names itself on stderr
 * and stops the game. Included again, it follows NDEBUG afresh. */
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
__attribute__((noreturn)) void __assert_fail(const char *assertion, const char *file, unsigned line,
                                             const char *function);
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif
