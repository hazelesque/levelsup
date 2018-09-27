
/* vim: set ts=8 sts=4 sw=4 et filetype=c: */

#include <stdio.h>
#define N 10000

/*
 ***********************************************************
 * Program 1.1 Quick-find solution to connectivity problem *
 *                                                         *
 * Source:  Algorithms in C, 3rd Ed., Robert Sedgewick     *
 *          Chapter 1, Section 1.3, Program 1.1. (Page 12) *
 ***********************************************************
 */

main() {

    int i, p, q, t, id[N];

    for (i = 0; i < N; i++)
        id[i] = i;

    while (scanf("%d %d\n", &p, &q) == 2) {
        if (id[p] == id[q])
            continue;

        for (t = id[p], i = 0; i < N; i++)
            if (id[i] == t)
                id[i] = id[q];

        printf(" %d %d\n", p, q);

    } // while (scanf(...))

} // main()
