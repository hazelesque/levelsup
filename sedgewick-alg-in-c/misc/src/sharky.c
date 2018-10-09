
/* vim: set ts=8 sts=4 sw=4 et filetype=c: */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
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


#define SHARKYBUF_STRATEGY_UNALLOCATED      0
#define SHARKYBUF_STRATEGY_MMAP             1
#define SHARKYBUF_STRATEGY_POSIX_MEMALIGN   2

struct sharkybuf {
    /* buffer information */
    int         strategy;
    void       *addr;
    size_t      len;
    bool        dirty;

    /* position of writer head */
    char       *writer_ptr;
    size_t      writer_len_remaining;
};

void sb_create_mmap(struct sharkybuf *sb, size_t len) {
    /*
     * Create a buffer, allocated by mmap(...) with MAP_ANONYMOUS flag
     *
     * Asserts:
     *      sb is not null
     *      len is an exact multiple of system page size
     */
    void       *addr;

    // Pre-flight checks
    assert(sb != NULL);
    assert((len % (size_t)sysconf(_SC_PAGESIZE)) == 0);

    // Perform mmap
    addr = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (addr == MAP_FAILED) {
        perror("[sb_create_mmap] mmap");
        exit(4);
    }

    // Zero buffer
    memset(addr, 0, len);

    // Populate struct
    sb->strategy = SHARKYBUF_STRATEGY_MMAP;
    sb->addr = addr;
    sb->len = len;
    sb->dirty = false;

    // Initialize "writer head" position
    sb->writer_ptr = (char*)addr;
    sb->writer_len_remaining = len;
}

void sb_dispose_mmap_(struct sharkybuf *sb) {
    /*
     * Dispose of buffer we allocated previously
     *
     * Asserts:
     *      sb is not NULL
     *      sb->strategy is SHARKYBUF_STRATEGY_MMAP
     */

    // Pre-flight checks
    assert(sb != NULL);
    assert(sb->strategy == SHARKYBUF_STRATEGY_MMAP);

    // Actually unmap the memory-mapped page(s)
    munmap(sb->addr, sb->len);

    // Clear struct
    sb->strategy = SHARKYBUF_STRATEGY_UNALLOCATED;
    sb->addr = NULL;
    sb->len = 0;
    sb->dirty = false;
    sb->writer_ptr = NULL;
    sb->writer_len_remaining = 0;
}

void sb_dispose(struct sharkybuf *sb) {
    switch (sb->strategy) {
        case SHARKYBUF_STRATEGY_MMAP:
            sb_dispose_mmap_(sb);
            break;
        default:
            fprintf(stderr, "[sb_dispose] invalid strategy %d.\n", sb->strategy);
            abort();
    }
}

int sb_append_line_or_zeroes(struct sharkybuf *sb, char *line) {
    /*
     * Append value of line followed by '\n' to buffer if there is
     * enough room. On detecting that snprintf has truncated the text
     * to append, zero out the remainder of the buffer.
     *
     * Returns:
     *      0 on success
     *      1 if remaining buffer was insufficient, and was zeroed
     *
     * Asserts:
     *      sb is not NULL
     *      sb->addr is not NULL
     *      line is not NULL
     */
    int snp_rv;

    // Pre-flight checks
    assert(sb != NULL);
    assert(sb->addr != NULL);
    assert(line != NULL);

    // Append string + newline to buffer
    //
    // Return value snp_rv is length of text that *ought* to have been written,
    // NOT INCLUDING the '\0' byte.
    snp_rv = snprintf(sb->writer_ptr, (sb->writer_len_remaining / sizeof(char)), "%s\n", line);
    sb->dirty = true;

    if (snp_rv < 0) {
        perror("[sb_append_line_or_zeroes] snprintf");
        exit(4);
    }

    if ((size_t)(snp_rv * sizeof(char)) >= sb->writer_len_remaining) {
        memset(sb->writer_ptr, 0, sb->writer_len_remaining);
        return 1;
    } else {
        // Deliberately update pointer to point at location of '\0',
        // as we'll overwrite that with a new null-terminated string
        sb->writer_len_remaining -= (size_t)(snp_rv * sizeof(char));
        sb->writer_ptr += snp_rv;
        return 0;
    }

}

void sb_sendbuf_vmsplice(struct sharkybuf *sb, int fd) {
    /*
     * Send content of buffer sb to pipe fd, then dispose of buffer
     * and replace with a new one.
     *
     * Asserts:
     *      sb is not NULL
     *      sb->addr is not NULL
     *      sb->strategy is SHARKYBUF_STRATEGY_MMAP
     */

    size_t          len;
    struct iovec    iov;
    size_t          reader_len_remaining;
    ssize_t         vms_rv;

    // Pre-flight checks
    assert(sb != NULL);
    assert(sb->addr != NULL);
    assert(sb->strategy == SHARKYBUF_STRATEGY_MMAP);

    // Setup
    reader_len_remaining = sb->len;
    iov.iov_base = sb->addr;
    iov.iov_len = reader_len_remaining;

    // Transfer
    while (reader_len_remaining) {
        vms_rv = vmsplice(fd, &iov, 1, SPLICE_F_GIFT);

        if (vms_rv < 0) {
            switch (errno) {
                case EAGAIN:
                    // Try again
                    continue;
                default:
                    perror("[sb_sendbuf_vmsplice] vmsplice");
                    exit(4);
            }
        } else {
            reader_len_remaining -= vms_rv;
            iov.iov_base += vms_rv;
            iov.iov_len -= vms_rv;
        }
    }

    // Dispose and replace
    len = sb->len;
    sb_dispose(sb);
    sb_create_mmap(sb, len);
}

void hamming(int max_ed, char *name, int fd) {
    struct sharkybuf    sbuf;
    size_t              buf_len;
    size_t              page_size;

    int                 name_len;
    char                name_temp[MAX_NAME_LEN];
    int                 editcols[MAX_ED_LIMIT];
    int                 ed, i, j, edit;
    char                c[MAX_ED_LIMIT];

    name_len = strlen(name);

    fprintf(stderr, "Max edit distance: %d, Name: \"%s\" (Length: %d)\n", max_ed, name, name_len);

    // Allocate a buffer, page-aligned, one page in size
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    buf_len = page_size;

    sb_create_mmap(&sbuf, buf_len);

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
                        int append_rv = sb_append_line_or_zeroes(&sbuf, name_temp);

                        // If truncation has occurred, i.e. only part of the candidate word
                        // was able to be written to the buffer and was subsequently
                        // zeroed, then:
                        //
                        // 1. Write the buffer out to fd, retrying until the entire buffer
                        //    has been written out
                        // 2. Clear (zero) the buffer
                        // 3. Reset pointers and counters
                        // 4. Go around the loop again in order to retry appending the
                        //    candidate word to the buffer

                        if (append_rv != 0) {
                            // Give away page(s) to pipe using vmsplice, and receive details of
                            // new page into struct at &sbuf.
                            sb_sendbuf_vmsplice(&sbuf, fd);

                            // Retry writing candidate word
                            continue;

                        } else {
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
    if (sbuf.dirty) {
        // Give away page(s) to pipe using vmsplice, and receive details of
        // new page into struct at &sbuf.
        sb_sendbuf_vmsplice(&sbuf, fd);
    }

    // Clean up
    sb_dispose(&sbuf);

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
        fprintf(stderr, "[catlines] posix_memalign failed, returned %d.\n", pma_rv);
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
