#ifndef FILE_HANDLER
#define FILE_HANDLER
#define TYPE_MASK (1ULL << 63) 
#define MAKE_VERSION(x) ((x) | TYPE_MASK)
#define MAKE_BLOCK(x)   ((x) & ~TYPE_MASK)
#define IS_VERSION(x)   ((x) & TYPE_MASK)
#define GET_VALUE(x)    ((x) & ~TYPE_MASK)


#include "stdint.h"
#include "stdio.h"

int save_entire_file(char* path, int fd_disk); 
int store_blocks(uint64_t* blocks, uint64_t count, int fd_disk, int fd_file, int fd_vt, int fd_vf, uint64_t version);
uint64_t* count_affected_blocks(size_t size, off_t offset, uint64_t* ba);

#endif