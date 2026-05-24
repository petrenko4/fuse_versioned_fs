#include <stdio.h>
#include <string.h>
#include "fs_helper.h"
#include "myfs.h"
#include "stdint.h"

int is_internal_file(const char *path) {
    if (!path) {
        return 0;  
    }
    size_t len = strlen(path);

    if (len < 3)   
        return 0;

    const char *suffix = path + len - 5;
    const char *dsuffix = path + len - 4;

    return (strcmp(suffix, "..vt.") == 0 ||
            strcmp(suffix, "..vf.") == 0 ||
            strcmp(dsuffix, "..d.") == 0);
}