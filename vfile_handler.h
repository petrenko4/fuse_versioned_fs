#ifndef FILE_HANDLER
#define FILE_HANDLER

#include "stdint.h"
#include "stdio.h"

int store_blocks(uint64_t* blocks, uint64_t count, int fd_disk, int fd_file);
uint64_t* count_affected_blocks(size_t size, off_t offset, uint64_t* ba);

#endif