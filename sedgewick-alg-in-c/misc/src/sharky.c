
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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/uio.h>

#define MAX_NAME_LEN 50
#define MAX_ED_LIMIT 10
#define SKIPLIST_MAX_LEVELS 30
#define SKIPLIST_UNROLLED_DATAITEMS 5

#define DEBUG_MSG(format, ...) fprintf(stderr, format, __VA_ARGS__)

/*
 ***************************************************************
 * sharky.c     Calculate alternative usernames                *
 *                                                             *
 ***************************************************************
 */

// Usage: $0 <max edit distance> <name> [dictionary file]


#define SHARKYBUF_STRATEGY_UNALLOCATED      0
#define SHARKYBUF_STRATEGY_MMAP             1
#define SHARKYBUF_STRATEGY_POSIX_MEMALIGN   2
#define SHARKYBUF_STRATEGY_MALLOC           3

struct sharkybuf {
    /* buffer information */
    int         strategy;
    void       *addr;
    size_t      len;

    /* clean/dirty status is only guaranteed if buffer is only ever written
     * to/cleared by sb_* functions
     */
    bool        dirty;

    /* position of writer head */
    char       *writer_ptr;
    size_t      writer_len_remaining;
};

struct skiplist_node {
    /* skiplist node with support for multiple links and multiple
     * data item pointers
     */
    int         linkptr_ct;
    int         dataptr_ct;
    void       *ptr[]; /* flexible array of type void* */
};

struct sdict {
    /* dictonary text */
    int                     dict_fd;
    char                   *dict_addr;
    size_t                  dict_len;
    /* dictionary index */
    struct sharkybuf        sl_sbuflist_sbuf;       // Memory where sl_sbuflist is stored
    int                     sl_sbuflist_entry_ct;   // Number of sbufs in sl_sbuflist
    struct sharkybuf       *sl_sbuflist;            // List of sbufs, each representing memory
                                                    //     for storing skiplist nodes in
    struct skiplist_node   *sl_headnode;            // Pointer to head skiplist node
    struct skiplist_node   *sl_sentinel;            // Pointer to skiplist sentinel node
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

void hamming(int max_ed, char *name, int fd) {
    /*
     * Generate all possible permutations of the string name where up to
     * max_ed columns have been overwritten with a character from a-z,
     * and then write them to pipe fd in buffer-sized chunks, separated
     * by newlines.
     *
     * Asserts:
     *      strlen(name) <= (MAX_NAME_LEN - 1)
     *      max_ed <= MAX_ED_LIMIT
     */
    struct sharkybuf    sbuf;
    size_t              buf_len;
    size_t              page_size;

    int                 name_len;
    char                name_temp[MAX_NAME_LEN];
    int                 editcols[MAX_ED_LIMIT];
    int                 ed, i, j, edit;
    char                c[MAX_ED_LIMIT];

    // Pre-flight checks
    assert(strlen(name) <= (MAX_NAME_LEN - 1));
    assert(max_ed <= MAX_ED_LIMIT);

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
    /*
     * Read buffer-sized chunks from pipe fd and write back out to standard
     * output, truncating any null bytes from the end of the received buffer.
     */
    struct sharkybuf    sbuf;
    size_t              buf_len;
    size_t              page_size;

    // Allocate a buffer, page-aligned, one page in size
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    buf_len = page_size;
    sb_create_posix_memalign(&sbuf, buf_len);

    while (true) {
        int read_rv = sb_recvbuf_read(&sbuf, fd);

        // Write content of buffer to stdout
        sb_buf_to_stdout(&sbuf);

        // Wipe buffer and reset writer head
        sb_wipe(&sbuf);

        // Did we reach EOF?
        if (read_rv == 1) break;
    }

    // Clean up
    sb_dispose(&sbuf);
}

void sdict_sl_realloc(struct sdict *sd) {
    size_t          new_len;

    // Calculate new size
    new_len = sd->sl_sbuflist_sbuf.len * 2;

    // Request realloc
    sb_realloc(&(sd->sl_sbuflist_sbuf), new_len);

    // Update address of sd->sl_sbuflist
    sd->sl_sbuflist = sd->sl_sbuflist_sbuf.addr;
}

struct skiplist_node* sdict_sl_allocnode(struct sdict *sd, int linkptr_ct, int dataptr_ct) {
    size_t                  page_size;
    struct skiplist_node   *node_addr;
    size_t                  node_size;

    page_size = (size_t)sysconf(_SC_PAGESIZE);

    // Calculate size required
    node_size = sizeof(struct skiplist_node) + ((linkptr_ct + dataptr_ct) * sizeof(void*));

    // Check available space
    if (((sd->sl_sbuflist)[(sd->sl_sbuflist_entry_ct - 1)]).writer_len_remaining < node_size) {
        // Need another buffer...
        //
        // First, check if memory allocation for list of buffers needs enlarging...
        if (sd->sl_sbuflist_sbuf.writer_len_remaining < sizeof(struct sharkybuf)) {
            sdict_sl_realloc(sd);
        }

        // Assert that there is enough room now for another sharkybuf struct
        assert(sd->sl_sbuflist_sbuf.writer_len_remaining >= sizeof(struct sharkybuf));

        // Create new buffer
        page_size = (size_t)sysconf(_SC_PAGESIZE);
        sb_create_malloc(&((sd->sl_sbuflist)[sd->sl_sbuflist_entry_ct]), page_size);
        (sd->sl_sbuflist_entry_ct)++;
    }

    // Assert that there is enough room now for the new node
    assert(((sd->sl_sbuflist)[(sd->sl_sbuflist_entry_ct - 1)]).writer_len_remaining >= node_size);

    // Initialize skiplist node
    node_addr = (struct skiplist_node*)(((sd->sl_sbuflist)[(sd->sl_sbuflist_entry_ct - 1)]).writer_ptr);
    node_addr->linkptr_ct = linkptr_ct;
    node_addr->dataptr_ct = dataptr_ct;

    // Bump writer pointer
    ((sd->sl_sbuflist)[(sd->sl_sbuflist_entry_ct - 1)]).writer_ptr += (node_size / sizeof(char));
    ((sd->sl_sbuflist)[(sd->sl_sbuflist_entry_ct - 1)]).writer_len_remaining -= node_size;

    return node_addr;
}

void sdict_sl_init(struct sdict *sd) {
    /*
     * Initialize skiplist datastructure, allocating required buffers and
     * generating the head and sentinel nodes of the skiplist.
     *
     * Asserts:
     *          sd is not NULL
     */
    size_t              page_size;

    // Pre-flight checks
    assert(sd != NULL);

    // Allocate buffer to store skiplist buffer list in
    //
    // N.B. This buffer can be moved safely (e.g. when resized with
    // realloc), as there is only ever one pointer to it, and it is
    // only used to keep track of freeing buffers from the pool used
    // for the skiplist index.

    page_size = (size_t)sysconf(_SC_PAGESIZE);
    sb_create_malloc(&(sd->sl_sbuflist_sbuf), page_size);

    DEBUG_MSG("-DD- Allocated sl_sbuflist_sbuf, .addr=%p, .len=%d.\n", sd->sl_sbuflist_sbuf.addr, sd->sl_sbuflist_sbuf.len);

    sd->sl_sbuflist = (struct sharkybuf*)(sd->sl_sbuflist_sbuf.addr);
    sd->sl_sbuflist_entry_ct = 0;

    // Allocate buffer to store skiplist index in
    sb_create_malloc(&((sd->sl_sbuflist)[sd->sl_sbuflist_entry_ct]), page_size);
    (sd->sl_sbuflist_entry_ct)++;

    DEBUG_MSG("-DD- Allocated sl_sbuflist[%d], .addr=%p, .len=%d. sl_sbuflist_entry_ct is now %d.\n", (sd->sl_sbuflist_entry_ct - 1), (sd->sl_sbuflist)[(sd->sl_sbuflist_entry_ct - 1)].addr, (sd->sl_sbuflist)[(sd->sl_sbuflist_entry_ct - 1)].len, sd->sl_sbuflist_entry_ct);

    // Bump sd->sl_sbuflist_sbuf writer pointer
    sd->sl_sbuflist_sbuf.writer_ptr += (sizeof(struct sharkybuf) / sizeof(char));
    sd->sl_sbuflist_sbuf.writer_len_remaining -= sizeof(struct sharkybuf);

    // Initialize skiplist headnode
    sd->sl_headnode = sdict_sl_allocnode(sd, SKIPLIST_MAX_LEVELS, 0);

    DEBUG_MSG("-DD- Allocated sl_headnode at address %p.\n", sd->sl_headnode);

    // Initialise skiplist sentinel node
    sd->sl_sentinel = sdict_sl_allocnode(sd, 0, 0);

    DEBUG_MSG("-DD- Allocated sl_sentinel at address %p.\n", sd->sl_sentinel);

    // Initialize skiplist headnode pointers
    for (int i = 0; i < (sd->sl_headnode->linkptr_ct + sd->sl_headnode->dataptr_ct); i++) {
        sd->sl_headnode->ptr[i] = sd->sl_sentinel;
    }
}

void sdict_sl_destruct(struct sdict *sd) {
    /*
     * Free all buffers allocated for the skiplist, and clear
     * skiplist-related struct members
     *
     * Asserts:
     *          sd is not NULL
     */

    // Pre-flight checks
    assert(sd != NULL);

    // Dispose of all sbufs in the pool
    while(sd->sl_sbuflist_entry_ct > 0) {
        sb_dispose(&((sd->sl_sbuflist)[(sd->sl_sbuflist_entry_ct - 1)]));
        (sd->sl_sbuflist_entry_ct)--;
    }

    // Dispose of sbuf used to store pool sbuf structs
    sb_dispose(&(sd->sl_sbuflist_sbuf));

    // Clear skiplist struct entries
    sd->sl_sentinel = NULL;
    sd->sl_headnode = NULL;
    sd->sl_sbuflist = NULL;
}

void sdict_open(struct sdict *sd, char *dictpath) {
    /*
     * Open dictionary at dictpath, mmap it, process it into a skiplist
     * data structure and store necessary information to access it in *sb.
     *
     * Asserts:
     *          sd is not NULL
     *          dictpath is not NULL
     */
    char               *dict_addr;
    int                 dict_fd;
    int                 fst_rv;
    struct stat         dict_statbuf;
    size_t              dict_len;

    // Pre-flight checks
    assert(sd != NULL);
    assert(dictpath != NULL);

    // Open
    dict_fd = open(dictpath, O_RDONLY);

    if (dict_fd == -1) {
        perror("[sdict_open] open");
        exit(4);
    }

    // Get size
    fst_rv = fstat(dict_fd, &dict_statbuf);

    if (fst_rv == -1) {
        perror("[sdict_open] fstat");
        exit(4);
    }

    dict_len = dict_statbuf.st_size;

    // Mmap
    dict_addr = mmap(NULL, dict_len, PROT_READ, MAP_PRIVATE, dict_fd, 0);

    if (dict_addr == MAP_FAILED) {
        perror("[sdict_open] mmap");
        exit(4);
    }

    // Populate struct
    sd->dict_addr = dict_addr;
    sd->dict_fd = dict_fd;
    sd->dict_len = dict_len;

    // Initialize skiplist
    sdict_sl_init(sd);

    // Populate skiplist from dictionary

}

void sdict_close(struct sdict *sd) {
    int         munmap_rv;
    int         close_rv;

    // Free buffers used by skiplist and by buffer pool
    sdict_sl_destruct(sd);

    // Munmap
    munmap_rv = munmap(sd->dict_addr, sd->dict_len);

    if (munmap_rv == -1) {
        perror("[sdict_close] munmap");
        exit(4);
    }

    // Close file
    close_rv = close(sd->dict_fd);

    if (close_rv == -1) {
        perror("[sdict_close] close");
        exit(4);
    }

    // Clear struct
    sd->dict_fd = 0;
    sd->dict_addr = NULL;
    sd->dict_len = 0;
}

void checkwords(int fd, char *dictpath) {
    /*
     * Read buffer-sized chunks from pipe fd containing zero or more newline-separated
     * candidate words followed by null bytes up to the end of the buffer, and write
     * those that appear in dictionary file dictpath to standard output.
     */
    struct sharkybuf    candw_sbuf;
    size_t              candw_buf_len;
    struct sdict        sd;
    size_t              page_size;
    int                 read_rv;

    // Get system page size
    page_size = (size_t)sysconf(_SC_PAGESIZE);

    // Read in dictionary
    sdict_open(&sd, dictpath);

    // Allocate buffer to receive candidate words
    candw_buf_len = page_size;
    sb_create_posix_memalign(&candw_sbuf, candw_buf_len);

    // Read buffer-size chunks of candidate words from fd, and check against dictionary
    while (true) {
        read_rv = sb_recvbuf_read(&candw_sbuf, fd);

        // Check words and emit those that appear in the dictionary to standard output
        //XXXX;

        // Wipe buffer and reset writer head
        sb_wipe(&candw_sbuf);

        // Did we reach EOF?
        if (read_rv == 1) break;
    }

    // Close dictionary
    sdict_close(&sd);

    // Clean up
    sb_dispose(&candw_sbuf);
}

void usage(char *progname) {
    fprintf(stderr, "Usage: %s <max hamming distance> <name> [dictionary file]\n", progname);
}

int main(int argc, char *argv[]) {
    int     fd[2], max_ed;
    char   *dictpath;
    char   *name;
    pid_t   childpid_dictcheck;
    int     status_dictcheck;

    // Check and extract command-line arguments
    switch (argc) {
        case 4:
            dictpath = argv[3];
        case 3:
            sscanf(argv[1], "%d", &max_ed);
            name = argv[2];
            break;
        default:
            fprintf(stderr, "%s: Unexpected number of arguments: %d. Exiting.\n\n", argv[0], argc - 1);
            usage(argv[0]);
            return 3;
    }


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

        if (dictpath) {
            checkwords(fd[0], dictpath);
        } else {
            catlines(fd[0]);
        }

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
