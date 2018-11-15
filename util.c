#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>


int create_shared_mutex(pthread_mutex_t *mutex)
{
	int ret = 0;
	pthread_mutexattr_t mutexattr;

	ret = pthread_mutexattr_init(&mutexattr);
	if (ret == -1)
		return -errno;

	ret = pthread_mutexattr_setpshared(&mutexattr,
					   PTHREAD_PROCESS_SHARED);
	if (ret == -1) {
		ret = -errno;
		goto out;
	}

	ret = pthread_mutex_init(mutex, &mutexattr);
	if (ret == -1) {
		ret = -errno;
		goto out;
	}

out:
	pthread_mutexattr_destroy(&mutexattr);
	return ret;
}

int create_shared_cond(pthread_cond_t *cond)
{
	int ret = 0;
	pthread_condattr_t condattr;

	ret = pthread_condattr_init(&condattr);
	if (ret == -1)
		return -errno;

	ret = pthread_condattr_setpshared(&condattr,
	                                  PTHREAD_PROCESS_SHARED);
	if (ret == -1) {
		ret = -errno;
		goto out;
	}

	ret = pthread_cond_init(cond, &condattr);
	if (ret == -1) {
		ret = -errno;
		goto out;
	}

out:
	pthread_condattr_destroy(&condattr);
	return ret;
}

/*
 * Atomically increase @atomic with @val only if @atomic is not zero.
 */
size_t atomic_fetch_add_not_zero(atomic_size_t *atomic, size_t val)
{
	size_t old = atomic_load(atomic);

	do {
		if (old == 0)
			break;

	} while (!atomic_compare_exchange_strong(atomic, &old, old + val));

	return old;
}
