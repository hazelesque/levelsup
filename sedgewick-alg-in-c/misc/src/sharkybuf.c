
/* vim: set ts=8 sts=4 sw=4 et filetype=c: */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/uio.h>
// --
//#include <sys/types.h>
//#include <sys/wait.h>

#include "sharkybuf.h"

/*
 ***************************************************************
 * sharkybuf.c  Buffer handling utility routines               *
 *                                                             *
 ***************************************************************
 */


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

void sb_create_posix_memalign(struct sharkybuf *sb, size_t len) {
    /*
     * Create a pagesize-aligned buffer, allocated by posix_memalign(3).
     *
     * Asserts:
     *      sb is not null
     *      len is an exact multiple of system page size
     */
    void       *addr;
    int         pma_rv;

    // Pre-flight checks
    assert(sb != NULL);
    assert((len % (size_t)sysconf(_SC_PAGESIZE)) == 0);

    // Perform allocation
    pma_rv = posix_memalign((void**)&addr, (size_t)sysconf(_SC_PAGESIZE), len);

    // posix_memalign(3) states "The value of errno is indeterminate after a call to posix_memalign()",
    // so we mustn't use perror().
    if (pma_rv) {
        fprintf(stderr, "[sb_create_posix_memalign] posix_memalign failed, returned %d.\n", pma_rv);
        exit(4);
    }

    // Zero buffer
    memset(addr, 0, len);

    // Populate struct
    sb->strategy = SHARKYBUF_STRATEGY_POSIX_MEMALIGN;
    sb->addr = addr;
    sb->len = len;
    sb->dirty = false;

    // Initialize "writer head" position
    sb->writer_ptr = (char*)addr;
    sb->writer_len_remaining = len;
}

void sb_create_malloc(struct sharkybuf *sb, size_t len) {
    /*
     * Create a buffer of size len, allocated by malloc(3).
     *
     * Asserts:
     *      sb is not null
     *      len > 0
     */
    void       *addr;

    // Pre-flight checks
    assert(sb != NULL);
    assert(len > 0);

    // Perform allocation
    addr = malloc(len);

    if (addr == NULL) {
        perror("[sb_create_malloc] malloc");
        exit(4);
    }

    // Zero buffer
    memset(addr, 0, len);

    // Populate struct
    sb->strategy = SHARKYBUF_STRATEGY_MALLOC;
    sb->addr = addr;
    sb->len = len;
    sb->dirty = false;

    // Initialize "writer head" position
    sb->writer_ptr = (char*)addr;
    sb->writer_len_remaining = len;
}

void sb_realloc(struct sharkybuf *sb, size_t new_len) {
    /*
     * Realloc(3) a buffer previously allocated by malloc(3),
     * to have a new size of new_len
     *
     * Asserts:
     *      sb is not null
     *      sb->strategy is SHARKYBUF_STRATEGY_MALLOC
     *      new_len > 0
     *      new_len > sb->len
     */
    void       *new_addr;
    void       *old_addr;
    size_t      old_len;
    ptrdiff_t   addr_delta;
    size_t      len_delta;

    // Pre-flight checks
    assert(sb != NULL);
    assert(sb->strategy == SHARKYBUF_STRATEGY_MALLOC);
    assert(new_len > 0);
    assert(new_len > sb->len);

    // Prep
    old_addr = sb->addr;
    old_len = sb->len;
    len_delta = new_len - old_len;

    // Perform allocation
    new_addr = realloc(old_addr, new_len);

    if (new_addr == NULL) {
        perror("[sb_realloc] realloc");
        exit(4);
    }

    // Zero new part of buffer
    memset((new_addr + old_len), 0, (new_len - old_len));

    // Calculate difference in addresses
    addr_delta = new_addr - old_addr;

    // Update struct
    sb->addr = new_addr;
    sb->len = new_len;

    // Update "writer head"
    sb->writer_ptr += addr_delta;
    sb->writer_len_remaining += len_delta;
}

void sb_dispose_munmap_(struct sharkybuf *sb) {
    /*
     * Dispose of buffer we allocated previously with mmap(2)
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

void sb_dispose_free_(struct sharkybuf *sb) {
    /*
     * Dispose of buffer we allocated previously with either
     * posix_memalign(3) or malloc(3)
     *
     * Asserts:
     *      sb is not NULL
     *      sb->addr is not NULL
     *      sb->strategy is SHARKYBUF_STRATEGY_POSIX_MEMALIGN
     *                   or SHARKYBUF_STRATEGY_MALLOC
     */

    // Pre-flight checks
    assert(sb != NULL);
    assert(sb->addr != NULL);
    assert((sb->strategy == SHARKYBUF_STRATEGY_POSIX_MEMALIGN) ||
           (sb->strategy == SHARKYBUF_STRATEGY_MALLOC));

    // Actually free the page(s)
    free(sb->addr);

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
            sb_dispose_munmap_(sb);
            break;
        case SHARKYBUF_STRATEGY_POSIX_MEMALIGN:
        case SHARKYBUF_STRATEGY_MALLOC:
            sb_dispose_free_(sb);
            break;
        default:
            fprintf(stderr, "[sb_dispose] invalid strategy %d.\n", sb->strategy);
            abort();
    }
}

void sb_wipe(struct sharkybuf *sb) {
    /*
     * Wipe buffer, reset "writer head" position and clear dirty flag
     *
     * Asserts:
     *      sb is not NULL
     *      sb->addr is not NULL
     */

    // Pre-flight checks
    assert(sb != NULL);
    assert(sb->addr != NULL);

    // Zero buffer
    memset(sb->addr, 0, sb->len);

    // Initialize "writer head" position
    sb->writer_ptr = (char*)(sb->addr);
    sb->writer_len_remaining = sb->len;

    // Reset dirty flag
    sb->dirty = false;
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

int sb_recvbuf_read(struct sharkybuf *sb, int fd) {
    /*
     * Read from pipe fd until either buffer is full or EOF is reached
     *
     * Returns:
     *      0 if we filled our buffer without reaching EOF
     *      1 if we reached EOF
     *
     * Asserts:
     *      sb is not NULL
     *      sb->addr is not NULL
     */

    ssize_t         rd_rv;

    // Pre-flight checks
    assert(sb != NULL);
    assert(sb->addr != NULL);

    // Read
    while (true) {
        rd_rv = read(fd, sb->writer_ptr, sb->writer_len_remaining);

        // Check if we actually managed to read anything,
        // and handle retries and errors
        if (rd_rv < 0) {
            switch (errno) {
                case EINTR:
                case EAGAIN:
                    // Try again
                    continue;
                default:
                    perror("[sb_recvbuf_read] read");
                    exit(4);
            }
        } else {
            sb->dirty = true;
            sb->writer_ptr += (rd_rv / sizeof(char));
            sb->writer_len_remaining -= ((rd_rv / sizeof(char))  * sizeof(char));
        }

        // Check if we've reached EOF
        if(rd_rv == 0) return 1;

        // Check if we're out of buffer
        if(!(sb->writer_len_remaining > 0)) return 0;
    }
}

void sb_sendbuf_vmsplice(struct sharkybuf *sb, int fd) {
    /*
     * Send content of buffer sb to pipe fd, then dispose of buffer
     * and replace with a new one as we are not allowed to touch these
     * pages once we've given them away with vmsplice(... SPLICE_F_GIFT)
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

void sb_buf_to_stdout(struct sharkybuf *sb) {
    /*
     * Send content of buffer sb to stdout using write(2), except for
     * any null bytes at the end of the buffer 
     *
     * Asserts:
     *      sb is not NULL
     *      sb->addr is not NULL
     */

    char           *reader_ptr;
    size_t          reader_len_remaining;
    ssize_t         wr_rv;

    // Pre-flight checks
    assert(sb != NULL);
    assert(sb->addr != NULL);

    // Setup
    reader_ptr = sb->addr;
    reader_len_remaining = sb->len;

    // Adjust reader_len_remaining so that we don't write nulls to the terminal
    for ( ; reader_len_remaining >= sizeof(char); ) {
        if (reader_ptr[(reader_len_remaining/sizeof(char))-1] == '\0') {
            reader_len_remaining -= sizeof(char);
        } else {
            break;
        }
    }

    // Start writing to stdout
    while (reader_len_remaining > 0) {
        wr_rv = write(fileno(stdout), reader_ptr, reader_len_remaining);

        if (wr_rv < 0) {
            switch (errno) {
                case EINTR:
                case EAGAIN:
                    // Try again
                    continue;
                default:
                    perror("[sb_buf_to_stdout] write");
                    exit(4);
            }
        } else {
            reader_ptr += (wr_rv / sizeof(char));
            reader_len_remaining -= ((wr_rv / sizeof(char)) * sizeof(char));
        }
    }
}
