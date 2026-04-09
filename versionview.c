#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include "fs_helper.h"
#include "vfile_handler.h"
#include "vtable_handler.h"

void usage()
{
    printf("Usage:\n");
    printf("  versionview list <file>\n");
    printf("  versionview read <file> <version>\n");
}

int open_internals(const char *file, struct my_file_handle *myfh)
{
    strncpy(myfh->path, file, MAX_PATH_LEN - 1);
    myfh->path[MAX_PATH_LEN - 1] = '\0';
    char version_file[MAX_PATH_LEN + 3];
    char version_table[MAX_PATH_LEN + 3];
    char new_path[MAX_PATH_LEN];
    append_path(file, new_path);

    snprintf(version_file, sizeof(version_file), "%s.vf", new_path);
    snprintf(version_table, sizeof(version_table), "%s.vt", new_path);

    myfh->fd_vf = open(version_file, O_RDWR, 0644);
    if (myfh->fd_vf == -1)
        return -errno;

    myfh->fd_vt = open(version_table, O_RDWR, 0644);
    if (myfh->fd_vt == -1)
    {
        return -errno;
    }
    myfh->fd_file = open(new_path, O_RDWR, 0644);
    if (myfh->fd_file == -1)
        return -errno;

    return 0;
}

void close_internals(struct my_file_handle *myfh)
{
    close(myfh->fd_file);
    close(myfh->fd_vf);
    close(myfh->fd_vt);
    free(myfh);
}

void print_version_info(uint64_t version, uint64_t size_bytes)
{
    double size = (double)size_bytes;
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;

    while (size >= 1024 && unit < 4)
    {
        size /= 1024;
        unit++;
    }

    printf("Version %-5llu  Size: %.2f %s\n",
           (unsigned long long)version,
           size,
           units[unit]);
}

int cmd_list(struct my_file_handle *myfh)
{
    uint64_t versions_count = 0;
    if (pread(myfh->fd_vt, &versions_count, sizeof(versions_count), 0) != sizeof(versions_count))
        return errno;

    printf("Verison count: %ld\n", versions_count);
    for (uint64_t i = 1; i <= versions_count; i++)
    {
        uint64_t version_offset = 0;
        if (pread(myfh->fd_vt, &version_offset, sizeof(version_offset), i * sizeof(uint64_t)) == -1)
            return errno;
        uint64_t version_number = 0;

        if (pread(myfh->fd_vf, &version_number, sizeof(version_number), version_offset) == -1)
            return errno;

        uint64_t version_size = 0;
        if (pread(myfh->fd_vf, &version_size, sizeof(version_size), version_offset + sizeof(version_offset)) == -1)
            return errno;

        print_version_info(version_number, version_size);
    }
    return 0;
}

// traverse previous versions untill pointer to disk found
uint64_t check_version(struct my_file_handle *myfh, uint64_t version, uint64_t version_max, uint64_t block_number)
{

    if (version > version_max)
        return -1;

    uint64_t vf_off = 0;
    if (pread(myfh->fd_vt, &vf_off, sizeof(vf_off), version * sizeof(uint64_t)) == -1)
        return errno;

    uint64_t value = 0;
    if (pread(myfh->fd_vf, &value, sizeof(value), vf_off + 2 * sizeof(uint64_t) + block_number * sizeof(uint64_t)) == -1)
        return errno;

    if (!IS_VERSION(value))
        return value;

    uint64_t res = check_version(myfh, GET_VALUE(value), version_max, block_number);

    // if (pwrite(myfh->fd_vf, &res, sizeof(value), vf_off + 2 * sizeof(uint64_t) + block_number * sizeof(uint64_t)) == -1)
    //     return errno;

    return res;
}

int cmd_read(struct my_file_handle *myfh, uint64_t version)
{
    uint64_t buffer[BLOCK_SIZE / sizeof(uint64_t)];
    uint64_t vf_offset = 0;
    if (pread(myfh->fd_vt, &vf_offset, sizeof(vf_offset), version * sizeof(uint64_t)) == -1)
        return errno;
    uint64_t version_max = 0;
    if (pread(myfh->fd_vt, &version_max, sizeof(uint64_t), 0) == -1)
        return errno;

    uint64_t size = 0;

    if (pread(myfh->fd_vf, &size, sizeof(size), vf_offset + sizeof(uint64_t)) == -1)
        return errno;

    uint64_t next_version_off = vf_offset + 2 * sizeof(uint64_t) + ((size + BLOCK_SIZE - 1) / BLOCK_SIZE) * sizeof(uint64_t);

    vf_offset += 2 * sizeof(uint64_t);

    char disk_file[MAX_PATH_LEN + 38];

    snprintf(disk_file, sizeof(disk_file), "/workspaces/fuse_versioned_fs/fuse/%s.d", myfh->path);

    for (uint64_t i = vf_offset; i < next_version_off; i += sizeof(uint64_t))
    {
        uint64_t value = 0;
        if (pread(myfh->fd_vf, &value, sizeof(value), i) == -1)
            return errno;

        uint64_t num_blocks = (next_version_off - vf_offset) / sizeof(uint64_t);
        uint64_t block_index = (i - vf_offset) / sizeof(uint64_t);
        int is_last = (block_index == num_blocks - 1);
        size_t bytes_to_write = (is_last && size % BLOCK_SIZE != 0)
                                    ? size % BLOCK_SIZE
                                    : BLOCK_SIZE;
        if (!IS_VERSION(value))
        {
            char *disk_path = disk_file;
            int fd_disk = open(disk_path, O_RDWR);
            if (pread(fd_disk, &buffer, BLOCK_SIZE, value * BLOCK_SIZE) == -1)
                return errno;
            if (write(STDOUT_FILENO, &buffer, bytes_to_write) == -1)
                return errno;
        }
        else
        {

            char *disk_path = disk_file;
            int fd_disk = open(disk_path, O_RDWR);
            uint64_t relevant_block = check_version(myfh, version, version_max,((i - vf_offset) / sizeof(uint64_t)));
            if (relevant_block == -1)
            {
                if (pread(myfh->fd_file, &buffer, BLOCK_SIZE, ((i - vf_offset)/sizeof(uint64_t))*BLOCK_SIZE) == -1)
                    return errno;
                if (write(STDOUT_FILENO, &buffer, bytes_to_write) == -1)
                    return errno;
            }
            else
            {
                if (pread(fd_disk, &buffer, BLOCK_SIZE, relevant_block * BLOCK_SIZE) == -1)
                    return errno;

                if (write(STDOUT_FILENO, &buffer, bytes_to_write) == -1)
                    return errno;
            }
            // if(pwrite(myfh->fd_vf, &relevant_block, sizeof(relevant_block), i) == -1)
            //     return errno;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        usage();
        return 1;
    }

    const char *cmd = argv[1];
    const char *file = argv[2];

    if (strcmp(cmd, "list") == 0)
    {
        if (argc != 3)
        {
            usage();
            return 1;
        }

        struct my_file_handle *myfh = malloc(sizeof(struct my_file_handle));
        if (open_internals(file, myfh) != 0)
        {
            fprintf(stderr, "Failed to open internal version files for %s\n", file);
            return 1;
        }

        int ret = cmd_list(myfh);

        close_internals(myfh);
        return ret;
    }
    else if (strcmp(cmd, "read") == 0)
    {
        if (argc != 4)
        {
            usage();
            return 1;
        }
        const char *version = argv[3];

        struct my_file_handle *myfh = malloc(sizeof(struct my_file_handle));
        if (open_internals(file, myfh) != 0)
        {
            fprintf(stderr, "Failed to open internal version files for %s\n", file);
            return 1;
        }

        int ret = cmd_read(myfh, strtoull(version, NULL, 10));

        close_internals(myfh);
        return ret;
    }
    else
    {
        usage();
        return 1;
    }
}