#ifndef MMAP_H
#define MMAP_H

typedef struct mmap_t {
    char *buf;
    off_t f_len;
    struct mmap_t *next;
    int active;
    int contiguous;
} mmap_t;

void init_mmap(void);
mmap_t *ag_mmap(int fd, off_t f_len);
void ag_munmap(mmap_t *m);

#endif
