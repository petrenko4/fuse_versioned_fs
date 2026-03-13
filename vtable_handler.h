#ifndef TABLE_HANDLER
#define TABLE_HANDLER

#include "stdint.h"
#include "stdio.h"

int save_entire_file(char* path, int fd_disk); 
int vt_init(uint64_t starting_version, int fd_vt);
int vt_insert(int fd_table, uint32_t version, off_t offset);
int update_version_counter(int fd_table);
uint64_t get_version(int fd_table);

#endif