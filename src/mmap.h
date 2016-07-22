#ifndef MMAP_H
#define MMAP_H

typedef struct mmap_t {
    char *buf;
    char *target;
    off_t f_len;
    struct mmap_t *next;
    int active;
} mmap_t;

void init_mmap(void);
mmap_t *ag_mmap(int fd, off_t f_len);
void ag_munmap(mmap_t *m);

#endif
