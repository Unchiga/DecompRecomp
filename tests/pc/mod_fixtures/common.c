/* A tentative definition, which -fcommon makes a COMMON symbol. */
int shared_counter;
int run(void) { return shared_counter; }
