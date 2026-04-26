#ifndef FILE_HANDLER
#define FILE_HANDLER
#define TYPE_MASK (1ULL << 63) 
#define MAKE_VERSION(x) ((x) | TYPE_MASK)
#define MAKE_BLOCK(x)   ((x) & ~TYPE_MASK)
#define IS_VERSION(x)   ((x) & TYPE_MASK)
#define GET_VALUE(x)    ((x) & ~TYPE_MASK)


#include "stdint.h"
#include "stdio.h"

int store_blocks(size_t write_size, off_t offset, int fd_disk, int fd_file, int fd_vt, int fd_vf);

int read_version(size_t read_size, off_t offset, char *buf, uint64_t version, int fd_disk, int fd_file, int fd_vt, int fd_vf, uint64_t* bytes_read);

#endif