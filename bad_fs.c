
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

char mount_dir[MAX_PATH_LEN];
int root_fd;

static void *xmp_init(struct fuse_conn_info *conn,
                      struct fuse_config *cfg)
{
        (void)conn;
        cfg->use_ino = 1;
        cfg->nullpath_ok = 1;

        char out[MAX_PATH_LEN];
        append_path("/", out);
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
        struct my_file_handle *myfh;
        if (fi)
        {
                myfh = (struct my_file_handle *)fi->fh;
                if (is_internal_file(myfh->path))
                        return -ENOENT;
        }
        size_t len = strlen(path);
        if (len > 4 && strcmp(path + len - 4, ".vls") == 0)
        {
                memset(stbuf, 0, sizeof(struct stat));
                stbuf->st_mode = S_IFDIR | 0555;
                stbuf->st_nlink = 2;
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
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";

        // printf("[DEBUG] [myfs.c] xmp_access() called for: %s\n", rel_path);

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

        // printf("[DEBUG] [myfs.c] xmp_readlink() called for: %s\n", rel_path);

        res = readlinkat(root_fd, rel_path, buf, size - 1);
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
        char path[MAX_PATH_LEN];
};

static int xmp_opendir(const char *path, struct fuse_file_info *fi)
{
        size_t len = strlen(path);
        if (len > 4 && strcmp(path + len - 4, ".vls") == 0)
        {
                struct xmp_dirp *d = malloc(sizeof(struct xmp_dirp));
                d->dp = NULL;
                d->offset = 0;
                fi->fh = (uint64_t)d;
                return 0;
        }
        int res;
        struct xmp_dirp *d = malloc(sizeof(struct xmp_dirp));
        if (d == NULL)
                return -ENOMEM;
        strncpy(d->path, path, MAX_PATH_LEN - 1);
        d->path[MAX_PATH_LEN - 1] = '\0';
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";

        // printf("[DEBUG] [myfs.c] xmp_opendir() called for: %s\n", rel_path);

        int fd = openat(root_fd, rel_path, O_RDONLY | O_DIRECTORY);
        if (fd == -1)
        {
                res = -errno;
                free(d);
                return res;
        }

        d->dp = fdopendir(fd);
        if (d->dp == NULL)
        {
                res = -errno;
                close(fd);
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
                if (S_ISREG(st.st_mode))
                {
                        char vls_name[NAME_MAX];
                        struct stat vst = st; 

                        snprintf(vls_name, sizeof(vls_name), "%s.vls", d->entry->d_name);

                        vst.st_mode = S_IFDIR | 0555; 
                        vst.st_size = 0;

                        filler(buf, vls_name, &vst, 0, 0);
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

        printf("[DEBUG] [myfs.c] xmp_mknod() called for: %s\n", rel_path);

        res = mknodat(root_fd, rel_path, mode, rdev);

        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
        if (is_internal_file(path))
                return -ENOENT;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";

        int res = mkdirat(root_fd, rel_path, mode);
        return (res == -1) ? -errno : 0;
}

static int xmp_unlink(const char *path)
{
        if (is_internal_file(path))
                return -ENOENT;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;

        int res = unlinkat(root_fd, rel_path, 0);
        return (res == -1) ? -errno : 0;
}

static int xmp_rmdir(const char *path)
{
        if (is_internal_file(path))
                return -ENOENT;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;

        int res = unlinkat(root_fd, rel_path, AT_REMOVEDIR);
        return (res == -1) ? -errno : 0;
}

static int xmp_symlink(const char *from, const char *to)
{
        const char *rel_to = (to[0] == '/') ? to + 1 : to;
        int res = symlinkat(from, root_fd, rel_to);
        return (res == -1) ? -errno : 0;
}

static int xmp_rename(const char *from, const char *to, unsigned int flags)
{
        if (flags)
                return -EINVAL;
        const char *rel_from = (from[0] == '/') ? from + 1 : from;
        const char *rel_to = (to[0] == '/') ? to + 1 : to;

        char vf_from[PATH_MAX], vt_from[PATH_MAX], d_from[PATH_MAX], vf_to[PATH_MAX], vt_to[PATH_MAX], d_to[PATH_MAX];
        snprintf(vf_from, PATH_MAX, "%s.vf", rel_from);
        snprintf(vt_from, PATH_MAX, "%s.vt", rel_from);
        snprintf(d_from, PATH_MAX, "%s.d", rel_from);
        snprintf(vf_to, PATH_MAX, "%s.vf", rel_to);
        snprintf(vt_to, PATH_MAX, "%s.vt", rel_to);
        snprintf(d_to, PATH_MAX, "%s.d", rel_to);

        // Rename versioning files
        renameat(root_fd, vf_from, root_fd, vf_to);
        renameat(root_fd, vt_from, root_fd, vt_to);
        renameat(root_fd, d_from, root_fd, d_to);

        int res = renameat(root_fd, rel_from, root_fd, rel_to);
        return (res == -1) ? -errno : 0;
}

static int xmp_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
        const char *rel_path = (path[0] == '/') ? path + 1 : path;

        int fd = openat(root_fd, rel_path, (fi->flags & ~O_ACCMODE) | O_RDWR | O_CREAT, mode);
        if (fd == -1)
                return -errno;

        char vf_n[PATH_MAX], vt_n[PATH_MAX], d_n[PATH_MAX];
        snprintf(vf_n, PATH_MAX, "%s.vf", rel_path);
        snprintf(vt_n, PATH_MAX, "%s.vt", rel_path);
        snprintf(d_n, PATH_MAX, "%s.d", rel_path);

        int fd_vf = openat(root_fd, vf_n, O_CREAT | O_RDWR, 0644);
        int fd_vt = openat(root_fd, vt_n, O_CREAT | O_RDWR, 0644);
        int fd_disk = openat(root_fd, d_n, O_CREAT | O_RDWR | O_APPEND, 0644);

        struct my_file_handle *h = malloc(sizeof(struct my_file_handle));
        h->fd_file = fd;
        h->fd_vf = fd_vf;
        h->fd_vt = fd_vt;
        h->fd_disk = fd_disk;
        strncpy(h->path, rel_path, MAX_PATH_LEN - 1);
        h->path[MAX_PATH_LEN - 1] = '\0';
        fi->fh = (uint64_t)h;
        return 0;
}

static int xmp_link(const char *from, const char *to)
{
        const char *rel_from = (from[0] == '/') ? from + 1 : from;
        const char *rel_to = (to[0] == '/') ? to + 1 : to;

        int res = linkat(root_fd, rel_from, root_fd, rel_to, 0);
        return (res == -1) ? -errno : 0;
}

static int xmp_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
        int res;
        if (fi)
                res = fchmod(((struct my_file_handle *)fi->fh)->fd_file, mode);
        else
        {
                const char *rel_path = (path[0] == '/') ? path + 1 : path;
                res = fchmodat(root_fd, rel_path, mode, 0);
        }
        return (res == -1) ? -errno : 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid,
                     struct fuse_file_info *fi)
{
        struct my_file_handle *myfh;
        if (fi)
        {
                myfh = (struct my_file_handle *)fi->fh;
                if (is_internal_file(myfh->path))
                        return -ENOENT;
        }
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

static int xmp_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
        struct my_file_handle *myfh = (struct my_file_handle *)fi->fh;
        int res = ftruncate(myfh->fd_file, size);
        return (res == -1) ? -errno : 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi)
{
        int res;
        if (fi)
                res = futimens(((struct my_file_handle *)fi->fh)->fd_file, ts);
        else
        {
                const char *rel_path = (path[0] == '/') ? path + 1 : path;
                res = utimensat(root_fd, rel_path, ts, AT_SYMLINK_NOFOLLOW);
        }
        return (res == -1) ? -errno : 0;
}
#endif

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
        
        if(is_internal_file(path))
                return -ENOENT;
        // Convert absolute FUSE path to relative path for openat
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                rel_path = ".";
        

        printf("[DEBUG] [myfs.c] xmp_open() called for: %s\n", rel_path);

        char version_file[PATH_MAX];
        char version_table[PATH_MAX];
        char disk_file[PATH_MAX];

        snprintf(version_file, sizeof(version_file), "%s.vf", rel_path);
        snprintf(version_table, sizeof(version_table), "%s.vt", rel_path);
        snprintf(disk_file, sizeof(disk_file), "%s.d", rel_path);

        // Open internal versioning files via root_fd
        int fd_vf = openat(root_fd, version_file, O_CREAT | O_RDWR, 0644);
        if (fd_vf == -1)
                return -errno;

        int fd_vt = openat(root_fd, version_table, O_CREAT | O_RDWR, 0644);
        if (fd_vt == -1)
        {
                close(fd_vf);
                return -errno;
        }

        int fd_disk = openat(root_fd, disk_file, O_RDWR | O_APPEND | O_CREAT, 0644);
        if (fd_disk == -1)
        {
                // If it doesn't exist, we might need to create it depending on your logic
                // For now, staying consistent with your O_RDWR | O_APPEND
                close(fd_vf);
                close(fd_vt);
                return -errno;
        }

        if (fi->flags & O_DIRECT)
        {
                fi->direct_io = 1;
                fi->parallel_direct_writes = 1;
        }

        struct my_file_handle *h = malloc(sizeof(struct my_file_handle));
        if (!h)
        {
                close(fd_vf);
                close(fd_vt);
                close(fd_disk);
                return -ENOMEM;
        }

        h->fd_vf = fd_vf;
        h->fd_vt = fd_vt;
        h->fd_disk = fd_disk;

        // Store path relative to root_fd for consistency
        strncpy(h->path, rel_path, MAX_PATH_LEN - 1);
        h->path[MAX_PATH_LEN - 1] = '\0';
        fi->fh = (uint64_t)h;

        int trunced = 0;
        // Use openat for the main file
        int fd_file = openat(root_fd, rel_path, O_RDWR, 0644);
        if (fd_file == -1)
        {
                // Clean up is handled by xmp_release if we return here,
                // but since we haven't finished, manual cleanup:
                close(fd_vf);
                close(fd_vt);
                close(fd_disk);
                free(h);
                return -errno;
        }

        struct stat sb;
        if (fstat(fd_file, &sb) == -1)
        {
                close(fd_file);
                return -errno;
        }

        uint64_t file_size = sb.st_size;

        // LOGIC REMAINS UNTOUCHED
        if (fi->flags & O_TRUNC)
        {
                trunced = 1;
                if (file_size > 0)
                {
                        uint64_t version = update_version_counter(h->fd_vt);
                        off_t vt_off = version * sizeof(uint64_t);
                        uint64_t offset_for_vf = lseek(h->fd_vf, 0, SEEK_END);

                        if (pwrite(h->fd_vt, &offset_for_vf, sizeof(offset_for_vf), vt_off) != sizeof(offset_for_vf))
                                return -errno;

                        if (pwrite(h->fd_vf, &version, sizeof(version), offset_for_vf) != sizeof(version))
                                return -errno;
                        if (pwrite(h->fd_vf, &file_size, sizeof(file_size), offset_for_vf + sizeof(uint64_t)) != sizeof(file_size))
                                return -errno;
                        if (store_blocks(file_size, 0, h->fd_disk, fd_file, h->fd_vt, h->fd_vf) == -1)
                                return -errno;

                        close(fd_file);
                }
        }

        if (((fi->flags & O_RDWR) || (fi->flags & O_WRONLY)) && !trunced)
        {
                if (file_size > 0)
                {
                        uint64_t version = update_version_counter(h->fd_vt);
                        off_t vt_off = version * sizeof(uint64_t);
                        uint64_t offset_for_vf = lseek(h->fd_vf, 0, SEEK_END);

                        if (pwrite(h->fd_vt, &offset_for_vf, sizeof(offset_for_vf), vt_off) != sizeof(offset_for_vf))
                                return -errno;

                        if (pwrite(h->fd_vf, &version, sizeof(version), offset_for_vf) != sizeof(version))
                                return -errno;
                        if (pwrite(h->fd_vf, &file_size, sizeof(file_size), offset_for_vf + sizeof(uint64_t)) != sizeof(file_size))
                                return -errno;

                        uint64_t ceil_file_size = ((file_size / BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;
                        uint64_t value = MAKE_VERSION(version + 1);
                        for (uint64_t i = offset_for_vf + 2 * sizeof(uint64_t); i < ceil_file_size; i += sizeof(uint64_t))
                        {
                                if (pwrite(fd_vf, &value, sizeof(value), i) == -1)
                                        return -errno;
                        }
                }
        }

        if (trunced || ((fi->flags & O_RDWR) || (fi->flags & O_WRONLY)))
        {
                // If we closed it earlier in the logic, re-open it
                if (trunced && file_size > 0)
                {
                        fd_file = openat(root_fd, rel_path, fi->flags);
                }
                else
                {
                        // Or just keep the one we had if it's still valid
                        // But for safety to match your logic:
                        close(fd_file);
                        fd_file = openat(root_fd, rel_path, fi->flags);
                }
        }
        else
        {
                close(fd_file);
                fd_file = openat(root_fd, rel_path, fi->flags);
        }

        if (fd_file == -1)
                return -errno;

        h->fd_file = fd_file;
        return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
        struct my_file_handle *myfh;
        if (fi)
        {
                myfh = (struct my_file_handle *)fi->fh;
                if (is_internal_file(myfh->path))
                        return -ENOENT;
        }
        printf("[DEBUG] [myfs.c] xmp_read() called, path: %s\n", path);
        int res;

        if (!is_internal_file(myfh->path))
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
//         if (is_internal_file(myfh->path))
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
                return -errno;

        return res;
}

// static int xmp_write_buf(const char *path, struct fuse_bufvec *buf,
//                          off_t offset, struct fuse_file_info *fi)
// {
//         if (is_internal_file(myfh->path))
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
        int res;
        const char *rel_path = (path[0] == '/') ? path + 1 : path;
        if (rel_path[0] == '\0')
                return (statvfs(".", stbuf) == -1) ? -errno : 0;

        int fd = openat(root_fd, rel_path, O_RDONLY);
        if (fd == -1)
                return -errno;
        res = fstatvfs(fd, stbuf);
        close(fd);
        return (res == -1) ? -errno : 0;
}

static int xmp_flush(const char *path, struct fuse_file_info *fi)
{
        struct my_file_handle *myfh;
        if (fi)
        {
                myfh = (struct my_file_handle *)fi->fh;
                if (is_internal_file(myfh->path))
                        return -ENOENT;
        }
        int res;

        printf("[DEBUG] [myfs.c] xmp_flush() called\n");
        /* This is called from every close on an open file, so call the
           close on the underlying filesystem.  But since flush may be
           called multiple times for an open file, this must not really
           close the file.  This is important if used on a network
           filesystem like NFS which flush the data/metadata on close() */
        res = close(dup(myfh->fd_file));
        if (res == -1)
                return -errno;

        return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
        struct my_file_handle *myfh;
        if (fi)
        {
                myfh = (struct my_file_handle *)fi->fh;
                if (is_internal_file(myfh->path))
                        return -ENOENT;
        }
        printf("[DEBUG] [myfs.c] xmp_release() called\n");
        if (myfh->fd_file)
                close(myfh->fd_file);
        if (myfh->fd_vf)
                close(myfh->fd_vf);
        if (myfh->fd_vt)
                close(myfh->fd_vt);
        if (myfh->fd_disk)
                close(myfh->fd_disk);
        free(myfh);
        return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
                     struct fuse_file_info *fi)
{
        struct my_file_handle *myfh;
        if (fi)
        {
                myfh = (struct my_file_handle *)fi->fh;
                if (is_internal_file(myfh->path))
                        return -ENOENT;
        }
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
        if (is_internal_file(myfh->path))
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
        if (is_internal_file(myfh->path))
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
        if (is_internal_file(myfh->path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_getxattr() called\n");
        int res = lgetxattr(path, name, value, size);
        if (res == -1)
                return -errno;
        return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
        if (is_internal_file(myfh->path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_listxattr() called\n");
        int res = llistxattr(path, list, size);
        if (res == -1)
                return -errno;
        return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
        if (is_internal_file(myfh->path))
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
        if (is_internal_file(myfh->path))
                return -ENOENT;
        printf("[DEBUG] [myfs.c] xmp_lock() called\n");

        return ulockmgr_op(fi->fh, cmd, lock, &fi->lock_owner,
                           sizeof(fi->lock_owner));
}
#endif

static int xmp_flock(const char *path, struct fuse_file_info *fi, int op)
{
        struct my_file_handle *myfh;
        if (fi)
        {
                myfh = (struct my_file_handle *)fi->fh;
                if (is_internal_file(myfh->path))
                        return -ENOENT;
        }
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
        struct my_file_handle *myfh;
        if (fi)
        {
                myfh = (struct my_file_handle *)fi->fh;
                if (is_internal_file(myfh->path))
                        return -ENOENT;
        }
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