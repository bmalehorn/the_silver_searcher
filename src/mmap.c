#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

#include "mmap.h"
#include "log.h"
#include "util.h"

static off_t pagesize;
static int buffer_size = 0;
static size_t mem_size = 0; // in bytes

static pthread_mutex_t mmap_mtx;
static mmap_t *head = NULL;
static mmap_t *tail = NULL;
static mmap_t *first_active = NULL;
static char *next_addr;
static int inactive_count = 0;
static size_t mem_count = 0;

void init_mmap(void) {
    pagesize = (off_t)sysconf(_SC_PAGESIZE);
    if (pthread_mutex_init(&mmap_mtx, NULL)) {
        die("Could not initialize mmap_mtx!");
    }
    char *mem_size_str = getenv("MEM_SIZE");
    if (!mem_size_str || !(mem_size = atoi(mem_size_str))) {
        mem_size = LONG_MAX;
    }
    char *buffer_size_str = getenv("BUFFER_SIZE");
    if (!buffer_size_str || !(buffer_size = atoi(buffer_size_str))) {
        buffer_size = 20;
    }

#if INTPTR_MAX == INT32_MAX
    next_addr = (char*)0x50000000;
#else
    next_addr = (char*)0x500000000000;
#endif
}

static off_t round_up_to_pagesize(off_t n) {
    return (n+pagesize-1) & ~(pagesize-1);
}

static void *try_mmap_addr(void *target, int fd, off_t f_len) {
    void *buf;
#ifdef _WIN32
    {
        HANDLE hmmap = CreateFileMapping(
            (HANDLE)_get_osfhandle(fd), 0, PAGE_READONLY, 0, f_len, NULL);
        buf = (char *)MapViewOfFile(hmmap, FILE_SHARE_READ, 0, 0, f_len);
        if (hmmap != NULL)
            CloseHandle(hmmap);
    }
    if (buf == NULL) {
        FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, GetLastError(), 0, (void *)&buf, 0, NULL);
        log_err("File failed to load: %s.", buf);
        LocalFree((void *)buf);
        return NULL;
    }
#else
    /* static int count = 0; */
    /* if (count++ % 2 == 0) { */
    /*     buf = mmap(NULL, f_len, PROT_READ, MAP_SHARED, fd, 0); */
    /* } else { */
        buf = mmap(target, f_len, PROT_READ, MAP_SHARED, fd, 0);
    /* } */
    log_debug("mmap(%p, %zd) => %p", target, (size_t)f_len, buf);
    if (buf == MAP_FAILED) {
        log_err("File failed to load: %s.", strerror(errno));
        return NULL;
    }
#if HAVE_MADVISE
    madvise(buf, f_len, MADV_SEQUENTIAL);
#elif HAVE_POSIX_FADVISE
    posix_fadvise(fd, 0, f_len, POSIX_MADV_SEQUENTIAL);
#endif
#endif
    return buf;
}

mmap_t *ag_mmap(int fd, off_t f_len) {
    mmap_t *m = ag_malloc(sizeof(*m));

    pthread_mutex_lock(&mmap_mtx);

    m->target = next_addr;
    next_addr += round_up_to_pagesize(f_len);

    m->f_len = f_len;
    m->active = TRUE;
    m->next = NULL;

    if (head) {
        tail->next = m;
        tail = m;
        if (!first_active) {
            first_active = tail;
        }
    } else {
        head = tail = first_active = m;
    }

    pthread_mutex_unlock(&mmap_mtx);

    m->buf = try_mmap_addr(m->target, fd, f_len);
    if (m->buf == MAP_FAILED) {
        log_err("File failed to load: %s.", strerror(errno));
        goto fail;
    }

    return m;

fail:
    ag_munmap(m);
    return NULL;
}

void ag_munmap(mmap_t *m) {
    pthread_mutex_lock(&mmap_mtx);

    m->active = FALSE;
    while (first_active != NULL && !first_active->active) {
        inactive_count++;
        mem_count += round_up_to_pagesize(first_active->f_len);
        first_active = first_active->next;
    }

    mmap_t *non_contiguous = NULL;
    char *batch_start = NULL;
    char *batch_end = NULL;
    int have_batch = FALSE;

    if (inactive_count >= buffer_size || mem_count >= mem_size) {
        batch_start = head->target;
        while (head != first_active) {
            batch_end = head->target + round_up_to_pagesize(head->f_len);
            inactive_count--;
            mem_count -= round_up_to_pagesize(head->f_len);
            mmap_t *new_head = head->next;
            if (head->buf == head->target) {
                have_batch = TRUE;
                free(head);
            } else {
                head->next = non_contiguous;
                non_contiguous = head;
            }
            head = new_head;
        }
        if (head == NULL) {
            tail = first_active = NULL;
        }
    }
    log_debug("mmap: mem_count = %d\n", mem_count);

    pthread_mutex_unlock(&mmap_mtx);

    if (have_batch) {
#ifdef _WIN32
        log_err("Cannot block munmap(%p, %zd) on windows.",
                batch_start, (size_t)(batch_end - batch_start));
#else
        int ret = munmap(batch_start, batch_end - batch_start);
        log_debug("block munmap(%p, %zd) => %d", batch_start,
                  (size_t)(batch_end - batch_start), ret);
#endif
    }

    while (non_contiguous) {
#ifdef _WIN32
        UnmapViewOfFile(buf);
#else
        int ret = munmap(non_contiguous->buf, non_contiguous->f_len);
        log_debug("solo  munmap(%p, %zd) => %d", non_contiguous->buf,
                  (size_t)non_contiguous->f_len, ret);
#endif
        mmap_t *next = non_contiguous->next;
        free(non_contiguous);
        non_contiguous = next;
    }
}
