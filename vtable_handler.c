#include "vtable_handler.h"
#include "fs_helper.h"
#include <stdint.h>
#include "fcntl.h"
#include "stdint.h"
#include "unistd.h"
#include "assert.h"
#include "errno.h"

int vt_init(uint64_t starting_verison, int fd_vt)
{
    uint64_t buffer[1];
    buffer[0] = starting_verison;
    if(write(fd_vt, buffer, sizeof(buffer)) == -1) 
        return -errno;

    return 0;
}

int vt_insert(int fd_table, uint32_t version, off_t offset)
{
    // assert((version * 8) == lseek(fd_table, 0, SEEK_CUR));
    off_t buf[1];
    buf[0] = offset;
    if (write(fd_table, buf, 1) == -1)
        perror("Could not write offset to version table");
    return 0;
}

int update_version_counter(int fd_table)
{
    uint64_t old_version;
    if (read(fd_table, &old_version, sizeof(old_version)) == -1)
        return -errno;
    uint64_t new_version = old_version + 1;
    lseek(fd_table, 0, SEEK_SET);
    if (write(fd_table, &new_version, sizeof(new_version)) == -1)
        return -errno;

    printf("Version updated");
    return 0;
}

uint64_t get_version(int fd_table)
{

    uint64_t version;
    if (read(fd_table, &version, sizeof(version)) == -1)
        return -errno;
}