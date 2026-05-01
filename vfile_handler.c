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
        uint64_t value;
        if (pread(fd_vf, &value, sizeof(value), vf_offset + i * sizeof(uint64_t)) == -1)
            return -errno;
        if (IS_VERSION(value))
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
    }

    return 0;
}

// traverse previous versions untill pointer to disk found
uint64_t check_version(int fd_disk, int fd_file, int fd_vt, int fd_vf, uint64_t version, uint64_t version_max, uint64_t block_number)
{

    if (version > version_max)
        return -1;

    uint64_t vf_off = 0;
    if (pread(fd_vt, &vf_off, sizeof(vf_off), version * sizeof(uint64_t)) == -1)
        return errno;

    uint64_t value = 0;
    if (pread(fd_vf, &value, sizeof(value), vf_off + 2 * sizeof(uint64_t) + block_number * sizeof(uint64_t)) == -1)
        return errno;

    if (!IS_VERSION(value))
        return value;

    uint64_t res = check_version(fd_disk, fd_file, fd_vt, fd_vf, GET_VALUE(value), version_max, block_number);

    // if (pwrite(myfh->fd_vf, &res, sizeof(value), vf_off + 2 * sizeof(uint64_t) + block_number * sizeof(uint64_t)) == -1)
    //     return errno;

    return res;
}

int read_version(size_t read_size, off_t offset, char *buf, uint64_t version, int fd_disk, int fd_file, int fd_vt, int fd_vf, uint64_t *bytes_read)
{

    uint64_t buffer[BLOCK_SIZE / sizeof(uint64_t)];
    uint64_t vf_offset = 0;
    if (pread(fd_vt, &vf_offset, sizeof(vf_offset), version * sizeof(uint64_t)) == -1)
        return -errno;
    uint64_t version_max = 0;
    if (pread(fd_vt, &version_max, sizeof(uint64_t), 0) == -1)
        return -errno;

    uint64_t file_size = 0;

    if (pread(fd_vf, &file_size, sizeof(file_size), vf_offset + sizeof(uint64_t)) == -1)
        return -errno;
    uint64_t affected_interval_left = offset / BLOCK_SIZE;

    uint64_t affected_interval_right = (file_size < (offset + read_size))
                                           ? ((file_size + BLOCK_SIZE - 1) / BLOCK_SIZE)
                                           : (((offset + read_size) + BLOCK_SIZE - 1) / BLOCK_SIZE);
    uint64_t next_version_off = vf_offset + 2 * sizeof(uint64_t) + ((file_size + BLOCK_SIZE - 1) / BLOCK_SIZE) * sizeof(uint64_t);

    vf_offset += 2 * sizeof(uint64_t);

    *bytes_read = 0;

    for (uint64_t i = affected_interval_left; i < affected_interval_right; i++)
    {
        uint64_t to_read = BLOCK_SIZE;
        int is_one = 0;
        int is_first = 0;
        int is_last = 0;
        if ((affected_interval_right - affected_interval_left) == 1)
        {
            to_read = file_size;
            is_one = 1;
        }
        else if (i == affected_interval_left)
        {
            to_read = BLOCK_SIZE - (offset % BLOCK_SIZE);
            is_first = 1;
        }
        else if (i == affected_interval_right - 1)
        {
            to_read = (offset + file_size) % BLOCK_SIZE;
            if (to_read == 0)
                to_read = BLOCK_SIZE;
            is_last = 1;
        }
        uint64_t value;
        if (pread(fd_vf, &value, sizeof(value), vf_offset + (i * sizeof(uint64_t))) == -1)
            return -errno;

        if (!IS_VERSION(value))
        {
            if (is_one)
            {
                if (pread(fd_disk, buf + *bytes_read, to_read, (value * BLOCK_SIZE) + (offset % BLOCK_SIZE)) == -1)
                    return -errno;
                *bytes_read += to_read;
            }
            else if (is_first)
            {
                if (pread(fd_disk, buf + *bytes_read, to_read, (value * BLOCK_SIZE) + (offset % BLOCK_SIZE)) == -1)
                    return -errno;
                *bytes_read += to_read;
            }
            else if (is_last)
            {
                if (pread(fd_disk, buf + *bytes_read, to_read, value * BLOCK_SIZE) == -1)
                    return -errno;
                *bytes_read += to_read;
            }
            else
            {
                if (pread(fd_disk, buf + *bytes_read, BLOCK_SIZE, value * BLOCK_SIZE) == -1)
                    return -errno;
                *bytes_read += BLOCK_SIZE;
            }
        }
        else
        {

            uint64_t relevant_block = check_version(fd_disk, fd_file, fd_vt, fd_vf, version, version_max, i);
            if (relevant_block == -1)
            {
                if (is_one)
                {
                    if (pread(fd_file, buf + *bytes_read, to_read, (i * BLOCK_SIZE) + (offset % BLOCK_SIZE)) == -1)
                        return -errno;
                    *bytes_read += BLOCK_SIZE;
                }
                else if (is_first)
                {
                    if (pread(fd_file, buf + *bytes_read, to_read, (i * BLOCK_SIZE) + (offset % BLOCK_SIZE)) == -1)
                        return -errno;
                    *bytes_read += BLOCK_SIZE;
                }
                else if (is_last)
                {
                    if (pread(fd_file, buf + *bytes_read, to_read, i * BLOCK_SIZE) == -1)
                        return -errno;
                    *bytes_read += BLOCK_SIZE;
                }
                else
                {
                    if (pread(fd_file, buf + *bytes_read, BLOCK_SIZE, i * BLOCK_SIZE) == -1)
                        return -errno;
                    *bytes_read += BLOCK_SIZE;
                }
            }
            else
            {
                if (is_one)
                {
                    if (pread(fd_disk, buf + *bytes_read, to_read, (relevant_block * BLOCK_SIZE) + (offset % BLOCK_SIZE)) == -1)
                        return -errno;
                    *bytes_read += BLOCK_SIZE;
                }
                else if (is_first)
                {
                    if (pread(fd_file, buf + *bytes_read, to_read, (relevant_block * BLOCK_SIZE) + (offset % BLOCK_SIZE)) == -1)
                        return -errno;
                    *bytes_read += BLOCK_SIZE;
                }
                else if (is_last)
                {
                    if (pread(fd_file, buf + *bytes_read, to_read, relevant_block * BLOCK_SIZE) == -1)
                        return -errno;
                    *bytes_read += BLOCK_SIZE;
                }
                else
                {
                    if (pread(fd_disk, buf + *bytes_read, BLOCK_SIZE, relevant_block * BLOCK_SIZE) == -1)
                        return -errno;
                    *bytes_read += BLOCK_SIZE;
                }
            }
            // if(pwrite(myfh->fd_vf, &relevant_block, sizeof(relevant_block), i) == -1)
            //     return errno;
        }
    }
    return *bytes_read;
}