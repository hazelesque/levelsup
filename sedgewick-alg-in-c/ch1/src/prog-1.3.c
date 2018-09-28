
/* vim: set ts=8 sts=4 sw=4 et filetype=c: */

#include <stdio.h>
#define N 10000

/*
 ***************************************************************
 * Program 1.3 Weighted quick-union                            *
 *                                                             *
 * Source(s):   Algorithms in C, 3rd Ed., Robert Sedgewick     *
 *              Chapter 1, Section 1.3, Program 1.3. (Page 17) *
 ***************************************************************
 */

main() {

    int i, j, p, q, t, id[N], sz[N];

    for (i = 0; i < N; i++) {
        id[i] = i;
        sz[i] = 1;
    } // for (i...)

    while (scanf("%d %d\n", &p, &q) == 2) {
        // Follow links until we find the set representative, i for p, and j for q
        for (i = p; i != id[i]; i = id[i]) ;
        for (j = q; j != id[j]; j = id[j]) ;

        // Check if p and q have the same set representative to see if they're in the same set...
        //
        // If they are, then we discard this input and move on.
        if (i == j) continue;

        // We made it this far, so replace the sets with i and j as their representatives
        // with the union of those sets.

        // Choose j as the representative of the new set if the set currently represented by j
        // is strictly the larger of the two sets, otherwise choose i as the representative of
        // the new set - and reparent accordingly.
        if (sz[i] < sz[j]) {
            // Reparent the tree with root i as a subtree under j
            id[i] = j;
            sz[j] += sz[i];
        } else {
            // Reparent the tree with root j as a subtree under i
            id[j] = i;
            sz[i] += sz[j];
        }

        // Emit this connection, it is part of the spanning tree
        printf(" %d %d\n", p, q);

    } // while (scanf(...))

} // main()
