
/* vim: set ts=8 sts=4 sw=4 et filetype=c: */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define MAX_NAME_LEN 50
#define MAX_ED_LIMIT 10

/*
 ***************************************************************
 * sharky.c     Calculate alternative usernames                *
 *                                                             *
 ***************************************************************
 */

// Usage: $0 [max edit distance] [name] [dictionary file]

int main(int argc, char *argv[]) {
    int max_ed;
    char *name;
    int name_len;
    char name_temp[MAX_NAME_LEN];
    int editcols[MAX_ED_LIMIT];

    // Check args

    if (argc != 4) {
        fprintf(stderr, "Unexpected number of arguments: %d. Exiting.\n", argc - 1);
        return 3;
    }

    sscanf(argv[1], "%d", &max_ed);
    name = argv[2];
    name_len = strlen(name);

    fprintf(stderr, "Max edit distance: %d, Name: \"%s\" (Length: %d)\n", max_ed, name, name_len);

    int ed, i, j, edit;
    char c[MAX_ED_LIMIT];

    // Edit distance
    for (ed = 1; ed <= max_ed; ed++) {
        // Initialise state for editcols
        i = -1;
        for (j = (ed - 1); j >= 0; ) {
            editcols[j] = j;
            j--;
        }

        // Choose columns
        for ( ; ; ) {
            // Is it time to set the columns?
            if (i >= 0) {
                if (editcols[i] < (name_len - (ed - i))) {
                    editcols[i]++;

                    // Set following columns incrementally
                    i++;
                    for ( ; i < ed; i++) {
                        editcols[i] = editcols[i-1]+1;
                    }
                    // Set i=-1 to stop further column cycling for now
                    i = (-1);
                } else {
                    // Seek backwards until we find an edit position whose column isn't at max
                    i--;
                    if (i < 0) break;
                    continue;
                }
            }

            // Start from clean copy of string
            strncpy(name_temp, name, MAX_NAME_LEN);

            // Initialise state for edits
            edit = 0;
            for (j = (ed - 1); j >= 0; ) {
                c[j] = 'a';
                j--;
            }

            // Perform edits
            for (; ;) {
                // Do this edit
                name_temp[editcols[edit]] = c[edit];

                // More columns to do this round?
                if (edit < (ed - 1)) {
                    // Yes, do next...
                    edit++;
                    continue;
                } else if (edit == (ed - 1)) {
                    // No, print candidate
                    printf("%s\n", name_temp);

                    // Select next set of chars
                    for (j = (ed - 1); j >= 0; ) {
                        if (c[j] < 'z') {
                            c[j]++;
                            break;
                        } else {
                            c[j] = 'a';
                            j--;
                            continue;
                        }
                    }

                    // Check if we ran out of values for this set of columns
                    if (j < 0) break;

                    // Go round again, applying new edits
                    edit = 0;
                    continue;
                }
            }
            i = (ed - 1);
        }

    } // for ed

} // main()
