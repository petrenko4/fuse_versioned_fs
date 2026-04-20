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

// int save_entire_file(char *path, int fd_disk, int fd_vf)
// {

//     struct stat st;
//     int fd_file = open(path, O_RDWR, 0644);
//     if (fd_file == -1)
//         return -errno;
//     if (fstat(fd_file, &st) == -1)
//         return -errno;

//     off_t size = st.st_size;
//     uint64_t total_blocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;

//     if (write(fd_vf, &size, sizeof(size)) != sizeof(size))
//         return errno;
//     if (lseek(fd_vf, 0, SEEK_END) == -1)
//         return -errno;

//     for (uint64_t i = 0; i < total_blocks; i++)
//     {
//         uint64_t block = i;
//         // for readability
//         char buffer[BLOCK_SIZE] = {'\0'};

//         if (lseek(fd_file, block * BLOCK_SIZE, SEEK_SET) == -1)
//             return -errno;
//         if (read(fd_file, buffer, sizeof(buffer)) == -1)
//             return -errno;
//         if (write(fd_disk, buffer, sizeof(buffer)) == -1)
//             return -errno;
//         uint64_t whence_disk = (lseek(fd_disk, 0, SEEK_END) / BLOCK_SIZE) - 1;
//         if (write(fd_vf, &whence_disk, sizeof(whence_disk)) == -1)
//             return -errno;
//     }
//     return 0;
// }
// int store_blocks(size_t write_size, off_t offset, int fd_disk, int fd_file, int fd_vt, int fd_vf, uint64_t version)
// {
//     struct stat st;
//     if (fstat(fd_file, &st) == -1)
//         return -errno;
//     off_t file_size = st.st_size;

//     // compute affected blocks right here, single source of truth
//     uint64_t file_num_blocks = (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
//     uint64_t write_start = offset / BLOCK_SIZE;
//     uint64_t write_end = (offset + write_size - 1) / BLOCK_SIZE;

//     // if (write_start >= file_num_blocks)
//     //     return -EINVAL;

//     uint64_t clamp_end = write_end < file_num_blocks - 1 ? write_end : file_num_blocks - 1;
//     uint64_t count = (file_num_blocks == 0 || write_start >= file_num_blocks)
//                          ? 0
//                          : clamp_end - write_start + 1;

//     // write file size to vf
//     if (lseek(fd_vf, 0, SEEK_END) == -1)
//         return -errno;
//     if (write(fd_vf, &file_size, sizeof(file_size)) != sizeof(file_size))
//         return -errno;

//     uint64_t value = version + 1;
//     assert(value == GET_VALUE(value));
//     value = MAKE_VERSION(value);

//     // version markers for blocks before the affected range
//     for (uint64_t i = 0; i < write_start; i++)
//     {
//         if (lseek(fd_vf, 0, SEEK_END) == -1)
//             return -errno;
//         if (write(fd_vf, &value, sizeof(value)) != sizeof(value))
//             return -errno;
//     }

//     // copy each affected block to disk and record its position in vf
//     for (uint64_t i = 0; i < count; i++)
//     {
//         uint64_t block = write_start + i;
//         char buffer[BLOCK_SIZE] = {'\0'};

//         if (lseek(fd_file, block * BLOCK_SIZE, SEEK_SET) == -1)
//             return -errno;
//         if (read(fd_file, buffer, sizeof(buffer)) == -1)
//             return -errno;
//         if (lseek(fd_disk, 0, SEEK_END) == -1)
//             return -errno;
//         if (write(fd_disk, buffer, sizeof(buffer)) != sizeof(buffer))
//             return -errno;

//         uint64_t whence_disk = (lseek(fd_disk, 0, SEEK_END) / BLOCK_SIZE) - 1;

//         if (lseek(fd_vf, 0, SEEK_END) == -1)
//             return -errno;
//         if (write(fd_vf, &whence_disk, sizeof(whence_disk)) != sizeof(whence_disk))
//             return -errno;
//     }

//     for (uint64_t i = clamp_end + 1; i < file_num_blocks; i++)
//     {
//         if (lseek(fd_vf, 0, SEEK_END) == -1)
//             return -errno;
//         if (write(fd_vf, &value, sizeof(value)) != sizeof(value))
//             return -errno;
//     }
//     return 0;
// }
int store_blocks(size_t write_size, off_t offset, int fd_disk, int fd_file, int fd_vt, int fd_vf, uint64_t version)
{
    struct stat st;
    if (fstat(fd_file, &st) == -1)
        return -errno;

    off_t file_size = st.st_size;

    uint64_t affected_interval_left = offset / BLOCK_SIZE;

    uint64_t affected_interval_right = (file_size - 1 < (offset + write_size) - 1)
                                           ? ((file_size - 1 + BLOCK_SIZE - 1) / BLOCK_SIZE)
                                           : (((offset + write_size) - 1 + BLOCK_SIZE - 1) / BLOCK_SIZE);

    off_t disk_pos = lseek(fd_disk, 0, SEEK_END);
    if (disk_pos == -1)
        return -errno;

    char buffer[BLOCK_SIZE];

    uint64_t vf_offset;
    if (pread(fd_vt, &vf_offset, sizeof(vf_offset), version * sizeof(uint64_t)) == -1)
        return -errno;

    vf_offset += 2 * sizeof(uint64_t); // skip version number(ts) and version size

    for (uint64_t i = affected_interval_left; i < affected_interval_right; i++)
    {
        if (pread(fd_file, buffer, BLOCK_SIZE, i * BLOCK_SIZE) == -1)
            return -errno;

        if (write(fd_disk, buffer, BLOCK_SIZE) != BLOCK_SIZE)
            return -errno;

        uint64_t disk_block_id = disk_pos / BLOCK_SIZE;
        disk_pos += BLOCK_SIZE;

        if (pwrite(fd_vf, &disk_block_id, sizeof(disk_block_id), vf_offset + (i * sizeof (uint64_t))) == -1)
            return -errno;
    }

    uint64_t vf_end = (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    if (ftruncate(fd_vf, vf_offset + (vf_end * sizeof(uint64_t))) == -1)
        return -errno;

    return 0;
}
// int store_blocks(uint64_t *blocks, uint64_t count, int fd_disk, int fd_file, int fd_vt, int fd_vf, uint64_t version)
// {
//     struct stat st;
//     if (fstat(fd_file, &st) == -1)
//         return -errno;

//     off_t size = st.st_size;

//     if (write(fd_vf, &size, sizeof(size)) != sizeof(size))
//         return errno;
//     if (lseek(fd_vf, 0, SEEK_END) == -1)
//         return -errno;

//     // intersection of existing blocks and blocks to write
//     uint64_t file_num_blocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;

//     uint64_t write_start = offset / BLOCK_SIZE;
//     uint64_t write_end = (offset + size - 1) / BLOCK_SIZE;

//     uint64_t clamp_start = write_start;
//     uint64_t clamp_end = write_end < file_num_blocks - 1 ? write_end : file_num_blocks - 1;

//     // no intersection at all (write is fully past EOF)
//     if (write_start >= file_num_blocks)
//         return -EINVAL; // or handle as extending the file

//     uint64_t diff = clamp_end - clamp_start + 1;

//     uint64_t value = version - 1;
//     assert(value == GET_VALUE(value));
//     value = MAKE_VERSION(value);
//     for (uint64_t i = 0; i < blocks[0]; i++)
//     {
//         if (write(fd_vf, &value, sizeof(value)) != sizeof(value))
//             return -errno;
//         if (lseek(fd_vf, 0, SEEK_END) == -1)
//             return -errno;
//     }

//     for (uint64_t i = 0; i < diff; i++)
//     {
//         uint64_t block = blocks[i];
//         // for readability
//         char buffer[BLOCK_SIZE] = {'\0'};

//         if (lseek(fd_file, block * BLOCK_SIZE, SEEK_SET) == -1)
//             return -errno;
//         if (read(fd_file, buffer, sizeof(buffer)) == -1)
//             return -errno;
//         if (write(fd_disk, buffer, sizeof(buffer)) == -1)
//             return -errno;
//         uint64_t whence_disk = (lseek(fd_disk, 0, SEEK_END) / BLOCK_SIZE) - 1;
//         if (write(fd_vf, &whence_disk, sizeof(whence_disk)) == -1)
//             return -errno;
//     }
//     return 0;
// }

// uint64_t *count_affected_blocks(size_t size, off_t offset, uint64_t *ba)
// {
//     uint64_t first_block = offset / BLOCK_SIZE;
//     uint64_t last_block = (offset + size - 1) / BLOCK_SIZE;
//     uint64_t blocks_amount = last_block - first_block + 1;
//     *ba = blocks_amount;
//     //!!!!!!!!!!!!!!!!!!!!!!!!!!
//     uint64_t *changed_blocks = malloc(blocks_amount * sizeof(uint64_t));
//     if (!changed_blocks)
//         return NULL;
//     uint64_t k = first_block;
//     for (uint64_t i = 0; i < blocks_amount; i++)
//     {
//         changed_blocks[i] = k;
//         k++;
//     }
//     return changed_blocks;
// }