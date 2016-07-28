#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "log.h"
#include "mmap.h"
#include "util.h"


static pthread_mutex_t mmap_mtx;
static off_t pagesize;

typedef struct {
    void *buf;
    off_t len;
} mmap_block_t;

static mmap_block_t *blocks;
static int blocks_capacity;
static int blocks_n;


void init_mmap(void) {
    pagesize = (off_t)sysconf(_SC_PAGESIZE);
    if (pthread_mutex_init(&mmap_mtx, NULL)) {
        die("Could not initialize mmap_mtx!");
    }
    blocks_capacity = 20;
    blocks_n = 0;
    blocks = ag_malloc(sizeof(*blocks) * blocks_capacity);
}

static off_t round_up_to_pagesize(off_t n) {
    return (n + pagesize - 1) & ~(pagesize - 1);
}

static int block_cmp(const void *a, const void *b) {
    mmap_block_t *const *c = a;
    mmap_block_t *const *d = b;
    return (*c)->buf < (*d)->buf ? -1 : 1;
}

static void do_munmap(void *buf, off_t f_len) {
    int ret = 1337;
    (void)ret;
#ifdef _WIN32
    UnmapViewOfFile(buf);
#else
    ret = munmap(buf, f_len);
#endif
    /* log_debug("munmap(%p, %zd) => %d", buf, f_len, ret); */
}

void ag_munmap(void *addr, off_t f_len) {
    mmap_block_t *old_blocks = NULL;
    int i;
    int n;

    pthread_mutex_lock(&mmap_mtx);

    n = blocks_n++;
    blocks[n].buf = addr;
    blocks[n].len = f_len;

    if (blocks_n == blocks_capacity) {
        old_blocks = blocks;
        blocks = ag_malloc(sizeof(*blocks) * blocks_capacity);
        blocks_n = 0;
    }

    pthread_mutex_unlock(&mmap_mtx);

    if (old_blocks) {
        char *base = NULL;
        off_t total_len = 0;
        qsort(old_blocks, blocks_capacity, sizeof(*old_blocks), block_cmp);


        static int munmaps;
        static int seen;
        for (i = 0; i < blocks_capacity; i++) {
            void *buf = old_blocks[i].buf;
            off_t len = round_up_to_pagesize(old_blocks[i].len);
            seen++;
            /* printf("@@@ %p + %6zd, %p + %6zd\n", base, total_len, buf, len); */
            if (base == NULL) {
                base = buf;
                total_len = len;
            } else if (base + total_len == buf) {
                total_len += len;
            } else {
                do_munmap(base, total_len);
                munmaps++;
                base = buf;
                total_len = len;
            }
        }
        do_munmap(base, total_len);
        munmaps++;
        free(old_blocks);
        static int hits;
        if (hits++ % 64 == 0) {
            /* log_debug("@@@ %d / %d", munmaps, seen); */
        }
    }
}
