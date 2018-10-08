
/* vim: set ts=8 sts=4 sw=4 et filetype=c: */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/uio.h>

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
    char       *buf_writeptr;
    char       *buf_readptr;
    size_t      buf_len;
    size_t      buf_len_remaining;
    size_t      buf_len_remaining_read;
    size_t      page_size;
    bool        buf_dirty = false;
    int         pma_rv;

    int         name_len;
    char        name_temp[MAX_NAME_LEN];
    int         editcols[MAX_ED_LIMIT];
    int         ed, i, j, edit;
    char        c[MAX_ED_LIMIT];

    name_len = strlen(name);

    fprintf(stderr, "Max edit distance: %d, Name: \"%s\" (Length: %d)\n", max_ed, name, name_len);

    // Allocate a buffer, page-aligned, one page in size
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    buf_len = page_size;
    pma_rv = posix_memalign((void**)&buf, page_size, buf_len);
    buf_writeptr = buf;
    buf_readptr = buf;
    buf_len_remaining = buf_len;
    buf_len_remaining_read = buf_len;

    // posix_memalign(3) states "The value of errno is indeterminate after a call to posix_memalign()",
    // so we mustn't use perror().
    if (pma_rv) {
        fprintf(stderr, "[hamming] posix_memalign failed, returned %d.\n", pma_rv);
        abort();
    }

    // Zero buffer
    memset(buf_writeptr, 0, buf_len_remaining);

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
                    // No, emit candidate
                    for ( ; ; ) {
                        // Append candidate word + newline to buffer
                        int snp_rv = snprintf(buf_writeptr, buf_len_remaining, "%s\n", name_temp);
                        buf_dirty = true;

                        // Check for error
                        if (snp_rv < 0) {
                            perror("[hamming] snprintf");
                            exit(4);
                        }

                        // If truncation has occurred, i.e. only part of the candidate word
                        // was able to be written to the buffer, then:
                        //
                        // 1. Zero out the partial write in the buffer
                        // 2. Write the buffer out to fd, retrying until the entire buffer
                        //    has been written out
                        // 3. Clear (zero) the buffer
                        // 4. Reset pointers and counters
                        // 5. Go around the loop again in order to retry appending the
                        //    candidate word to the buffer

                        if ((size_t)(snp_rv * sizeof(char)) >= buf_len_remaining) {
                            // Fill remainder of buffer with null bytes
                            memset(buf_writeptr, 0, buf_len_remaining);

                            // Give away page(s) to pipe using vmsplice
                            struct iovec iov = {
                                .iov_base   = buf_readptr,
                                .iov_len    = buf_len_remaining_read,
                            };

                            while (buf_len_remaining_read) {
                                ssize_t vms_rv = vmsplice(fd, &iov, 1, SPLICE_F_GIFT);

                                if (vms_rv < 0) {
                                    switch (errno) {
                                        case EAGAIN:
                                            // Try again
                                            continue;
                                        default:
                                            perror("[hamming] vmsplice");
                                            exit(4);
                                    }
                                } else {
                                    buf_len_remaining_read -= vms_rv;
                                    iov.iov_base += vms_rv;
                                    iov.iov_len -= vms_rv;
                                }
                            }

                            // Free old buffer that we gave away
                            free(buf);

                            // New buffer
                            pma_rv = posix_memalign((void**)&buf, page_size, buf_len);
                            buf_writeptr = buf;
                            buf_readptr = buf;
                            buf_len_remaining = buf_len;
                            buf_len_remaining_read = buf_len;
                            buf_dirty = false;

                            // posix_memalign(3) states "The value of errno is indeterminate after a call to posix_memalign()",
                            // so we mustn't use perror().
                            if (pma_rv) {
                                fprintf(stderr, "[hamming] posix_memalign failed, returned %d.\n", pma_rv);
                                abort();
                            }

                            // Zero buffer
                            memset(buf_writeptr, 0, buf_len_remaining);

                            // Retry writing candidate word
                            continue;

                        } else {
                            // Deliberately update pointer to point at location of '\0'
                            buf_len_remaining -= (size_t)(snp_rv * sizeof(char));
                            buf_writeptr += snp_rv;

                            // Candidate word was written OK, no need to retry, break out of loop
                            break;
                        }
                    }

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

    // Write partially-full page to pipe before freeing it
    if (buf_dirty) {
        // Fill remainder of buffer with null bytes
        memset(buf_writeptr, 0, buf_len_remaining);

        // Give away page(s) to pipe using vmsplice
        struct iovec iov = {
            .iov_base   = buf_readptr,
            .iov_len    = buf_len_remaining_read,
        };

        while (buf_len_remaining_read) {
            ssize_t vms_rv = vmsplice(fd, &iov, 1, SPLICE_F_GIFT);

            if (vms_rv < 0) {
                switch (errno) {
                    case EAGAIN:
                        // Try again
                        continue;
                    default:
                        perror("[hamming] vmsplice");
                        exit(4);
                }
            } else {
                buf_len_remaining_read -= vms_rv;
                iov.iov_base += vms_rv;
                iov.iov_len -= vms_rv;
            }
        }
    }

    // Clean up
    if (buf) free(buf);

}

void catlines(int fd) {
    char       *buf;
    char       *buf_writeptr;
    char       *buf_readptr;
    size_t      buf_len;
    size_t      buf_len_remaining;
    size_t      buf_len_remaining_read;
    size_t      page_size;
    size_t      chars_read;

    // Allocate a buffer, page-aligned, one page in size
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    buf_len = page_size;
    int pma_rv = posix_memalign((void**)&buf, page_size, buf_len);
    buf_writeptr = buf;
    buf_readptr = buf;
    buf_len_remaining = buf_len;
    buf_len_remaining_read = buf_len;

    // posix_memalign(3) states "The value of errno is indeterminate after a call to posix_memalign()",
    // so we mustn't use perror().
    if (pma_rv) {
        fprintf(stderr, "[hamming] posix_memalign failed, returned %d.\n", pma_rv);
        abort();
    }

    // Zero buffer
    memset(buf_writeptr, 0, buf_len_remaining);

    for ( ; ; ) {
        ssize_t rd_rv = read(fd, buf_writeptr, buf_len_remaining);

        if (rd_rv < 0) {
            switch (errno) {
                case EINTR:
                case EAGAIN:
                    // Try again
                    continue;
                default:
                    perror("[catlines] read");
                    exit(4);
            }
        } else {
            buf_writeptr += (rd_rv / sizeof(char));
            buf_len_remaining -= ((rd_rv / sizeof(char))  * sizeof(char));
        }

        // If there's free buffer left, and we've not reached EOF, keep reading
        if(buf_len_remaining && (rd_rv != 0)) continue;

        // Adjust buf_len_remaining_read so that we don't write nulls to the terminal
        for ( ; buf_len_remaining_read >= sizeof(char); ) {
            if (buf_readptr[(buf_len_remaining_read/sizeof(char))-1] == '\0') {
                buf_len_remaining_read -= sizeof(char);
            } else {
                break;
            }
        }

        // Start writing to stdout
        for (; buf_len_remaining_read > 0; ) {
            ssize_t wr_rv = write(fileno(stdout), buf_readptr, buf_len_remaining_read);

            if (wr_rv < 0) {
                switch (errno) {
                    case EINTR:
                    case EAGAIN:
                        // Try again
                        continue;
                    default:
                        perror("[catlines] write");
                        exit(4);
                }
            } else {
                buf_readptr += (wr_rv / sizeof(char));
                buf_len_remaining_read -= ((wr_rv / sizeof(char)) * sizeof(char));
            }
        }

        // Did we reach EOF?
        if (rd_rv == 0) break;

        // Clear and go around
        buf_writeptr = buf;
        buf_readptr = buf;
        buf_len_remaining = buf_len;
        buf_len_remaining_read = buf_len;

        // Zero buffer
        memset(buf_writeptr, 0, buf_len_remaining);
    }

    // Clean up
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
