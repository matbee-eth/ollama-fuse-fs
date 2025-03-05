/**
 * @file fuse3/fuse.h
 * WinFsp FUSE3 compatible API.
 *
 * This file is derived from libfuse/include/fuse.h:
 *     FUSE: Filesystem in Userspace
 *     Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
 *
 * @copyright 2015-2022 Bill Zissimopoulos
 */
/*
 * This file is part of WinFsp.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 3 as published by the Free Software
 * Foundation.
 *
 * Licensees holding a valid commercial license may use this software
 * in accordance with the commercial license agreement provided in
 * conjunction with the software.  The terms and conditions of any such
 * commercial license agreement shall govern, supersede, and render
 * ineffective any application of the GPLv3 license to this software,
 * notwithstanding of any reference thereto in the software or
 * associated repository.
 */

#ifndef FUSE_H
#define FUSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>

// Define uid_t and gid_t for Windows
#ifdef _WIN32
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif

// FUSE API version
#define FUSE_USE_VERSION 31

// FUSE flags
#define FUSE_CAP_ASYNC_READ          (1 << 0)
#define FUSE_CAP_POSIX_LOCKS         (1 << 1)
#define FUSE_CAP_ATOMIC_O_TRUNC      (1 << 3)
#define FUSE_CAP_EXPORT_SUPPORT      (1 << 4)
#define FUSE_CAP_DONT_MASK           (1 << 6)
#define FUSE_CAP_SPLICE_WRITE        (1 << 7)
#define FUSE_CAP_SPLICE_MOVE         (1 << 8)
#define FUSE_CAP_SPLICE_READ         (1 << 9)
#define FUSE_CAP_FLOCK_LOCKS         (1 << 10)
#define FUSE_CAP_IOCTL_DIR           (1 << 11)
#define FUSE_CAP_AUTO_INVAL_DATA     (1 << 12)
#define FUSE_CAP_READDIRPLUS         (1 << 13)
#define FUSE_CAP_READDIRPLUS_AUTO    (1 << 14)
#define FUSE_CAP_ASYNC_DIO           (1 << 15)
#define FUSE_CAP_WRITEBACK_CACHE     (1 << 16)
#define FUSE_CAP_NO_OPEN_SUPPORT     (1 << 17)
#define FUSE_CAP_PARALLEL_DIROPS     (1 << 18)
#define FUSE_CAP_POSIX_ACL           (1 << 19)
#define FUSE_CAP_HANDLE_KILLPRIV     (1 << 20)

// Forward declarations
struct stat;
struct fuse_file_info;

// Define fuse_readdir_flags enum
enum fuse_readdir_flags {
    FUSE_READDIR_PLUS = (1 << 0)
};

// Define fuse_fill_dir_flags enum
enum fuse_fill_dir_flags {
    FUSE_FILL_DIR_PLUS = (1 << 1)
};

// Define fuse_fill_dir_t type
typedef int (*fuse_fill_dir_t) (void *buf, const char *name,
                               const struct stat *stbuf, off_t off,
                               enum fuse_fill_dir_flags flags);

// FUSE structures
struct fuse_file_info {
    int flags;
    unsigned int writepage : 1;
    unsigned int direct_io : 1;
    unsigned int keep_cache : 1;
    unsigned int flush : 1;
    unsigned int nonseekable : 1;
    unsigned int flock_release : 1;
    unsigned int padding : 26;
    uint64_t fh;
    uint64_t lock_owner;
    uint32_t poll_events;
};

struct fuse_conn_info {
    unsigned proto_major;
    unsigned proto_minor;
    unsigned async_read;
    unsigned max_write;
    unsigned max_readahead;
    unsigned capable;
    unsigned want;
    unsigned max_background;
    unsigned congestion_threshold;
    unsigned time_gran;
    unsigned reserved[22];
};

struct fuse_operations {
    int (*getattr) (const char *, struct stat *, struct fuse_file_info *);
    int (*readlink) (const char *, char *, size_t);
    int (*mknod) (const char *, mode_t, dev_t);
    int (*mkdir) (const char *, mode_t);
    int (*unlink) (const char *);
    int (*rmdir) (const char *);
    int (*symlink) (const char *, const char *);
    int (*rename) (const char *, const char *, unsigned int);
    int (*link) (const char *, const char *);
    int (*chmod) (const char *, mode_t, struct fuse_file_info *);
    int (*chown) (const char *, uid_t, gid_t, struct fuse_file_info *);
    int (*truncate) (const char *, off_t, struct fuse_file_info *);
    int (*open) (const char *, struct fuse_file_info *);
    int (*read) (const char *, char *, size_t, off_t, struct fuse_file_info *);
    int (*write) (const char *, const char *, size_t, off_t, struct fuse_file_info *);
    int (*statfs) (const char *, struct statvfs *);
    int (*flush) (const char *, struct fuse_file_info *);
    int (*release) (const char *, struct fuse_file_info *);
    int (*fsync) (const char *, int, struct fuse_file_info *);
    int (*setxattr) (const char *, const char *, const char *, size_t, int);
    int (*getxattr) (const char *, const char *, char *, size_t);
    int (*listxattr) (const char *, char *, size_t);
    int (*removexattr) (const char *, const char *);
    int (*opendir) (const char *, struct fuse_file_info *);
    int (*readdir) (const char *, void *, fuse_fill_dir_t, off_t, struct fuse_file_info *, enum fuse_readdir_flags);
    int (*releasedir) (const char *, struct fuse_file_info *);
    int (*fsyncdir) (const char *, int, struct fuse_file_info *);
    void *(*init) (struct fuse_conn_info *, struct fuse_config *);
    void (*destroy) (void *);
    int (*access) (const char *, int);
    int (*create) (const char *, mode_t, struct fuse_file_info *);
    int (*lock) (const char *, struct fuse_file_info *, int cmd, struct flock *);
    int (*utimens) (const char *, const struct timespec tv[2], struct fuse_file_info *);
    int (*bmap) (const char *, size_t blocksize, uint64_t *idx);
    int (*ioctl) (const char *, unsigned int cmd, void *arg, struct fuse_file_info *, unsigned int flags, void *data);
    int (*poll) (const char *, struct fuse_file_info *, struct fuse_pollhandle *ph, unsigned *reventsp);
    int (*write_buf) (const char *, struct fuse_bufvec *buf, off_t off, struct fuse_file_info *);
    int (*read_buf) (const char *, struct fuse_bufvec **bufp, size_t size, off_t off, struct fuse_file_info *);
    int (*flock) (const char *, struct fuse_file_info *, int op);
    int (*fallocate) (const char *, int, off_t, off_t, struct fuse_file_info *);
};

// Forward declarations for structures we don't need to fully define
struct fuse_config;
struct fuse_pollhandle;
struct fuse_bufvec;
struct statvfs;

// FUSE functions
struct fuse *fuse_new(struct fuse_args *args, const struct fuse_operations *op,
                     size_t op_size, void *user_data);
int fuse_mount(struct fuse *f, const char *mountpoint);
int fuse_loop(struct fuse *f);
int fuse_loop_mt(struct fuse *f, int clone_fd);
void fuse_unmount(struct fuse *f);
void fuse_destroy(struct fuse *f);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_H */
