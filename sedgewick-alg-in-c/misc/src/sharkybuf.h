
/* vim: set ts=8 sts=4 sw=4 et filetype=c: */

#ifndef SHARKYBUF_H
#define SHARKYBUF_H

/*
 ***************************************************************
 * sharkybuf.h  Buffer handling utility routines               *
 *                                                             *
 ***************************************************************
 */


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

void sb_create_mmap(struct sharkybuf *sb, size_t len);
void sb_create_posix_memalign(struct sharkybuf *sb, size_t len);
void sb_create_malloc(struct sharkybuf *sb, size_t len);
void sb_realloc(struct sharkybuf *sb, size_t new_len);
void sb_dispose_munmap_(struct sharkybuf *sb);
void sb_dispose_free_(struct sharkybuf *sb);
void sb_dispose(struct sharkybuf *sb);
void sb_wipe(struct sharkybuf *sb);
int sb_append_line_or_zeroes(struct sharkybuf *sb, char *line);
int sb_recvbuf_read(struct sharkybuf *sb, int fd);
void sb_sendbuf_vmsplice(struct sharkybuf *sb, int fd);
void sb_buf_to_stdout(struct sharkybuf *sb);

#endif /* SHARKYBUF_H */
