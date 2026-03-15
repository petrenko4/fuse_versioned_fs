#include "vfile_handler.h"
#include "fs_helper.h"
#include <stdint.h>
#include "fcntl.h"
#include "stdint.h"
#include "stdlib.h"
#include "unistd.h"
#include "assert.h"
#include "errno.h"
#include <sys/stat.h>

int save_entire_file(char *path, int fd_disk)
{

    struct stat st;
    int fd_file = open(path, O_RDWR, 0644);
    if (fstat(fd_file, &st) == -1)
        return -errno;

    off_t size = st.st_size;
    uint64_t total_blocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    for (uint64_t i = 0; i < total_blocks; i++)
    {
        // for readability
        char buffer[BLOCK_SIZE] = {'\0'};

        if (lseek(fd_file, i * BLOCK_SIZE, SEEK_SET) == -1)
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

int store_blocks(uint64_t *blocks, uint64_t count, int fd_disk, int fd_file, int fd_vt, int fd_vf, uint64_t version)
{
    struct stat st;
    if (fstat(fd_file, &st) == -1)
        return -errno;

    off_t size = st.st_size;

    if (write(fd_vf, &size, sizeof(size)) != sizeof(size))
        return errno;
    if (lseek(fd_vf, 0, SEEK_END) == -1)
        return -errno;

    // intersection of existing blocks and blocks to write
    uint64_t diff = (size - blocks[0] * BLOCK_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;

    uint64_t value = version - 1;
    assert(value == GET_VALUE(value));
    value = MAKE_VERSION(value);
    for (uint64_t i = 0; i < blocks[0]; i++)
    {
        if (write(fd_vf, &value, sizeof(value)) != sizeof(value))
            return -errno;
        if (lseek(fd_vf, 0, SEEK_END) == -1)
            return -errno;
    }

    for (uint64_t i = 0; i < diff; i++)
    {
        uint64_t block = blocks[i];
        // for readability
        char buffer[BLOCK_SIZE] = {'\0'};

        if (lseek(fd_file, block * BLOCK_SIZE, SEEK_SET) == -1)
            return -errno;
        if (read(fd_file, buffer, sizeof(buffer)) == -1)
            return -errno;
        if (write(fd_disk, buffer, sizeof(buffer)) == -1)
            return -errno;
        uint64_t whence_disk = (lseek(fd_disk, 0, SEEK_END) / BLOCK_SIZE) - 1;
        if (write(fd_vf, &whence_disk, sizeof(whence_disk)) == -1)
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