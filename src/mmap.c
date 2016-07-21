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

static size_t pagesize;
static pthread_mutex_t mmap_mtx;
static mmap_t *head = NULL;
static mmap_t *tail = NULL;
static mmap_t *first_active = NULL;

void init_mmap(void) {
    pagesize = (size_t)getpagesize();
    if (pthread_mutex_init(&mmap_mtx, NULL)) {
        die("Could not initialize mmap_mtx!");
    }
}

mmap_t *ag_mmap(int fd, off_t f_len) {
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
    buf = mmap(0, f_len, PROT_READ, MAP_SHARED, fd, 0);
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

    mmap_t *m = ag_malloc(sizeof(*m));
    m->f_len = f_len;
    m->buf = buf;
    m->active = TRUE;
    m->next = NULL;

    pthread_mutex_lock(&mmap_mtx);
    if (head) {
        tail->next = m;
        tail = m;
    } else {
        head = tail = first_active = m;
    }
    pthread_mutex_unlock(&mmap_mtx);

    return m;
}

void ag_munmap(mmap_t *m) {
    pthread_mutex_lock(&mmap_mtx);

    m->active = FALSE;
    while (first_active && !first_active->active) {
        first_active = first_active->next;
    }

    mmap_t *batch = head;
    while (head != first_active) {
        mmap_t *new_head = head->next;
        head->next = NULL;
        head = new_head;
    }
    if (head == NULL) {
        tail = first_active = NULL;
    }

    pthread_mutex_unlock(&mmap_mtx);

    for( ; batch ; batch = batch->next) {
#ifdef _WIN32
        UnmapViewOfFile(batch->buf);
#else
        munmap(batch->buf, batch->f_len);
#endif
        free(batch);
    }
}
