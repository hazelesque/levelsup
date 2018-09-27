
/* vim: set ts=8 sts=4 sw=4 et filetype=c: */

#include <stdio.h>
#define N 10000

/*
 ***************************************************************
 * Program 1.2 Quick-union solution to connectivity problem    *
 *                                                             *
 * Source(s):   Algorithms in C, 3rd Ed., Robert Sedgewick     *
 *              Chapter 1, Section 1.3, Program 1.1. (Page 12) *
 *              Chapter 1, Section 1.3, Program 1.2. (Page 15) *
 ***************************************************************
 */

main() {

    int i, p, q, t, id[N];

    for (i = 0; i < N; i++)
        id[i] = i;

    while (scanf("%d %d\n", &p, &q) == 2) {
        for (i = p; i != id[i]; i = id[i]) ;
        for (j = q; j != id[j]; j = id[j]) ;
        if (i == j) continue;
        id[i] = j;
        printf(" %d %d\n", p, q);

    } // while (scanf(...))

} // main()
