#ifndef MMAP_H
#define MMAP_H

void init_mmap(void);
void ag_munmap(void *buf, off_t f_len);

#endif
