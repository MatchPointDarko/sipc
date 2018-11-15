#ifndef __UTIL_H__
#define __UTIL_H__

#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>

#define min(x, y) (x) > (y) ? (y) : (x)
#define RETRY_SYSCALL(ret, call)  \
do { \
	ret = call; \
} while (ret == -1 && errno == EINTR); \


int create_shared_mutex(pthread_mutex_t *mutex);
int create_shared_cond(pthread_cond_t *cond);

size_t atomic_fetch_add_not_zero(atomic_size_t *atomic, size_t val);

#endif
