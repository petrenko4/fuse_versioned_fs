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

static void *xmp_init(struct fuse_conn_info *conn,
                      struct fuse_config *cfg)
{
        (void)conn;
        cfg->use_ino = 1;
        cfg->nullpath_ok = 1;

        char out[MAX_PATH_LEN];
        append_path("/", out);
        fopen(DISK_FILE, "w+");
        // printf("[DEBUG] [myfs.c] xmp_init() called\n");
        printf("Versioned filesystem is mounted at %s\n", out);

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

static int xmp_getattr(const char *path, struct stat *stbuf,
                       struct fuse_file_info *fi)
{
        int res;

        if (is_internal_file(path))
                return -ENOENT;
        if (fi)
        {
                struct my_file_handle *myfh = (struct my_file_handle *)fi->fh;
                res = fstat(myfh->fd_file, stbuf);
        }
        else
        {
                char new_path[MAX_PATH_LEN];
                append_path(path, new_path);
                printf("[DEBUG] [myfs.c] xmp_getattr() called\n");
                res = lstat(new_path, stbuf);
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
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_access() called\n");

        res = access(new_path, mask);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_readlink() called\n");

        res = readlink(new_path, buf, size - 1);
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
};

static int xmp_opendir(const char *path, struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        struct xmp_dirp *d = malloc(sizeof(struct xmp_dirp));
        if (d == NULL)
                return -ENOMEM;

        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_opendir() called\n");

        d->dp = opendir(new_path);
        if (d->dp == NULL)
        {
                res = -errno;
                free(d);
                return res;
        }
        d->offset = 0;
        d->entry = NULL;

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

        printf("[DEBUG] [myfs.c] xmp_readdir() called\n");
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
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_mknod() called\n");

        if (S_ISFIFO(mode))
                res = mkfifo(new_path, mode);
        else
                res = mknod(new_path, mode, rdev);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_mkdir() called\n");
        res = mkdir(new_path, mode);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_unlink(const char *path)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_unlink() called\n");
        res = unlink(new_path);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_rmdir(const char *path)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_rmdir() called\n");
        res = rmdir(new_path);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
        if (is_internal_file(from) || is_internal_file(to))
                return -ENOENT;
        int res;
        char new_from[MAX_PATH_LEN];
        char new_to[MAX_PATH_LEN];
        append_path(from, new_from);
        append_path(to, new_to);
        printf("[DEBUG] [myfs.c] xmp_symlink() called\n");
        res = symlink(new_from, new_to);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_rename(const char *from, const char *to, unsigned int flags)
{
        if (is_internal_file(from) || is_internal_file(to))
                return -ENOENT;
        int res;
        char new_from[MAX_PATH_LEN];
        char new_to[MAX_PATH_LEN];
        append_path(from, new_from);
        append_path(to, new_to);
        printf("[DEBUG] [myfs.c] xmp_rename() called\n");
        /* When we have renameat2() in libc, then we can implement flags */
        if (flags)
                return -EINVAL;

        res = rename(new_from, new_to);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_link(const char *from, const char *to)
{
        if (is_internal_file(from) || is_internal_file(to))
                return -ENOENT;
        int res;
        char new_from[MAX_PATH_LEN];
        char new_to[MAX_PATH_LEN];
        append_path(from, new_from);
        append_path(to, new_to);
        printf("[DEBUG] [myfs.c] xmp_link() called\n");
        res = link(new_from, new_to);

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
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_chmod() called\n");

        if (fi)
                res = fchmod(fi->fh, mode);
        else
                res = chmod(new_path, mode);
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
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_chown() called\n");
        if (fi)
                res = fchown(fi->fh, uid, gid);
        else
                res = lchown(new_path, uid, gid);
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        int res;
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_truncate() called\n");

        if (fi)
                res = ftruncate(fi->fh, size);
        else
                res = truncate(new_path, size);

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
        int fd;
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_create() called\n");
        fd = open(new_path, (fi->flags & ~O_ACCMODE) | O_RDWR, mode);
        if (fd == -1)
                return -errno;

        char version_file[PATH_MAX];
        char version_table[PATH_MAX];

        snprintf(version_file, sizeof(version_file), "%s.vf", new_path);
        snprintf(version_table, sizeof(version_table), "%s.vt", new_path);

        int fd_vf = open(version_file, O_CREAT | O_RDWR, 0644);
        if (fd_vf == -1)
                return -errno;

        int fd_vt = open(version_table, O_CREAT | O_RDWR, 0644);
        if (fd_vt == -1)
        {
                close(fd_vt);
                return -errno;
        }
        vt_init(52, fd_vt);
        struct my_file_handle *h = malloc(sizeof(struct my_file_handle));
        if (!h)
                return -ENOMEM;
        h->fd_file = fd;
        h->fd_vf = fd_vf;
        h->fd_vt = fd_vt; 
        strncpy(h->path, new_path, MAX_PATH_LEN - 1);
        h->path[MAX_PATH_LEN - 1] = '\0';
        fi->fh = (uint64_t)h;
        return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
        if (is_internal_file(path))
                return -ENOENT;
        int fd;
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_open() called\n");
        if (fi->flags & O_TRUNC)
        {
                int fd_disk = open(DISK_FILE, O_RDWR | O_APPEND, 0644);
                save_entire_file(new_path, fd_disk);
        }
        fd = open(new_path, (fi->flags & ~O_ACCMODE) | O_RDWR, 0644);
        if (fd == -1)
                return -errno;

        char version_file[PATH_MAX];
        char version_table[PATH_MAX];

        snprintf(version_file, sizeof(version_file), "%s.vf", new_path);
        snprintf(version_table, sizeof(version_table), "%s.vt", new_path);

        int fd_vf = open(version_file, O_CREAT | O_RDWR, 0644);
        if (fd_vf == -1)
                return -errno;

        int fd_vt = open(version_table, O_CREAT | O_RDWR, 0644);
        if (fd_vt == -1)
        {
                close(fd_vt);
                return -errno;
        }
        /* Enable direct_io when open has flags O_DIRECT to enjoy the feature
           parallel_direct_writes (i.e., to get a shared lock, not exclusive lock,
           for writes to the same file). */
        if (fi->flags & O_DIRECT)
        {
                fi->direct_io = 1;
                fi->parallel_direct_writes = 1;
        }
        struct my_file_handle *h = malloc(sizeof(struct my_file_handle));
        if (!h)
                return -ENOMEM;
        h->fd_file = fd;
        h->fd_vf = fd_vf;
        h->fd_vt = fd_vt;
        strncpy(h->path, new_path, MAX_PATH_LEN - 1);
        h->path[MAX_PATH_LEN - 1] = '\0';
        fi->fh = (uint64_t)h;
        return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
        struct my_file_handle *myfh = (struct my_file_handle *)fi->fh;
        if (is_internal_file(path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_read() called, path: %s\n", path);
        int res;

        if (!is_internal_file(path))
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
        
        char *disk_path = DISK_FILE;
        int fd_disk = open(disk_path, O_RDWR | O_APPEND);
        uint64_t a = 0;
        uint64_t *p_blocks_amount = &a;

        uint64_t *changed_blocks = count_affected_blocks(size, offset, p_blocks_amount);
        if (store_blocks(changed_blocks, *p_blocks_amount, fd_disk, myfh->fd_file) != 0)
                return -errno;
        update_version_counter(myfh->fd_vt);
        res = pwrite(myfh->fd_file, buf, size, offset);
        if (res == -1)
                res = -errno;
        //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
        free(changed_blocks);
        close(fd_disk);
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
        char new_path[MAX_PATH_LEN];
        append_path(path, new_path);
        printf("[DEBUG] [myfs.c] xmp_statfs() called\n");
        res = statvfs(new_path, stbuf);
        if (res == -1)
                return -errno;

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

        if (myfh->fd_file)
                close(myfh->fd_file);
        if(myfh->fd_vf)
                close(myfh->fd_vf);
        if(myfh->fd_vt)
                close(myfh->fd_vt);
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
        umask(0);
        return fuse_main(argc, argv, &xmp_oper, NULL);
}