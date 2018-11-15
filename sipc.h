#ifndef __SIPC_H__
#define __SIPC_H__

#define SIPC_BLOCK_WRITE 1
#define SIPC_BLOCK_READ  2

struct sipc;

int sipc_create(const char *name, size_t size, uint64_t attributes,
                struct sipc **sipc);
int sipc_open(const char *name, struct sipc **sipc);
int sipc_write(struct sipc *sipc, void *data, size_t data_size);
int sipc_read(struct sipc *sipc, void *data, size_t data_size);

#endif
