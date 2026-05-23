/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/

#define FUSE_USE_VERSION 31

#define _GNU_SOURCE

#include <fuse.h>

#ifdef HAVE_LIBULOCKMGR
#include <ulockmgr.h>
#endif
#include "fs_helper.h"
#include "vtable_handler.h"
#include "vfile_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif
#include <sys/file.h> /* flock(2) */

int root_fd;

static void *xmp_init(struct fuse_conn_info *conn,
                      struct fuse_config *cfg)
{
        (void)conn;
        cfg->use_ino = 1;
        cfg->nullpath_ok = 1;

        // printf("[DEBUG] [myfs.c] xmp_init() called\n");

        /* parallel_direct_writes feature depends on direct_io features.
           To make parallel_direct_writes valid, need either set cfg->direct_io
           in current function (recommended in high level API) or set fi->direct_io
           in xmp_create() or xmp_open(). */
        // cfg->direct_io = 1;
        // cfg->parallel_direct_writes = 1;

        /* Pick up changes from lower filesystem right away. This is
           also necessary for better hardlink support. When the kernel
           calls the unlink() handler, it does not know the inode of
           the to-be-removed entry and can therefore not invalidate
           the cache of the associated inode - resulting in an
           incorrect st_nlink value being reported for any remaining
           hardlinks to this inode. */
        cfg->entry_timeout = 0;
        cfg->attr_timeout = 0;
        cfg->negative_timeout = 0;

        return NULL;
}

int is_vls(const char *path)
{
        const char *vls_ext = strstr(path, ".vls");

        if (vls_ext)
        {
                if (vls_ext[4] == '\0' || (vls_ext[4] == '/' && vls_ext[5] == '\0'))
                {
                        return 1;
                }
        }
        return 0;
}

int is_virtfile(const char *path)
{
        if (strstr(path, ".vls/"))
                return 1;

        if(strchr(path, '@'))
                return 1;
        
        return 0;
}

int ends_with(const char *str, const char *suffix)
{
        if (!str || !suffix)
                return 0;

        size_t len_str = strlen(str);
        size_t len_suffix = strlen(suffix);

        if (len_suffix > len_str)
                return 0;

        return strcmp(str + len_str - len_suffix, suffix) == 0;
}

int fill_virtual_stats_dir(struct stat *stbuf)
{
        memset(stbuf, 0, sizeof(struct stat));

        stbuf->st_mode = S_IFDIR | 0555;

        stbuf->st_nlink = 2;

        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();

        stbuf->st_atime = stbuf->st_mtime = stbuf->st_ctime = time(NULL);

        stbuf->st_size = 4096;
}

int fill_virtual_stats_reg(struct stat *stbuf, struct my_file_handle *myfh, uint64_t version)
{
        if (myfh)
        {
                stbuf->st_mode = S_IFREG | 0444;

                stbuf->st_nlink = 1;

                stbuf->st_uid = getuid();
                stbuf->st_gid = getgid();

                uint64_t vf_offset;
                if (pread(myfh->fd_vt, &vf_offset, sizeof(vf_offset), version * sizeof(uint64_t)) == -1)
                        return -errno;
                uint64_t file_size;
                uint64_t timestamp;
                if (pread(myfh->fd_vf, &file_size, sizeof(file_size), (vf_offset + sizeof(uint64_t))) == -1)
                        return -errno;
                if (pread(myfh->fd_vf, &timestamp, sizeof(timestamp), vf_offset) == -1)
                        return -errno;
                stbuf->st_size = file_size;
                stbuf->st_mtime = stbuf->st_atime = stbuf->st_ctime = timestamp;
        }
}

int fill_virtual_stats_reg_nofh(struct stat *stbuf, char *path)
{
        char *actual_path = strdup(path);
        char *at_ptr = strrchr(actual_path, '@');
        uint64_t version;
        if (at_ptr != NULL)
        {
                char *endptr;
                version = strtoull(at_ptr + 1, &endptr, 10);
        }
        else
                return -ENOENT;

        const char *rel_path = (actual_path[0] == '/') ? actual_path + 1 : actual_path;

        char base_path[PATH_MAX];
        strncpy(base_path, rel_path, sizeof(base_path) - 1);
        base_path[sizeof(base_path) - 1] = '\0';

        char *ext = strstr(base_path, ".vls");
        if (ext)
                *ext = '\0';

        char vt_name[PATH_MAX];
        snprintf(vt_name, sizeof(vt_name), "%s..vt.", base_path);
        char vf_name[PATH_MAX];
        snprintf(vf_name, sizeof(vf_name), "%s..vf.", base_path);
        int fd_vt = openat(root_fd, vt_name, O_RDONLY);
        if (fd_vt == -1)
                return -errno;
        int fd_vf = openat(root_fd, vf_name, O_RDONLY);
        if (fd_vf == -1)
                return -errno;

        stbuf->st_mode = S_IFREG | 0444;

        stbuf->st_nlink = 1;

        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();

        uint64_t vf_offset;
        if (pread(fd_vt, &vf_offset, sizeof(vf_offset), version * sizeof(uint64_t)) == -1)
                return -errno;
        uint64_t file_size;
        uint64_t timestamp;
        if (pread(fd_vf, &file_size, sizeof(file_size), vf_offset + sizeof(uint64_t)) == -1)
                return -errno;
        if (pread(fd_vf, &timestamp, sizeof(timestamp), vf_offset) == -1)
                return -errno;
        stbuf->st_size = file_size;
        stbuf->st_mtime = stbuf->st_atime = stbuf->st_ctime = timestamp;
        close(fd_vf);
        close(fd_vt);
        free(actual_path);
}
static int xmp_getattr(const char *path, struct stat *stbuf,
                       struct fuse_file_info *fi)
{
        char *actual_path;
        if (fi && fi->fh)
        {
                struct my_file_handle *myfh = (struct my_file_handle *)fi->fh;
                if (myfh->is_virtual)
                {
                        if (is_virtfile(myfh->path))
                        {
                                char *at_ptr = strrchr(myfh->path, '@');
                                uint64_t version_num = 0;
                                if (at_ptr != NULL)
                                {
                                        char *endptr;
                                        version_num = strtoull(at_ptr + 1, &endptr, 10);
                                }
                                fill_virtual_stats_reg(stbuf, myfh, version_num);
                                return 0;
                        }
                        else
                        {
                                fill_virtual_stats_dir(stbuf);
                                return 0;
                        }
                }
                if (myfh->path)
                {
                        actual_path = myfh->path;
                }
                else
                        return 0;
        }
        else
        {
                if (path == NULL)
                {
                        actual_path = NULL;
                }
                else
                {
                        actual_path = strdup(path);
                        if (!actual_path)
                                return -ENOMEM;
                }
        }
        int res;

        if (is_internal_file(actual_path))
                return -ENOENT;
        if (is_vls(actual_path))
        {
                fill_virtual_stats_dir(stbuf);
                return 0;
        }
        if (is_virtfile(actual_path))
        {

                fill_virtual_stats_reg_nofh(stbuf, actual_path);
                return 0;
        }
        if (fi)
        {
                struct my_file_handle *myfh = (struct my_file_handle *)fi->fh;
                res = fstat(myfh->fd_file, stbuf);
        }
        else
        {
                const char *rel_path = (path[0] == '/') ? path + 1 : path;

                if (rel_path[0] == '\0')
                        rel_path = ".";

                // printf("[DEBUG] [myfs.c] xmp_getattr() called for: %s\n", rel_path);

                res = fstatat(root_fd, rel_path, stbuf, AT_SYMLINK_NOFOLLOW);
        }

        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_access(const char *path, int mask)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        if (is_vls(path))
        {
                return 0;
        }
        char *at_sign = strrchr(path, '@');
        if (at_sign)
                return 0;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        printf("[DEBUG] [myfs.c] xmp_access() called\n");

        res = faccessat(root_fd, rel_path, mask, 0);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        printf("[DEBUG] [myfs.c] xmp_readlink() called\n");

        res = readlinkat(root_fd, path, buf, size - 1);
        if (res == -1)
                return -errno;

        buf[res] = '\0';
        return 0;
}

struct xmp_dirp
{
        DIR *dp;
        struct dirent *entry;
        off_t offset;
        int is_virtual;
        char path[PATH_MAX];
};

static int xmp_opendir(const char *path, struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        if (strstr(path, ".vls"))
        {
                struct xmp_dirp *d = malloc(sizeof(struct xmp_dirp));
                if (!d)
                        return -ENOMEM;

                d->dp = NULL;
                d->is_virtual = 1;
                d->offset = 0;
                d->entry = NULL;
                strncpy(d->path, path, PATH_MAX - 1);
                d->path[PATH_MAX - 1] = '\0';

                fi->fh = (uintptr_t)d;

                return 0;
        }
        struct xmp_dirp *d = malloc(sizeof(struct xmp_dirp));
        if (d == NULL)
                return -ENOMEM;

        strncpy(d->path, path, PATH_MAX - 1);
        d->path[PATH_MAX - 1] = '\0';

        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        printf("[DEBUG] [myfs.c] xmp_opendir() called\n");
        int sub_fd = openat(root_fd, rel_path, O_RDONLY | O_DIRECTORY);
        if (sub_fd == -1)
                return -errno;
        d->dp = fdopendir(sub_fd);
        if (d->dp == NULL)
        {
                res = -errno;
                free(d);
                return res;
        }
        d->offset = 0;
        d->entry = NULL;
        d->is_virtual = 0;
        fi->fh = (unsigned long)d;
        return 0;
}

static inline struct xmp_dirp *get_dirp(struct fuse_file_info *fi)
{
        return (struct xmp_dirp *)(uintptr_t)fi->fh;
}

static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags)
{
        struct xmp_dirp *d = get_dirp(fi);

        if (d->is_virtual)
        {
                const char *my_path = d->path;
                const char *rel_path = (my_path[0] == '/') ? my_path + 1 : my_path;

                char base_path[PATH_MAX];
                strncpy(base_path, rel_path, sizeof(base_path) - 1);
                base_path[sizeof(base_path) - 1] = '\0';

                char *ext = strstr(base_path, ".vls");
                if (ext)
                        *ext = '\0';

                char vt_name[PATH_MAX];
                snprintf(vt_name, sizeof(vt_name), "%s..vt.", base_path);
                char vf_name[PATH_MAX];
                snprintf(vf_name, sizeof(vf_name), "%s..vf.", base_path);
                int fd_vt = openat(root_fd, vt_name, O_RDONLY);
                if (fd_vt == -1)
                        return -errno;
                int fd_vf = openat(root_fd, vf_name, O_RDONLY);
                if (fd_vf == -1)
                        return -errno;
                uint64_t version_count;
                if (pread(fd_vt, &version_count, sizeof(version_count), 0) == -1)
                {
                        close(fd_vt);
                        close(fd_vf);
                        return -errno;
                }
                for (uint64_t i = 0; i < version_count; i++)
                {
                        const char *filename = strrchr(base_path, '/');

                        if (filename)
                        {
                                filename++;
                        }
                        else
                        {
                                filename = base_path;
                        }
                        char v_name[NAME_MAX];
                        snprintf(v_name, sizeof(v_name), "%s@%lu",
                                 filename, i + 1);

                        filler(buf, v_name, NULL, 0, 0);
                }
                filler(buf, ".", NULL, 0, 0);
                filler(buf, "..", NULL, 0, 0);
                return 0;
        }

        // printf("[DEBUG] [myfs.c] xmp_readdir() called\n");
        if (offset != d->offset)
        {
#ifndef __FreeBSD__
                seekdir(d->dp, offset);
#else
                /* Subtract the one that we add when calling
                   telldir() below */
                seekdir(d->dp, offset - 1);
#endif
                d->entry = NULL;
                d->offset = offset;
        }
        while (1)
        {
                struct stat st;
                off_t nextoff;
                enum fuse_fill_dir_flags fill_flags = FUSE_FILL_DIR_DEFAULTS;

                if (!d->entry)
                {
                        d->entry = readdir(d->dp);
                        if (!d->entry)
                                break;
                }

                if (is_internal_file(d->entry->d_name))
                {
                        d->entry = NULL;
                        d->offset = telldir(d->dp);
                        continue; // skip this entry
                }
#ifdef HAVE_FSTATAT
                if (flags & FUSE_READDIR_PLUS)
                {
                        int res;

                        res = fstatat(dirfd(d->dp), d->entry->d_name, &st,
                                      AT_SYMLINK_NOFOLLOW);
                        if (res != -1)
                                fill_flags |= FUSE_FILL_DIR_PLUS;
                }
#endif
                if (!(fill_flags & FUSE_FILL_DIR_PLUS))
                {
                        memset(&st, 0, sizeof(st));
                        st.st_ino = d->entry->d_ino;
				st.st_mode = d->entry->d_type << 12;
                }
                nextoff = telldir(d->dp);
#ifdef __FreeBSD__
                /* Under FreeBSD, telldir() may return 0 the first time
                   it is called. But for libfuse, an offset of zero
                   means that offsets are not supported, so we shift
                   everything by one. */
                nextoff++;
#endif
                if (filler(buf, d->entry->d_name, &st, nextoff, fill_flags))
                        break;
                if (S_ISREG(st.st_mode))
                {
                        char vls_dir[NAME_MAX + 5];
                        snprintf(vls_dir, sizeof(vls_dir), "%s.vls", d->entry->d_name);
                        // this could be whatever. just show fake entry
                        struct stat st_vls = st;
                        st_vls.st_mode = (st.st_mode & ~S_IFREG) | S_IFDIR;
                        st_vls.st_mode |= 0555;
                        st_vls.st_nlink = 2;
                        st_vls.st_size = 4096;

                        if (filler(buf, vls_dir, &st_vls, nextoff, FUSE_FILL_DIR_PLUS))
                                break;
                }
                d->entry = NULL;
                d->offset = nextoff;
        }

        return 0;
}

static int xmp_releasedir(const char *path, struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        struct xmp_dirp *d = get_dirp(fi);
        printf("[DEBUG] [myfs.c] xmp_releasedir() called\n");
        closedir(d->dp);
        free(d);
        return 0;
}

static int xmp_mknod(const char *path, mode_t mode, dev_t rdev)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        printf("[DEBUG] [myfs.c] xmp_mknod() called\n");

        if (S_ISFIFO(mode))
                res = mkfifoat(root_fd, rel_path, mode);
        else
                res = mknodat(root_fd, rel_path, mode, rdev);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
        if (is_internal_file(path))
                return -ENOENT;
        if(is_vls(path))
                return -EPERM;
        int res;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        printf("[DEBUG] [myfs.c] xmp_mkdir() called\n");
        res = mkdirat(root_fd, rel_path, mode);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_unlink(const char *path)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        char version_file[PATH_MAX + 3];
        char version_table[PATH_MAX + 3];
        char disk_file[PATH_MAX + 2];

        snprintf(version_file, sizeof(version_file), "%s..vf.", rel_path);
        snprintf(version_table, sizeof(version_table), "%s..vt.", rel_path);
        snprintf(disk_file, sizeof(disk_file), "%s..d.", rel_path);

        printf("[DEBUG] [myfs.c] xmp_unlink() called\n");
        res = unlinkat(root_fd, version_file, 0);
        if (res == -1)
                return -errno;
        res = unlinkat(root_fd, version_table, 0);
        if (res == -1)
                return -errno;
        res = unlinkat(root_fd, disk_file, 0);
        if (res == -1)
                return -errno;
        res = unlinkat(root_fd, rel_path, 0);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_rmdir(const char *path)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        printf("[DEBUG] [myfs.c] xmp_rmdir() called\n");
        res = unlinkat(root_fd, rel_path, AT_REMOVEDIR);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
        if (is_internal_file(from) || is_internal_file(to))
                return -ENOENT;
        int res;
        const char *rel_path_from = (from[0] == '/') ? from + 1 : from;
        if (rel_path_from[0] == '\0')
                rel_path_from = ".";
        const char *rel_path_to = (to[0] == '/') ? to + 1 : to;
        if (rel_path_to[0] == '\0')
                rel_path_to = ".";
        printf("[DEBUG] [myfs.c] xmp_symlink() called\n");
        res = symlinkat(rel_path_from, root_fd, rel_path_to);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_rename(const char *from, const char *to, unsigned int flags)
{
        if (is_internal_file(from) || is_internal_file(to))
                return -ENOENT;
        int res;
        const char *rel_path_from = (from[0] == '/') ? from + 1 : from;
        if (rel_path_from[0] == '\0')
                rel_path_from = ".";
        const char *rel_path_to = (to[0] == '/') ? to + 1 : to;
        if (rel_path_to[0] == '\0')
                rel_path_to = ".";
        printf("[DEBUG] [myfs.c] xmp_rename() called\n");
        /* When we have renameat2() in libc, then we can implement flags */
        if (flags)
                return -EINVAL;

        char version_file_from[PATH_MAX];
        char version_table_from[PATH_MAX];
        char disk_from[PATH_MAX];
        char version_file_to[PATH_MAX];
        char version_table_to[PATH_MAX];
        char disk_to[PATH_MAX];

        snprintf(version_file_from, sizeof(version_file_from), "%s..vf.", rel_path_from);
        snprintf(version_table_from, sizeof(version_table_from), "%s..vt.", rel_path_from);
        snprintf(disk_from, sizeof(disk_from), "%s..d.", rel_path_from);
        snprintf(version_file_to, sizeof(version_file_to), "%s..vf.", rel_path_to);
        snprintf(version_table_to, sizeof(version_table_to), "%s..vt.", rel_path_to);
        snprintf(disk_to, sizeof(disk_to), "%s..d.", rel_path_from);

        res = renameat(root_fd, version_file_from, root_fd, version_file_to);
        if (res == -1)
                return errno;

        res = renameat(root_fd, version_table_from, root_fd, version_table_to);
        if (res == -1)
                return errno;

        res = renameat(root_fd, rel_path_from, root_fd, rel_path_to);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_link(const char *from, const char *to)
{
        if (is_internal_file(from) || is_internal_file(to))
                return -ENOENT;
        int res;
        const char *rel_path_from = (from[0] == '/') ? from + 1 : from;
        if (rel_path_from[0] == '\0')
                rel_path_from = ".";
        const char *rel_path_to = (to[0] == '/') ? to + 1 : to;
        if (rel_path_to[0] == '\0')
                rel_path_to = ".";
        printf("[DEBUG] [myfs.c] xmp_link() called\n");
        res = linkat(root_fd, rel_path_from, root_fd, rel_path_to, 0);

        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_chmod(const char *path, mode_t mode,
                     struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        printf("[DEBUG] [myfs.c] xmp_chmod() called\n");

        if (fi)
                res = fchmod(fi->fh, mode);
        else
                res = fchmodat(root_fd, rel_path, mode, 0);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid,
                     struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        printf("[DEBUG] [myfs.c] xmp_chown() called\n");
        if (fi)
                res = fchown(fi->fh, uid, gid);
        else
                res = fchownat(root_fd, rel_path, uid, gid, 0);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi)
{
        int res;

        printf("[DEBUG] [myfs.c] xmp_truncate() called\n");
        char *actual_path;
        struct my_file_handle *myfh;
        if (fi)
        {
                myfh = (struct my_file_handle *)fi->fh;
                actual_path = myfh->path;
        }
        else
        {
                actual_path = strdup(path);
                if (!actual_path)
                        return -ENOMEM;
        }
        if (is_internal_file(actual_path))
                return -ENOENT;
        const char *rel_path = (actual_path[0] == '/') ? actual_path + 1 : actual_path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        if (myfh)
                res = ftruncate(myfh->fd_file, size);
        else
        {
                int sub_fd = openat(root_fd, rel_path, O_RDWR);
                if (sub_fd == -1)
                        return -errno;
                res = ftruncate(sub_fd, size);
                close(sub_fd);
        }

        if (res == -1)
                return -errno;
        return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2],
                       struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        printf("[DEBUG] [myfs.c] xmp_utimens() called\n");

        /* don't use utime/utimes since they follow symlinks */
        if (fi)
                res = futimens(fi->fh, ts);
        else
                res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
        if (res == -1)
                return -errno;

        return 0;
}
#endif

// TO FIX. MY FILE HANDLER SHOULD BE INITIALIZED HERE AS IN xmp_open
static int xmp_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        if(is_vls(path) || is_virtfile(path))
                return -EINVAL;
        int fd;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        printf("[DEBUG] [myfs.c] xmp_create() called\n");
        fd = openat(root_fd, rel_path, (fi->flags & ~O_ACCMODE) | O_RDWR, mode);
        if (fd == -1)
                return -errno;

        char version_file[PATH_MAX + 3];
        char version_table[PATH_MAX + 3];
        char disk_file[PATH_MAX + 2];

        snprintf(version_file, sizeof(version_file), "%s..vf.", rel_path);
        snprintf(version_table, sizeof(version_table), "%s..vt.", rel_path);
        snprintf(disk_file, sizeof(disk_file), "%s..d.", rel_path);

        int fd_vf = openat(root_fd, version_file, O_CREAT | O_RDWR, 0644);
        if (fd_vf == -1)
                return -errno;

        int fd_vt = openat(root_fd, version_table, O_CREAT | O_RDWR, 0644);
        if (fd_vt == -1)
                return -errno;

        int fd_disk = openat(root_fd, disk_file, O_CREAT | O_RDWR | O_APPEND, 0644);
        if (fd_disk == -1)
                return -errno;

        vt_init(0, fd_vt);
        struct my_file_handle *h = malloc(sizeof(struct my_file_handle));
        if (!h)
                return -ENOMEM;
        h->fd_file = fd;
        h->fd_vf = fd_vf;
        h->fd_vt = fd_vt;
        h->fd_disk = fd_disk;
        strncpy(h->path, rel_path, PATH_MAX - 1);
        h->path[PATH_MAX - 1] = '\0';
        h->is_virtual = 0;
        fi->fh = (uint64_t)h;
        return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        struct my_file_handle *h = malloc(sizeof(struct my_file_handle));
        if (!h)
                return -ENOMEM;
        if (is_virtfile(path))
        {
                const char *rel_ptr = (path[0] == '/') ? path + 1 : path;
                char base_path[PATH_MAX];
                strncpy(base_path, rel_ptr, sizeof(base_path) - 1);
                base_path[sizeof(base_path) - 1] = '\0';

                char *vls_ptr = strstr(base_path, ".vls/");
                *vls_ptr = '\0';
                char version_file[PATH_MAX + 3];
                char version_table[PATH_MAX + 3];
                char disk_file[PATH_MAX + 2];

                snprintf(version_file, sizeof(version_file), "%s..vf.", base_path);
                snprintf(version_table, sizeof(version_table), "%s..vt.", base_path);
                snprintf(disk_file, sizeof(disk_file), "%s..d.", base_path);

                h->fd_vf = openat(root_fd, version_file, O_RDWR, 0644);
                if (h->fd_vf == -1)
                        return -errno;

                h->fd_vt = openat(root_fd, version_table, O_RDONLY, 0644);
                if (h->fd_vt == -1)
                        return -errno;

                h->fd_disk = openat(root_fd, disk_file, O_RDONLY, 0644);
                if (h->fd_disk == -1)
                        return -errno;
                h->fd_file = openat(root_fd, base_path, O_RDONLY, 0644);
                if (h->fd_disk == -1)
                        return -errno;
                struct my_file_handle *myfh = malloc(sizeof(struct my_file_handle));
                if (!myfh)
                        return -ENOMEM;
                h->is_virtual = 1;
                strncpy(h->path, path, PATH_MAX - 1);
                h->path[PATH_MAX - 1] = '\0';
                fi->fh = (uint64_t)h;
                return 0;
        }

        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        printf("[DEBUG] [myfs.c] xmp_open() called\n");

        char version_file[PATH_MAX + 3];
        char version_table[PATH_MAX + 3];
        char disk_file[PATH_MAX + 2];

        snprintf(version_file, sizeof(version_file), "%s..vf.", rel_path);
        snprintf(version_table, sizeof(version_table), "%s..vt.", rel_path);
        snprintf(disk_file, sizeof(disk_file), "%s..d.", rel_path);

        int fd_vf = openat(root_fd, version_file, O_CREAT | O_RDWR, 0644);
        if (fd_vf == -1)
                return -errno;

        int fd_vt = openat(root_fd, version_table, O_CREAT | O_RDWR, 0644);
        if (fd_vt == -1)
                return -errno;
        /* Enable direct_io when open has flags O_DIRECT to enjoy the feature
           parallel_direct_writes (i.e., to get a shared lock, not exclusive lock,
           for writes to the same file). */
        int fd_disk = openat(root_fd, disk_file, O_RDWR | O_APPEND, 0644);
        if (fd_disk == -1)
        {
                vt_init(0, fd_vt);
                fd_disk = openat(root_fd, disk_file, O_CREAT | O_RDWR | O_APPEND, 0644);
        }
        if (fd_disk == -1)
                return -errno;

        if (fi->flags & O_DIRECT)
        {
                fi->direct_io = 1;
                fi->parallel_direct_writes = 1;
        }

        h->fd_vf = fd_vf;
        h->fd_vt = fd_vt;
        h->fd_disk = fd_disk;
        strncpy(h->path, rel_path, PATH_MAX - 1);
        h->path[PATH_MAX - 1] = '\0';
        h->is_virtual = 0;
        fi->fh = (uint64_t)h;
        int trunced = 0;

        int fd_file = openat(root_fd, rel_path, O_RDWR, 0644);
        if (fd_file == -1)
                return -errno;
        struct stat sb;
        if (fstat(fd_file, &sb) == -1)
                return -errno;
        uint64_t file_size = sb.st_size;
        if (fi->flags & O_TRUNC)
        {
                trunced = 1;

                if (file_size > 0)
                {
                        uint64_t version = update_version_counter(h->fd_vt);
                        off_t vt_off = version * sizeof(uint64_t);

                        // update version table file
                        uint64_t offset_for_vf = lseek(h->fd_vf, 0, SEEK_END);
                        if (pwrite(h->fd_vt, &offset_for_vf, sizeof(offset_for_vf), vt_off) != sizeof(offset_for_vf))
                                return -errno;

                        // write version number to the version file
                        uint64_t timestamp = (uint64_t)time(NULL);

                        if (pwrite(h->fd_vf, &timestamp, sizeof(timestamp), offset_for_vf) != sizeof(timestamp))
                                return -errno;
                        if (pwrite(h->fd_vf, &file_size, sizeof(file_size), offset_for_vf + sizeof(uint64_t)) != sizeof(version))
                                return -errno;
                        uint64_t ceil_block = ((file_size + BLOCK_SIZE - 1) / BLOCK_SIZE);
                        uint64_t value = MAKE_VERSION(version + 1);
                        for (uint64_t i = 0; i < ceil_block; i += 1)
                        {
                                if (pwrite(fd_vf, &value, sizeof(value), (offset_for_vf + 2 * sizeof(uint64_t) + i * sizeof(uint64_t))) == -1)
                                        return -errno;
                        }

                        if (store_blocks(file_size, 0, h->fd_disk, fd_file, h->fd_vt, h->fd_vf) == -1)
                                return -errno;
                }
        }
        else
        {
                if (((fi->flags & O_RDWR) || (fi->flags & O_WRONLY)) && !trunced)
                {
                        if (file_size > 0)
                        {
                                uint64_t version = update_version_counter(h->fd_vt);
                                off_t vt_off = version * sizeof(uint64_t);

                                // update version table file
                                uint64_t offset_for_vf = lseek(h->fd_vf, 0, SEEK_END);
                                if (pwrite(h->fd_vt, &offset_for_vf, sizeof(offset_for_vf), vt_off) != sizeof(offset_for_vf))
                                        return -errno;

                                // write version number to the version file
                                uint64_t timestamp = (uint64_t)time(NULL);

                                if (pwrite(h->fd_vf, &timestamp, sizeof(timestamp), offset_for_vf) != sizeof(timestamp))
                                        return -errno;
                                if (pwrite(h->fd_vf, &file_size, sizeof(file_size), offset_for_vf + sizeof(uint64_t)) != sizeof(version))
                                        return -errno;

                                uint64_t ceil_block = ((file_size + BLOCK_SIZE - 1) / BLOCK_SIZE);
                                uint64_t value = MAKE_VERSION(version + 1);
                                for (uint64_t i = 0; i < ceil_block; i += 1)
                                {
                                        if (pwrite(fd_vf, &value, sizeof(value), (offset_for_vf + 2 * sizeof(uint64_t) + i * sizeof(uint64_t))) == -1)
                                                return -errno;
                                }
                        }
                }
        }

        close(fd_file);
        int res;
        if ((fi->flags & O_ACCMODE) == O_WRONLY)
        {
                fi->flags &= ~O_WRONLY;
                fi->flags |= O_RDWR;
        }
        res = openat(root_fd, rel_path, fi->flags);
        if (res == -1)
                return -errno;
        h->fd_file = res;
        return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
        char *actual_path;
        struct my_file_handle *myfh;
        if (fi)
        {
                myfh = (struct my_file_handle *)fi->fh;
                actual_path = myfh->path;
        }
        else
        {
                actual_path = strdup(path);
                if (!actual_path)
                        return -ENOMEM;
        }
        if (is_internal_file(actual_path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_read() called, path: %s\n", path);
        int res;
        if (is_virtfile(actual_path))
        {
                uint64_t bytes_read = 0;
                // const char *rel_ptr = (actual_path[0] == '/') ? actual_path + 1 : actual_path;
                // char base_path[PATH_MAX];
                // strncpy(base_path, rel_ptr, sizeof(base_path) - 1);
                // base_path[sizeof(base_path) - 1] = '\0';

                // char *vls_ptr = strstr(base_path, ".vls/");
                // *vls_ptr = '\0';
                // char version_file[MAX_PATH_LEN + 3];
                // char version_table[MAX_PATH_LEN + 3];
                // char disk_file[MAX_PATH_LEN + 2];

                // snprintf(version_file, sizeof(version_file), "%s..vf.", base_path);
                // snprintf(version_table, sizeof(version_table), "%s..vt.", base_path);
                // snprintf(disk_file, sizeof(disk_file), "%s..d.", base_path);

                // myfh->fd_vf = openat(root_fd, version_file, O_CREAT | O_RDWR, 0644);
                // if (myfh->fd_vf == -1)
                //         return -errno;

                // myfh->fd_vt = openat(root_fd, version_table, O_CREAT | O_RDWR, 0644);
                // if (myfh->fd_vt == -1)
                //         return -errno;

                // myfh->fd_disk = openat(root_fd, disk_file, O_CREAT | O_RDWR | O_APPEND, 0644);
                // if (myfh->fd_disk == -1)
                //         return -errno;
                char *at_sign = strrchr(actual_path, '@');
                if (at_sign)
                {
                        char *endptr;
                        uint64_t version_val = strtoull(at_sign + 1, &endptr, 10);

                        if (at_sign + 1 == endptr)
                        {
                                return -EINVAL;
                        }
                        if (read_version(size, offset, buf, version_val, myfh->fd_disk, myfh->fd_file, myfh->fd_vt, myfh->fd_vf, &bytes_read) < 0)
                                return -errno;
                }

                return bytes_read;
        }
        if (!is_internal_file(actual_path))
        {

                res = pread(myfh->fd_file, buf, size, offset);
                if (res == -1)
                        res = -errno;

                return res;
        }
        errno = EACCES;
        res = -errno;
        return res;
}

// static int xmp_read_buf(const char *path, struct fuse_bufvec **bufp,
//                         size_t size, off_t offset, struct fuse_file_info *fi)
// {
//         if (is_internal_file(path))
//                 return -ENOENT;
//         struct fuse_bufvec *src;

//         printf("[DEBUG] [myfs.c] xmp_read_buf() called\n");

//         src = malloc(sizeof(struct fuse_bufvec));
//         if (src == NULL)
//                 return -ENOMEM;

//         *src = FUSE_BUFVEC_INIT(size);

//         src->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
//         src->buf[0].fd = fi->fh;
//         src->buf[0].pos = offset;

//         *bufp = src;

//         return 0;
// }

static int xmp_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
        printf("[DEBUG] [myfs.c] xmp_write() called\n");

        struct my_file_handle *myfh = (struct my_file_handle *)fi->fh;

        if (is_internal_file(myfh->path))
                return -ENOENT;
        int res;
        if (store_blocks(size, offset, myfh->fd_disk, myfh->fd_file, myfh->fd_vt, myfh->fd_vf) != 0)
                return -errno;
        res = pwrite(myfh->fd_file, buf, size, offset);
        if (res == -1)
                res = -errno;
        return res;
}

// static int xmp_write_buf(const char *path, struct fuse_bufvec *buf,
//                          off_t offset, struct fuse_file_info *fi)
// {
//         if (is_internal_file(path))
//                 return -ENOENT;
//         struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));

//         printf("[DEBUG] [myfs.c] xmp_write_buf() called\n");

//         dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
//         dst.buf[0].fd = fi->fh;
//         dst.buf[0].pos = offset;

//         return fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
// }

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        printf("[DEBUG] [myfs.c] xmp_statfs() called\n");
        int sub_fd = openat(root_fd, rel_path, O_RDONLY | O_DIRECTORY);
        if (sub_fd == -1)
                return -errno;
        res = fstatvfs(sub_fd, stbuf);
        if (res == -1)
                return -errno;
        close(sub_fd);
        return 0;
}

static int xmp_flush(const char *path, struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;

        printf("[DEBUG] [myfs.c] xmp_flush() called\n");
        /* This is called from every close on an open file, so call the
           close on the underlying filesystem.  But since flush may be
           called multiple times for an open file, this must not really
           close the file.  This is important if used on a network
           filesystem like NFS which flush the data/metadata on close() */
        struct my_file_handle *myfh = (struct my_file_handle *)fi->fh;
        res = close(dup(myfh->fd_file));
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_release() called\n");
        struct my_file_handle *myfh = (struct my_file_handle *)fi->fh;
        //?
        if (myfh->is_virtual)

        {
                if (myfh->fd_file)
                        close(myfh->fd_file);
                if (myfh->fd_vf)
                        close(myfh->fd_vf);
                if (myfh->fd_vt)
                        close(myfh->fd_vt);
                if (myfh->fd_disk)
                        close(myfh->fd_disk);
        }
        else
        {
                if (myfh->fd_file)
                        close(myfh->fd_file);
                if (myfh->fd_vf)
                        close(myfh->fd_vf);
                if (myfh->fd_vt)
                        close(myfh->fd_vt);
                if (myfh->fd_disk)
                        close(myfh->fd_disk);
        }

        free(myfh);
        return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
                     struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        printf("[DEBUG] [myfs.c] xmp_fsync() called\n");

#ifndef HAVE_FDATASYNC
        (void)isdatasync;
#else
        if (isdatasync)
                res = fdatasync(fi->fh);
        else
#endif
        res = fsync(fi->fh);
        if (res == -1)
                return -errno;

        return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int xmp_fallocate(const char *path, int mode,
                         off_t offset, off_t length, struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_fallocate() called\n");

        if (mode)
                return -EOPNOTSUPP;

        return -posix_fallocate(fi->fh, offset, length);
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
                        size_t size, int flags)
{
        if (is_internal_file(path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_setxattr() called\n");
        int res = lsetxattr(path, name, value, size, flags);
        if (res == -1)
                return -errno;
        return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
                        size_t size)
{
        if (is_internal_file(path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_getxattr() called\n");
        int res = lgetxattr(path, name, value, size);
        if (res == -1)
                return -errno;
        return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
        if (is_internal_file(path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_listxattr() called\n");
        int res = llistxattr(path, list, size);
        if (res == -1)
                return -errno;
        return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
        if (is_internal_file(path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_removexattr() called\n");
        int res = lremovexattr(path, name);
        if (res == -1)
                return -errno;
        return 0;
}
#endif /* HAVE_SETXATTR */

#ifdef HAVE_LIBULOCKMGR
static int xmp_lock(const char *path, struct fuse_file_info *fi, int cmd,
                    struct flock *lock)
{
        if (is_internal_file(path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_lock() called\n");

        return ulockmgr_op(fi->fh, cmd, lock, &fi->lock_owner,
                           sizeof(fi->lock_owner));
}
#endif

static int xmp_flock(const char *path, struct fuse_file_info *fi, int op)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        printf("[DEBUG] [myfs.c] xmp_flock() called\n");

        res = flock(fi->fh, op);
        if (res == -1)
                return -errno;

        return 0;
}

#ifdef HAVE_COPY_FILE_RANGE
static ssize_t xmp_copy_file_range(const char *path_in,
                                   struct fuse_file_info *fi_in,
                                   off_t off_in, const char *path_out,
                                   struct fuse_file_info *fi_out,
                                   off_t off_out, size_t len, int flags)
{
        if (is_internal_file(path_in) || is_internal_file(path_out))
                return -ENOENT;
        ssize_t res;
        printf("[DEBUG] [myfs.c] xmp_copy_file_range() called\n");

        res = copy_file_range(fi_in->fh, &off_in, fi_out->fh, &off_out, len,
                              flags);
        if (res == -1)
                return -errno;

        return res;
}
#endif

static off_t xmp_lseek(const char *path, off_t off, int whence, struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        off_t res;
        printf("[DEBUG] [myfs.c] xmp_lseek() called\n");

        res = lseek(fi->fh, off, whence);
        if (res == -1)
                return -errno;

        return res;
}

static const struct fuse_operations xmp_oper = {
    .init = xmp_init,
    .getattr = xmp_getattr,
    .access = xmp_access,
    .readlink = xmp_readlink,
    .opendir = xmp_opendir,
    .readdir = xmp_readdir,
    .releasedir = xmp_releasedir,
    .mknod = xmp_mknod,
    .mkdir = xmp_mkdir,
    .symlink = xmp_symlink,
    .unlink = xmp_unlink,
    .rmdir = xmp_rmdir,
    .rename = xmp_rename,
    .link = xmp_link,
    .chmod = xmp_chmod,
    .chown = xmp_chown,
    .truncate = xmp_truncate,
#ifdef HAVE_UTIMENSAT
    .utimens = xmp_utimens,
#endif
    .create = xmp_create,
    .open = xmp_open,
    .read = xmp_read,
    //     .read_buf = xmp_read_buf,
    .write = xmp_write,
    //     .write_buf = xmp_write_buf,
    .statfs = xmp_statfs,
    .flush = xmp_flush,
    .release = xmp_release,
    .fsync = xmp_fsync,
#ifdef HAVE_POSIX_FALLOCATE
    .fallocate = xmp_fallocate,
#endif
#ifdef HAVE_SETXATTR
    .setxattr = xmp_setxattr,
    .getxattr = xmp_getxattr,
    .listxattr = xmp_listxattr,
    .removexattr = xmp_removexattr,
#endif
#ifdef HAVE_LIBULOCKMGR
    .lock = xmp_lock,
#endif
    .flock = xmp_flock,
#ifdef HAVE_COPY_FILE_RANGE
    .copy_file_range = xmp_copy_file_range,
#endif
    .lseek = xmp_lseek,
};

int main(int argc, char *argv[])
{
        char mount_dir[PATH_MAX];
        if (realpath(argv[argc - 1], mount_dir) == NULL)
        {
                perror("realpath");
                exit(1);
        }
        root_fd = open(mount_dir, O_RDONLY | O_DIRECTORY);
        if (root_fd == -1)
        {
                perror("Error: root_fd");
                return 1;
        };
        umask(0);
        return fuse_main(argc, argv, &xmp_oper, NULL);
}
