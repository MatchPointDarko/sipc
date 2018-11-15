#include <assert.h>
#include <stdatomic.h>
#include <limits.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "util.h"
#include "sipc.h"


/* A table that keeps information on the shared memory ring buffer. */
struct sipc_table {
	size_t size;

	/* We use uint64_t to avoid wrap-arounds. It is highly unlikely
	 * that uint64_t will wrap around... */
	uint64_t write_offset;
	uint64_t read_offset;

	uint64_t attributes;

	atomic_size_t refcnt;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

struct sipc {
	char name[NAME_MAX+1];

	int fd;
	struct sipc_table *table;
};

int sipc_create(const char *name, size_t size, uint64_t attributes,
                struct sipc **sipc)
{
	int ret = 0;
	int fd = -1;
	void *map = MAP_FAILED;
	char actualname[NAME_MAX+1];
	struct sipc *context = NULL;
	struct sipc_table *table = NULL;
	sem_t *sem = SEM_FAILED;
	bool mutex_created = false, cond_created = false;

	if (name == NULL || size == 0 || sipc == NULL)
		return -EINVAL;

	*sipc = NULL;

	snprintf(actualname, NAME_MAX, "/%s", name);

	/* First, let's create the named semaphore to synchronize possible
	 * concurrent sem_open()(e.g. sipc_open()) attempts. */
	sem = sem_open(actualname, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, 1);
	if (sem == SEM_FAILED)
		return -errno;

	/* Before!!!! We're doing anything, we must call
	 * sem_wait() to block other concurrent sipc_open() threads. */
	ret = sem_wait(sem);
	if (ret == -1) {
		ret = -errno;
		goto out;
	}

	context = calloc(1, sizeof(*context));
	if (context == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	fd = shm_open(actualname, O_RDWR | O_CREAT | O_EXCL,
	              S_IRUSR | S_IWUSR);
	if (fd == -1) {
		ret = -errno;
		goto out;
	}

	size += sizeof(*table);
	RETRY_SYSCALL(ret, ftruncate(fd, size));
	if (ret == -1) {
		ret = -errno;
		goto out;
	}

	map = mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		ret = -errno;
		goto out;
	}

	table = (struct sipc_table *) map;

	ret = create_shared_mutex(&table->mutex);
	if (ret < 0)
		goto out;

	mutex_created = true;

	ret = create_shared_cond(&table->cond);
	if (ret < 0)
		goto out;

	cond_created = true;

	atomic_fetch_add(&table->refcnt, 1);
	table->size = size;
	table->attributes = attributes;
	strncpy(context->name, actualname, NAME_MAX);
	context->fd = fd;
	context->table = table;

	*sipc = context;
out:
	if (ret != 0) {
		if (cond_created)
			pthread_cond_destroy(&table->cond);

		if (mutex_created)
			pthread_mutex_destroy(&table->mutex);

		if (map != MAP_FAILED)
			munmap(map, size);

		if (fd != -1) {
			int err = 0;

			/* We've failed... truncate the shared memory file to 0.
			 * This will cause any concurrent sipc_open() threads
			 * to fail. Make sure we don't fail because of
			 * signaling. If it's a different failure, we cannot
			 * do anything.. */
			RETRY_SYSCALL(err, ftruncate(fd, 0));
			assert(err == 0);

			close(fd);
			shm_unlink(actualname);
		}

		if (context)
			free(context);
	}

	if (sem != SEM_FAILED) {
		/* This will wake up possible concurrent sipc_open()
		 * threads. */
		sem_post(sem);
		sem_close(sem);
		sem_unlink(actualname);
	}

	return ret;
}

/* Here we handle a scenario where a newly created sipc object
 * hasn't been truncated yet, so it has size 0, and someone
 * tries to open the shm object(which is available) before we
 * actually initialized it.
 *
 *
 *  +----THREAD1----+----THREAD2----+
 *  | sipc_create() |               |
 *  |-------------------------------|
 *  |               | sipc_open()   |
 *  |-------------------------------|
 *  | ftruncate()   |               |
 *  +-------------------------------+
 *
 *  This makes the calling thread block until the shared memory is __fully__
 *  initialized.
*/
static int shm_sync(const char *name)
{
	int ret = 0;
	sem_t *sem = SEM_FAILED;

	/* First, let's create the named semaphore to synchronize possible
	 * concurrent sem_open()(e.g. sipc_open()) attempts. */
	sem = sem_open(name, 0);
	if (sem == SEM_FAILED) {
		/* If the semaphore doesn't exist, this means we're synced.
		 * The semaphore destruction is triggered by the sipc creator
		 * thread via sem_unlink(). */
		if (errno == ENOENT)
			return 0;

		return -errno;
	}

	/* OK, let's wait. */
	ret = sem_wait(sem);

	(void) sem_close(sem);

	return ret == -1 ? -errno : 0;
}

int sipc_open(const char *name, struct sipc **sipc)
{
	int ret = 0;
	int fd = -1;
	char actualname[NAME_MAX+1];
	size_t size = 0;
	size_t realsize = 0;
	void *map = MAP_FAILED;
	struct sipc *context = NULL;
	struct sipc_table *table = NULL;

	if (name == NULL || sipc == NULL)
		return -EINVAL;

	*sipc = NULL;

	snprintf(actualname, NAME_MAX, "/%s", name);

	context = calloc(1, sizeof(*context));
	if (context == NULL)
		return -ENOMEM;

	fd = shm_open(actualname, O_RDWR, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		ret = -errno;
		goto failed;
	}

	ret = shm_sync(actualname);
	if (ret < 0)
		goto failed;

	size = sizeof(*table);
	map = mmap(NULL, size, PROT_WRITE  | PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		ret = -errno;
		goto failed;
	}

	table = (struct sipc_table *) map;
	realsize = table->size;
	ret = munmap(map, size);
	if (ret == -1) {
		ret = -errno;
		goto failed;
	}

	table = NULL;
	map = MAP_FAILED;
	size = realsize;
	map = mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		ret = -errno;
		goto failed;
	}

	table = (struct sipc_table *) map;

	if (!atomic_fetch_add_not_zero(&table->refcnt, 1)) {
		/* If the previous atomic value is 0, the shm has been
		 * marked for unlinkage via shm_unlink()... fail. */
		ret = -ENOENT;
		goto failed;
	}

	strncpy(context->name, actualname, NAME_MAX);
	context->fd = fd;
	context->table = table;

	*sipc = context;
	return 0;

failed:
	if (map != MAP_FAILED)
		munmap(map, size);

	if (fd != -1)
		close(fd);

	if (context)
		free(context);

	return ret;
}

void sipc_destroy(struct sipc *sipc)
{
	if (sipc) {
		if (atomic_fetch_sub(&sipc->table->refcnt, 1) == 1) {
			shm_unlink(sipc->name);
			pthread_mutex_destroy(&sipc->table->mutex);
			pthread_cond_destroy(&sipc->table->cond);
		}

		close(sipc->fd);
		munmap((void *) sipc->table, sipc->table->size);

		free(sipc);
	}
}

int sipc_write(struct sipc *sipc, void *data, size_t data_size)
{
	int ret = 0;
	struct sipc_table *table = NULL;
	char *buffer = NULL;
	size_t buffer_size = 0;
	size_t written = 0;
	size_t avail_left = 0;

	if (sipc == NULL || data == NULL || data_size == 0)
		return -EINVAL;

	table = sipc->table;
	buffer = (char *) (sipc->table + 1);
	buffer_size = sipc->table->size - sizeof(*sipc->table);

	if (buffer_size < data_size)
		return -E2BIG;

	pthread_mutex_lock(&table->mutex);

	while (table->write_offset + data_size - table->read_offset >
	       buffer_size) {
		if (table->attributes & SIPC_BLOCK_WRITE) {
			/* If we're the user requested to block, block the
			 * writer. */
			ret = pthread_cond_wait(&table->cond, &table->mutex);
			if (ret != 0) {
				ret = -ret;
				goto out;
			}
		} else {
			table->read_offset = table->write_offset + data_size;
			break;
		}
	}

	/* Make sure we wrap around properly. */
	avail_left = min(data_size, buffer_size -
	                            table->write_offset % buffer_size);
	memcpy(buffer + table->write_offset % buffer_size,
	       data, avail_left);

	written += avail_left;
	table->write_offset += avail_left;
	data_size -= avail_left;
	if (data_size > 0) {
		/* If we have data to write, surely we've wrapped around.. */
		memcpy(buffer + table->write_offset % buffer_size,
		       data + written, data_size);
		written += data_size;
	}

	/* Wake up any possible readers. */
	ret = pthread_cond_signal(&table->cond);
	if (ret != 0) {
		ret = -ret;
		goto out;
	}

	ret = written;
out:
	pthread_mutex_unlock(&table->mutex);
	return ret;
}

int sipc_read(struct sipc *sipc, void *data, size_t data_size)
{
	int ret = 0;
	struct sipc_table *table = NULL;
	char *buffer = NULL;
	size_t buffer_size = 0;
	size_t read = 0;
	size_t avail_left = 0;

	if (sipc == NULL || data == NULL || data_size == 0)
		return -EINVAL;

	table = sipc->table;
	buffer = (char *) (sipc->table + 1);
	buffer_size = sipc->table->size - sizeof(*sipc->table);

	if (buffer_size < data_size)
		return -E2BIG;

	pthread_mutex_lock(&table->mutex);

	while (table->read_offset == table->write_offset) {
		if (table->attributes & SIPC_BLOCK_READ) {
			/* If the user requested to block, block the
			 * reader until theres something to read... */
			ret = pthread_cond_wait(&table->cond, &table->mutex);
			if (ret != 0) {
				ret = -ret;
				goto out;
			}

		} else
			goto out;
	}

	/* We might have less than the actual requested size, make sure
	 * we don't overread garbage data.. */
	data_size = min(data_size, table->write_offset - table->read_offset);

	avail_left = min(table->write_offset - table->read_offset,
	                 buffer_size - table->read_offset);
	avail_left = min(data_size, avail_left);
	memcpy(data, buffer + table->read_offset % buffer_size, avail_left);

	read += avail_left;
	table->read_offset += avail_left;
	data_size -= avail_left;
	if (data_size > 0) {
		/* If we have data to write, surely we've wrapped around.. */
		memcpy((char *) data + read,
		       buffer + table->read_offset % buffer_size,
		       data_size);
		read += data_size;
	}

	/* Wake up any possible writers. */
	ret = pthread_cond_signal(&table->cond);
	if (ret != 0) {
		ret = -ret;
		goto out;
	}

	ret = read;
out:
	pthread_mutex_unlock(&table->mutex);
	return ret;
}

int sipc_write2(struct sipc *sipc, void **buf, size_t size)
{
	int ret = 0;
	struct sipc_table *table = NULL;
	char *buffer = NULL;
	size_t buffer_size = 0;
	size_t written = 0;
	size_t avail_left = 0;

	if (sipc == NULL || data == NULL || data_size == 0)
		return -EINVAL;

	table = sipc->table;
	buffer = (char *) (sipc->table + 1);
	buffer_size = sipc->table->size - sizeof(*sipc->table);

	if (buffer_size < data_size)
		return -E2BIG;

	pthread_mutex_lock(&table->mutex);

	while (table->write_offset + data_size - table->read_offset >
	       buffer_size) {
		if (table->attributes & SIPC_BLOCK_WRITE) {
			/* If we're the user requested to block, block the
			 * writer. */
			ret = pthread_cond_wait(&table->cond, &table->mutex);
			if (ret != 0) {
				ret = -ret;
				goto out;
			}
		} else {
			table->read_offset = table->write_offset + data_size;
			break;
		}
	}

	/* Make sure we wrap around properly. */
	avail_left = min(data_size, buffer_size -
	                            table->write_offset % buffer_size);
	memcpy(buffer + table->write_offset % buffer_size,
	       data, avail_left);

	written += avail_left;
	table->write_offset += avail_left;
	data_size -= avail_left;
	if (data_size > 0) {
		/* If we have data to write, surely we've wrapped around.. */
		memcpy(buffer + table->write_offset % buffer_size,
		       data + written, data_size);
		written += data_size;
	}

	/* Wake up any possible readers. */
	ret = pthread_cond_signal(&table->cond);
	if (ret != 0) {
		ret = -ret;
		goto out;
	}

	ret = written;
out:
	pthread_mutex_unlock(&table->mutex);
	return ret;
}

int sipc_commit(struct sipc *sipc, void *buf)
{




}

int sipc_read2(struct sipc *sipc, void **buf, size_t *size)
{




}

#include <time.h>
#include <stdlib.h>

int do_parent(void)
{
	int ret = 0;
	struct sipc *sipc = NULL;

	ret = sipc_create("foo", 1024, SIPC_BLOCK_READ, &sipc);
	if (ret < 0) {
		//printf("PARENT: sipc_create()=%d\n", ret);
		exit(1);
	}

	srand(time(NULL));

	for (int i = 0;i < 25;i++) {
		//int i = rand() % 1024;

		ret = sipc_write(sipc, "foozball", sizeof("foozball"));
		if (ret < 0) {
			//printf("PARENT: sipc_write()=%d\n", ret);
			goto cleanup;
		}

		//printf("PARENT: write %d\n", i);
	}

	//printf("PARENT: done\n");

cleanup:
	if (sipc) {
		sipc_destroy(sipc);
	}

	exit(1);
}

int do_child(void)
{
	int ret = 0;
	struct sipc *sipc = NULL;
	int attempts = 0;
	int i = 0;

	//printf("CHILD: start\n");

	ret = sipc_open("foo", &sipc);
	while (ret != 0 && attempts < 30) {
		++attempts;
		printf("CHILD: sipc_open()=%d, retry...\n", ret);
		ret = sipc_open("foo", &sipc);
	}

	if (attempts == 30) {
		//printf("CHILD: retry failed..\n");
		exit(1);
	}

	for (i; i < 25; i++) {
		char buf[sizeof("foozball")];
		clock_t before = 0;
		clock_t after = 0;

		before = clock();
		ret = sipc_read(sipc, buf, sizeof(buf));
		if (ret < 0) {
			//printf("CHILD: sipc_read()=%d\n", ret);
			goto cleanup;
		}
		after = clock();
		printf("[%d] CHILD: time=%f read (%d) %s\n", i,
		      ((double) (after-before)) / CLOCKS_PER_SEC, ret, buf);
	}

	//printf("CHILD: done\n");

cleanup:
	if (sipc)
		sipc_destroy(sipc);
	exit(1);
}

int main(void)
{
	pid_t pid;

	//sem_unlink("/foo");
	//shm_unlink("/foo");

	pid = fork();
	if (pid) {
		do_parent();

	} else {
		do_child();
	}

	return 0;
}
