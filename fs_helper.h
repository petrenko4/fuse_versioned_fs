#ifndef HELPER_H
#define HELPER_H
#define BLOCK_SIZE 16

#include <stdio.h>
#include <string.h>
#include "stdint.h"
#include <limits.h>

struct my_file_handle {
    int fd_file;
    int fd_vf;
    int fd_vt;                 
    int fd_disk;
    int is_virtual;
    char path[PATH_MAX];    
};

int is_internal_file(const char *path);

#endif