#ifndef MMAP_H
#define MMAP_H

typedef struct {
    void *buf;
    off_t f_len;
} mmap_t;

mmap_t *ag_mmap(int fd, off_t f_len);
void ag_munmap(mmap_t *m);

#endif
