#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

void* sleepsort_start(void* arg)
{
    uintptr_t* e = (uintptr_t*)(arg);
    sleep(*e);
    printf("%d\n", (int)(*e));
    return NULL;
}

void* sleepsort_group_start(void* arg)
{
    uintptr_t* v = (uintptr_t*)(arg);
    size_t size = *v++;
    pthread_t* thrds = malloc(sizeof(thrds[0]) * size);
    if (thrds == NULL)
        exit(-1);

    {
        size_t i;
        for (i = 0 ; i != size ; ++i) {
            if (pthread_create(&thrds[i], NULL, sleepsort_start, v + i) != 0)
                exit(-1);
        }

        for (i = 0 ; i != size ; ++i) {
            if (pthread_join(thrds[i], NULL) != 0)
                exit(-1);
        }
    }

    free(thrds);
    free(arg);
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

int main()
{
    uintptr_t v[] = {8, 42, 38, 111, 2, 39, 1};

    pthread_t sleeper = sleepsort(v, sizeof(v) / sizeof(v[0]));

    if (pthread_join(sleeper, NULL) != 0)
        exit(-1);
}
