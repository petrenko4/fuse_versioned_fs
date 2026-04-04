#ifndef HELPER_H
#define HELPER_H
#define BASE_PATH "/workspaces/fuse_versioned_fs/fuse/"
#define DISK_FILE "/workspaces/fuse_versioned_fs/disk"
#define MAX_PATH_LEN 1024
#define BLOCK_SIZE 16

#include <stdio.h>
#include <string.h>
#include "stdint.h"

struct my_file_handle {
    int fd_file;
    int fd_vf;
    int fd_vt;                 
    int fd_disk;
    char path[MAX_PATH_LEN];    
};
void append_path(const char *path, char *out);
int is_internal_file(const char *path);

#endif