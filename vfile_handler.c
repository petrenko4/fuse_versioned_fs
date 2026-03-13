#include "vfile_handler.h"
#include "fs_helper.h"
#include <stdint.h>
#include "fcntl.h"
#include "stdint.h"
#include "stdlib.h"
#include "unistd.h"
#include "assert.h"
#include "errno.h"

int store_blocks(uint64_t *blocks, uint64_t count, int fd_disk, int fd_file)
{
    for (uint64_t i = 0; i < count; i++)
    {
        uint64_t block = blocks[i];
        char buffer[BLOCK_SIZE];

        if (lseek(fd_file, block * BLOCK_SIZE, SEEK_SET) == -1)
            return -errno;
        if (read(fd_file, buffer, sizeof(buffer)) == -1)
            return -errno;
        if (write(fd_disk, buffer, sizeof(buffer)) == -1)
            return -errno;
        if (lseek(fd_file, BLOCK_SIZE, SEEK_CUR) == -1)
            return -errno;
    }
    return 0;
}

uint64_t *count_affected_blocks(size_t size, off_t offset, uint64_t *ba)
{
    uint64_t blocks_amount = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint64_t fisrt_block = offset / BLOCK_SIZE;
    *ba = blocks_amount;
    //!!!!!!!!!!!!!!!!!!!!!!!!!!
    uint64_t *changed_blocks = malloc(blocks_amount * sizeof(uint64_t));
    if (!changed_blocks)
        return NULL;
    uint64_t k = fisrt_block;
    for (uint64_t i = 0; i < blocks_amount; i++)
    {
        changed_blocks[i] = k;
        k++;
    }
    return changed_blocks;
}