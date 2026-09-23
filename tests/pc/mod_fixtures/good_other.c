/* The second file of the good fixture: build_mod.py merges them into one. */
int other_file(int x)
{
    return x * x;
}

/* More than a page of stack, touched at the far end first: on Windows this
 * only works if each page is probed on the way down (build_mod.py). */
int deep(int n)
{
    volatile char block[20000];
    block[0] = (char)n;
    block[sizeof(block) - 1] = (char)n;
    return block[0] + block[sizeof(block) - 1];
}
