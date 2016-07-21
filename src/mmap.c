#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include "mmap.h"
#include "log.h"
#include "util.h"

static off_t pagesize;
static pthread_mutex_t mmap_mtx;
static mmap_t *head = NULL;
static mmap_t *tail = NULL;
static mmap_t *first_active = NULL;
static char *next_addr = (char*)0x500000000000;
static int inactive_count = 0;

void init_mmap(void) {
    pagesize = (off_t)getpagesize();
    if (pthread_mutex_init(&mmap_mtx, NULL)) {
        die("Could not initialize mmap_mtx!");
    }
}

static off_t round_up_to_pagesize(off_t n) {
    return (n+pagesize-1) & ~(pagesize-1);
}

mmap_t *ag_mmap(int fd, off_t f_len) {
    mmap_t *m = ag_malloc(sizeof(*m));

    pthread_mutex_lock(&mmap_mtx);

    void *addr = next_addr;
    next_addr += round_up_to_pagesize(f_len);

    m->f_len = f_len;
    m->buf = addr;
    m->active = TRUE;
    m->next = NULL;

    if (head) {
        tail->next = m;
        tail = m;
    } else {
        head = tail = first_active = m;
    }

    pthread_mutex_unlock(&mmap_mtx);

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
    buf = mmap(addr, f_len, PROT_READ, MAP_SHARED|MAP_FIXED, fd, 0);
    /* printf("mmap(%p, %d) => %p\n", addr, (int)f_len, buf); */
    if (buf == MAP_FAILED || buf != addr) {
        log_err("File failed to load: %s.", strerror(errno));
        goto fail;
    }
#if HAVE_MADVISE
    madvise(buf, f_len, MADV_SEQUENTIAL);
#elif HAVE_POSIX_FADVISE
    posix_fadvise(fd, 0, f_len, POSIX_MADV_SEQUENTIAL);
#endif
#endif

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

    void *batch_base = NULL;
    off_t batch_len = 0;

    if (inactive_count > 20) {
        batch_base = head->buf;
        while (head != first_active) {
            batch_len += round_up_to_pagesize(head->f_len);
            mmap_t *new_head = head->next;
            free(head);
            head = new_head;
            inactive_count--;
        }
        if (head == NULL) {
            tail = first_active = NULL;
        }
    }

    pthread_mutex_unlock(&mmap_mtx);

    if (batch_base) {
        /* printf("batch_base = %p, batch_len = %d\n", batch_base, (int)batch_len); */
        // XXX TODO: work on windows. No batching on windows
        int ret = munmap(batch_base, batch_len);
        /* printf("munmap(%p, %d) => %d\n", batch_base, (int)batch_len, ret); */
    }
}
