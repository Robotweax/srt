/* Qualification-only tracking of libobs allocations, enabled by fixture env. */
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

struct rw_allocation {
    void* ptr;
    size_t size;
    int depth;
    void* frames[24];
    struct rw_allocation* next;
};
static struct rw_allocation* rw_allocations[4096];
static pthread_mutex_t rw_allocation_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t rw_allocation_once = PTHREAD_ONCE_INIT;
static int rw_allocation_enabled;

static void rw_allocation_init(void)
{
    rw_allocation_enabled = getenv("ROBOTWEAX_OBS_TRACE_ALLOCATIONS") != NULL;
}

static void rw_allocation_update(void* old, void* ptr, size_t size)
{
    pthread_once(&rw_allocation_once, rw_allocation_init);
    if (!rw_allocation_enabled)
        return;
    struct rw_allocation* entry = NULL;
    if (ptr) {
        entry = malloc(sizeof(*entry));
        if (!entry)
            abort(); /* Incomplete evidence must not appear successful. */
        entry->ptr = ptr;
        entry->size = size;
        entry->depth = backtrace(entry->frames, 24);
    }
    pthread_mutex_lock(&rw_allocation_mutex);
    if (old) {
        struct rw_allocation** link =
            &rw_allocations[((uintptr_t)old >> 5) % 4096];
        while (*link && (*link)->ptr != old)
            link = &(*link)->next;
        if (*link) {
            struct rw_allocation* removed = *link;
            *link = removed->next;
            free(removed);
        }
    }
    if (entry) {
        size_t bucket = ((uintptr_t)ptr >> 5) % 4096;
        entry->next = rw_allocations[bucket];
        rw_allocations[bucket] = entry;
    }
    pthread_mutex_unlock(&rw_allocation_mutex);
}

static void rw_allocation_dump(void)
{
    if (!rw_allocation_enabled)
        return;
    pthread_mutex_lock(&rw_allocation_mutex);
    fprintf(stderr, "OBS_ALLOCATIONS counter=%ld\n", num_allocs);
    for (size_t bucket = 0; bucket < 4096; ++bucket) {
        for (struct rw_allocation* entry = rw_allocations[bucket]; entry;
            entry = entry->next) {
            fprintf(stderr, "OBS_ALLOCATION ptr=%p size=%zu bytes=", entry->ptr,
                entry->size);
            for (size_t i = 0; i < entry->size && i < 64; ++i)
                fprintf(stderr, "%02x", ((const unsigned char*)entry->ptr)[i]);
            fprintf(stderr, "\n");
#ifdef __APPLE__
            for (int i = 0; i < entry->depth; ++i) {
                Dl_info image;
                if (dladdr(entry->frames[i], &image))
                    fprintf(stderr, "OBS_ALLOC_FRAME %p %p %s\n",
                        entry->frames[i], image.dli_fbase, image.dli_fname);
            }
#else
            backtrace_symbols_fd(entry->frames, entry->depth, 2);
#endif
        }
    }
    pthread_mutex_unlock(&rw_allocation_mutex);
}
