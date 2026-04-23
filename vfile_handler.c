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
#include <time.h>

int store_blocks(size_t write_size, off_t offset, int fd_disk, int fd_file, int fd_vt, int fd_vf)
{
    struct stat st;
    if (fstat(fd_file, &st) == -1)
        return -errno;

    off_t file_size = st.st_size;
    uint64_t version;
    if (pread(fd_vt, &version, sizeof(version), 0) == -1)
        return -errno;

    uint64_t affected_interval_left = offset / BLOCK_SIZE;

    uint64_t affected_interval_right = (file_size < (offset + write_size)) 
                                           ? ((file_size + BLOCK_SIZE - 1) / BLOCK_SIZE)
                                           : (((offset + write_size) + BLOCK_SIZE - 1) / BLOCK_SIZE);

    off_t disk_pos = lseek(fd_disk, 0, SEEK_END);
    if (disk_pos == -1)
        return -errno;

    uint64_t vf_offset;
    if (pread(fd_vt, &vf_offset, sizeof(vf_offset), version * sizeof(uint64_t)) == -1)
        return -errno;

    vf_offset += 2 * sizeof(uint64_t); // skip version number(ts) and version size

    char buffer[BLOCK_SIZE];

    for (uint64_t i = affected_interval_left; i < affected_interval_right; i++)
    {
        if (pread(fd_file, buffer, BLOCK_SIZE, i * BLOCK_SIZE) == -1)
            return -errno;

        if (write(fd_disk, buffer, BLOCK_SIZE) != BLOCK_SIZE)
            return -errno;

        uint64_t disk_block_id = disk_pos / BLOCK_SIZE;
        disk_pos += BLOCK_SIZE;

        if (pwrite(fd_vf, &disk_block_id, sizeof(disk_block_id), vf_offset + (i * sizeof(uint64_t))) == -1)
            return -errno;
    }

    return 0;
}