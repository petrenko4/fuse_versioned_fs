#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include "fs_helper.h"
#include "vfile_handler.h"
#include "vtable_handler.h"

void usage()
{
    printf("Usage:\n");
    printf("  versionview list <file>\n");
    printf("  versionview read <file> <version>\n");
    printf("  versionview read <file> @<timestamp>   (unix timestamp or partial, e.g. @1712920000)\n");
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
        return -errno;

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

void print_version_info(uint64_t version, uint64_t size_bytes, uint64_t timestamp)
{
    double size = (double)size_bytes;
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;

    while (size >= 1024 && unit < 4)
    {
        size /= 1024;
        unit++;
    }

    time_t ts = (time_t)timestamp;
    char timebuf[32];
    struct tm *tm_info = localtime(&ts);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);

    printf("Version %-5llu  Size: %7.2f %-2s  Time: %s  (@%llu)\n",
           (unsigned long long)version,
           size,
           units[unit],
           timebuf,
           (unsigned long long)timestamp);
}

int cmd_list(struct my_file_handle *myfh)
{
    uint64_t versions_count = 0;
    if (pread(myfh->fd_vt, &versions_count, sizeof(versions_count), 0) != sizeof(versions_count))
        return errno;

    printf("Version count: %llu\n", (unsigned long long)versions_count);
    for (uint64_t i = 1; i <= versions_count; i++)
    {
        uint64_t version_offset = 0;
        if (pread(myfh->fd_vt, &version_offset, sizeof(version_offset), i * sizeof(uint64_t)) == -1)
            return errno;

        uint64_t timestamp = 0;
        if (pread(myfh->fd_vf, &timestamp, sizeof(timestamp), version_offset) == -1)
            return errno;

        uint64_t version_size = 0;
        if (pread(myfh->fd_vf, &version_size, sizeof(version_size), version_offset + sizeof(uint64_t)) == -1)
            return errno;

        print_version_info(i, version_size, timestamp);
    }
    return 0;
}

// find the closest version at or before the given timestamp
uint64_t find_version_by_timestamp(struct my_file_handle *myfh, uint64_t target_ts)
{
    uint64_t versions_count = 0;
    if (pread(myfh->fd_vt, &versions_count, sizeof(versions_count), 0) != sizeof(versions_count))
        return (uint64_t)-1;

    uint64_t best_version = (uint64_t)-1;
    uint64_t best_ts = 0;

    for (uint64_t i = 1; i <= versions_count; i++)
    {
        uint64_t version_offset = 0;
        if (pread(myfh->fd_vt, &version_offset, sizeof(version_offset), i * sizeof(uint64_t)) == -1)
            continue;

        uint64_t ts = 0;
        if (pread(myfh->fd_vf, &ts, sizeof(ts), version_offset) == -1)
            continue;

        // find closest version whose timestamp <= target_ts
        if (ts <= target_ts && ts >= best_ts)
        {
            best_ts = ts;
            best_version = i;
        }
    }
    return best_version;
}

uint64_t check_version(struct my_file_handle *myfh, uint64_t version, uint64_t version_max, uint64_t block_number)
{
    if (version > version_max)
        return (uint64_t)-1; // exceeded all versions, read from current file

    uint64_t vf_off = 0;
    if (pread(myfh->fd_vt, &vf_off, sizeof(vf_off), version * sizeof(uint64_t)) == -1)
        return (uint64_t)-1;

    uint64_t value = 0;
    if (pread(myfh->fd_vf, &value, sizeof(value), vf_off + 2 * sizeof(uint64_t) + block_number * sizeof(uint64_t)) == -1)
        return (uint64_t)-1;

    if (value != 0)
        return value - 1; // found disk address

    return check_version(myfh, version + 1, version_max, block_number); // go forward
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

    // skip timestamp, read file size
    uint64_t size = 0;
    if (pread(myfh->fd_vf, &size, sizeof(size), vf_offset + sizeof(uint64_t)) == -1)
        return errno;
    uint64_t num_blocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    char disk_file[MAX_PATH_LEN + 38];
    snprintf(disk_file, sizeof(disk_file), "/workspaces/fuse_versioned_fs/fuse/%s.d", myfh->path);
    int fd_disk = open(disk_file, O_RDWR);
    if (fd_disk == -1)
        return errno;

    for (uint64_t block_index = 0; block_index < num_blocks; block_index++)
    {
        int is_last = (block_index == num_blocks - 1);
        size_t bytes_to_write = (is_last && size % BLOCK_SIZE != 0)
                                    ? size % BLOCK_SIZE
                                    : BLOCK_SIZE;

        uint64_t value = 0;
        off_t entry_off = vf_offset + 2 * sizeof(uint64_t) + block_index * sizeof(uint64_t);
        if (pread(myfh->fd_vf, &value, sizeof(value), entry_off) == -1)
        {
            close(fd_disk);
            return errno;
        }

        uint64_t disk_block;
        if (value != 0)
        {
            disk_block = value - 1;
        }
        else
        {
            disk_block = check_version(myfh, version + 1, version_max, block_index);
            if (disk_block == (uint64_t)-1)
            {
                // block never changed after this version — read from current file
                if (pread(myfh->fd_file, buffer, BLOCK_SIZE, block_index * BLOCK_SIZE) == -1)
                {
                    close(fd_disk);
                    return errno;
                }
                if (write(STDOUT_FILENO, buffer, bytes_to_write) == -1)
                {
                    close(fd_disk);
                    return errno;
                }
                continue;
            }
        }

        if (pread(fd_disk, buffer, BLOCK_SIZE, disk_block * BLOCK_SIZE) == -1)
        {
            close(fd_disk);
            return errno;
        }
        if (write(STDOUT_FILENO, buffer, bytes_to_write) == -1)
        {
            close(fd_disk);
            return errno;
        }
    }

    close(fd_disk);
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

        struct my_file_handle *myfh = malloc(sizeof(struct my_file_handle));
        if (open_internals(file, myfh) != 0)
        {
            fprintf(stderr, "Failed to open internal version files for %s\n", file);
            return 1;
        }

        uint64_t version;
        const char *version_arg = argv[3];

        if (version_arg[0] == '@')
        {
            // timestamp-based lookup
            uint64_t target_ts = strtoull(version_arg + 1, NULL, 10);
            version = find_version_by_timestamp(myfh, target_ts);
            if (version == (uint64_t)-1)
            {
                fprintf(stderr, "No version found at or before timestamp %llu\n",
                        (unsigned long long)target_ts);
                close_internals(myfh);
                return 1;
            }
            fprintf(stderr, "Using version %llu\n", (unsigned long long)version);
        }
        else
        {
            version = strtoull(version_arg, NULL, 10);
        }

        int ret = cmd_read(myfh, version);
        close_internals(myfh);
        return ret;
    }
    else
    {
        usage();
        return 1;
    }
}