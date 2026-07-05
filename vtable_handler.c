#include "vtable_handler.h"
#include "fs_helper.h"
#include <stdint.h>
#include "fcntl.h"
#include "stdint.h"
#include "unistd.h"
#include "assert.h"
#include "errno.h"

int vt_init(uint64_t starting_version, int fd_vt)
{
    uint64_t buffer[1];
    buffer[0] = starting_version;
    if(write(fd_vt, buffer, sizeof(buffer)) == -1) 
        return -errno;

    return 0;
}

uint64_t update_version_counter(int fd_table)
{
    uint64_t old_version;
    if (pread(fd_table, &old_version, sizeof(old_version), 0) == -1)
        return -errno;
    uint64_t new_version = old_version + 1;
    if (pwrite(fd_table, &new_version, sizeof(new_version), 0) == -1)
        return -errno;

    return new_version;
}