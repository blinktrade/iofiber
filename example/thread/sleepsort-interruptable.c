#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>

static int pfd[2];

void handler(int sig)
{
    (void)(sig);
    if (write(pfd[1], ".", 1) == -1)
        exit(-1);
}

struct info
{
    pthread_t* thrds;
    size_t size;
    size_t joined_count;
};

static void cancel_and_join(void* arg)
{
    struct info* info = (struct info*)(arg);
    size_t i;
    for (i = info->joined_count ; i != info->size ; ++i) {
        pthread_cancel(info->thrds[i]);
    }
    for (i = info->joined_count ; i != info->size ; ++i) {
        // The biggest semantic difference between the C POSIX version and an
        // exception-based version is that we don't need to disable cancellation
        // -- via `pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL)` -- in
        // the cleanup handler before the pthread_join().
        if (pthread_join(info->thrds[i], NULL) != 0)
            exit(-1);
    }
}

void* sleepsort_start(void* arg)
{
    uintptr_t* e = (uintptr_t*)(arg);
    sleep(*e);
    printf("%d\n", (int)(*e));
    return NULL;
}

void* sleepsort_group_start(void* arg)
{
    pthread_cleanup_push(free, arg);

    uintptr_t* v = (uintptr_t*)(arg);
    size_t size = *v++;
    pthread_t* thrds = malloc(sizeof(thrds[0]) * size);
    if (thrds == NULL)
        exit(-1);
    pthread_cleanup_push(free, thrds);

    {
        size_t i;
        for (i = 0 ; i != size ; ++i) {
            if (pthread_create(&thrds[i], NULL, sleepsort_start, v + i) != 0)
                exit(-1);
        }

        struct info info;
        info.thrds = thrds;
        info.size = size;
        info.joined_count = 0;

        pthread_cleanup_push(cancel_and_join, &info);

        for (i = 0 ; i != size ; ++i) {
            if (pthread_join(thrds[i], NULL) != 0)
                exit(-1);
            ++info.joined_count;
        }

        pthread_cleanup_pop(0);
    }

    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    return NULL;
}

pthread_t sleepsort(uintptr_t* v, size_t size)
{
    pthread_t thrd;
    uintptr_t* v2 = malloc(sizeof(v2[0]) * (size + 1));
    if (v2 == NULL)
        exit(-1);
    v2[0] = size;
    memcpy(v2 + 1, v, size * sizeof(v[0]));
    if (pthread_create(&thrd, NULL, sleepsort_group_start, v2) != 0)
        exit(-1);
    return thrd;
}

void* sigwaiter(void* arg)
{
    pthread_t* sleeper = (pthread_t*)(arg);
    char ch;
    if (read(pfd[0], &ch, 1) == -1)
        exit(-1);
    // This call can happen after `pthread_join(sleeper)` so we ignore the error
    pthread_cancel(*sleeper);
    return NULL;
}

int main()
{
    uintptr_t v[] = {8, 42, 38, 111, 2, 39, 1};

    if (pipe(pfd) == -1)
        exit(-1);

    if (signal(SIGUSR1, handler) == SIG_ERR)
        exit(-1);

    pthread_t sleeper = sleepsort(v, sizeof(v) / sizeof(v[0]));
    pthread_t sigwaiter_thrd;

    if (pthread_create(&sigwaiter_thrd, NULL, sigwaiter, &sleeper) != 0)
        exit(-1);

    if (pthread_join(sleeper, NULL) != 0)
        exit(-1);

    // This piece is optional if you start `sigwaiter` thread detached, but it
    // is included anyway to show how to synchronize tasks so you can properly
    // manage resources in more complex code.
    //
    // For instance: were sigwaiter thread to keep running after we deallocated
    // the `sleeper` object, we'd have a bug. {{{
    pthread_cancel(sigwaiter_thrd);
    if (pthread_join(sigwaiter_thrd, NULL) != 0)
        exit(-1);
    // }}}
}
