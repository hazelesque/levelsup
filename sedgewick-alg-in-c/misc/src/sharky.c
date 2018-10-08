
/* vim: set ts=8 sts=4 sw=4 et filetype=c: */

#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_NAME_LEN 50
#define MAX_ED_LIMIT 10

/*
 ***************************************************************
 * sharky.c     Calculate alternative usernames                *
 *                                                             *
 ***************************************************************
 */

// Usage: $0 [max edit distance] [name] [dictionary file]

void hamming(int max_ed, char *name, int fd) {
    char       *buf;
    size_t      buf_len;
    size_t      page_size;
    int         name_len;
    char        name_temp[MAX_NAME_LEN];
    int         editcols[MAX_ED_LIMIT];
    int         ed, i, j, edit;
    char        c[MAX_ED_LIMIT];
    FILE       *f;

    // Convert fd to FILE* with fdopen
    if ((f = fdopen(fd, "w")) == NULL) {
        perror("[hamming] fdopen");
        exit(4);
    }

    name_len = strlen(name);

    fprintf(stderr, "Max edit distance: %d, Name: \"%s\" (Length: %d)\n", max_ed, name, name_len);

    // Allocate a buffer, page-aligned, one page in size
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    buf_len = page_size;
    int pma_rv = posix_memalign((void**)&buf, page_size, buf_len);

    // posix_memalign(3) states "The value of errno is indeterminate after a call to posix_memalign()",
    // so we mustn't use perror().
    if (pma_rv) {
        fprintf(stderr, "[hamming] posix_memalign failed, returned %d.\n", pma_rv);
        abort();
    }

    // Hamming distance
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
                    fprintf(f, "%s\n", name_temp);

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

    // Clean up
    if (buf) free(buf);

    // Close FILE for pipe fd
    fclose(f);

}

void catlines(int fd) {
    char       *buf;
    size_t      buf_len = ((sizeof(char)) * (MAX_NAME_LEN+1));
    size_t      chars_read;
    FILE       *f;

    // Convert fd to FILE* with fdopen
    if ((f = fdopen(fd, "r")) == NULL) {
        perror("[catlines] fdopen");
        exit(4);
    }

    // Allocate buffer
    if ((buf = (char *) malloc(buf_len)) == NULL) {
        perror("malloc");
        exit(4);
    }

    // Read lines and re-emit them to stdout
    //
    // getline(...) will realloc as necessary if buffer is too small
    // and hence needs a pointer to our char array pointer so it can
    // update our pointer for us, and likewise for buf_len.
    while ((chars_read = getline(&buf, &buf_len, f)) != -1) {
        fprintf(stdout, "%s", buf);
    }

    // Clean up
    fclose(f);
    if (buf) free(buf);
}

int main(int argc, char *argv[]) {
    int     fd[2], max_ed;
    char   *name;
    pid_t   childpid_dictcheck;
    int     status_dictcheck;

    // Check args
    if (argc != 4) {
        fprintf(stderr, "Unexpected number of arguments: %d. Exiting.\n", argc - 1);
        return 3;
    }

    sscanf(argv[1], "%d", &max_ed);
    name = argv[2];

    // Create pipe
    //
    if ((pipe(fd)) == -1) {
        perror("pipe");
        exit(4);
    }

    // Fork
    //
    if ((childpid_dictcheck = fork()) == -1) {
        perror("fork");
        exit(4);
    }

    if (0 == childpid_dictcheck) {
        // Child closes input end of pipe
        close(fd[1]);

        catlines(fd[0]);

        // Tidy up and exit
        close(fd[0]);
        exit(0);
    } else {
        // Parent closes output end of pipe
        close(fd[0]);

        hamming(max_ed, name, fd[1]);

        // Tidy up and wait for child to exit
        close(fd[1]);
        waitpid(childpid_dictcheck, &status_dictcheck, 0);

        if (status_dictcheck != 0) {
            fprintf(stderr, "Child %d exited with status %d!\n", childpid_dictcheck, status_dictcheck);
            exit(5);
        }

        exit(0);
    }

} // main()
