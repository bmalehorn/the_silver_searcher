#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>

#include "mmap.h"
#include "log.h"
#include "util.h"

static off_t pagesize;
static int buffer_size;

static pthread_mutex_t mmap_mtx;
static mmap_t *head = NULL;
static mmap_t *tail = NULL;
static mmap_t *first_active = NULL;
static char *next_addr = (char*)0x500000000000;
static int inactive_count = 0;

void init_mmap(void) {
    pagesize = (off_t)sysconf(_SC_PAGESIZE);
    if (pthread_mutex_init(&mmap_mtx, NULL)) {
        die("Could not initialize mmap_mtx!");
    }
    char *buffer_size_str = getenv("BUFFER_SIZE");
    if (!buffer_size_str || !(buffer_size = atoi(buffer_size_str))) {
        buffer_size = 20;
    }
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
    /* printf("mmap(%p, %d) => %p\n", target, (int)f_len, buf); */
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
    while (first_active != tail && !first_active->active) {
        first_active = first_active->next;
        inactive_count++;
    }

    mmap_t *non_contiguous = NULL;
    char *batch_start = NULL;
    char *batch_end = NULL;

    if (inactive_count >= buffer_size) {
        batch_start = head->target;
        while (head != first_active) {
            batch_end = head->target + round_up_to_pagesize(head->f_len);
            mmap_t *new_head = head->next;
            if (head->buf == head->target) {
                free(head);
            } else {
                head->next = non_contiguous;
                non_contiguous = head;
            }
            head = new_head;
            inactive_count--;
        }
        if (head == NULL) {
            tail = first_active = NULL;
        }
    }

    pthread_mutex_unlock(&mmap_mtx);

    if (batch_start) {
        /* printf("batch_start = %p, batch_len = %d\n", batch_start, (int)batch_len); */
        // XXX TODO: work on windows. No batching on windows
        int ret = munmap(batch_start, batch_end - batch_start);
        (void)ret;
        /* printf("BLOCK munmap(%p, %d) => %d\n", batch_start, */
        /*        (int)(batch_end - batch_start), ret); */
    }

    while (non_contiguous) {
#ifdef _WIN32
#error "not supported"
#else
        int ret = munmap(non_contiguous->buf, non_contiguous->f_len);
        (void)ret;
        /* printf("SOLO  munmap(%p, %d) => %d\n", non_contiguous->buf, */
        /*        (int)non_contiguous->f_len, ret); */
#endif
        mmap_t *next = non_contiguous->next;
        free(non_contiguous);
        non_contiguous = next;
    }
}
