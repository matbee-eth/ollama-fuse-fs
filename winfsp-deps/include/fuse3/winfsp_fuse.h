/**
 * @file fuse3/winfsp_fuse.h
 * WinFsp FUSE3 compatible API.
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

#ifndef WINFSP_FUSE_H
#define WINFSP_FUSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "fuse.h"

/* WinFsp-specific definitions */
#define FSP_FUSE_API
#define FSP_FUSE_API_NAME(n) n
#define FSP_FUSE_API_CALL(n) n
#define FSP_FUSE_SYM(n, x)

/* WinFsp-specific structures */
struct fsp_fuse_env;
struct fuse3_session;
struct fuse3_loop_config;

/* WinFsp-specific functions */
static inline struct fsp_fuse_env *fsp_fuse_env(void) {
    return 0;
}

// WinFsp-specific structures - avoid redefining what's already in fuse.h
#ifndef FUSE_ARGS_DEFINED
#define FUSE_ARGS_DEFINED
struct fuse_args {
    int argc;
    char **argv;
    int allocated;
};
#endif

#ifndef FUSE_CONFIG_DEFINED
#define FUSE_CONFIG_DEFINED
struct fuse_config {
    int set_gid;
    unsigned int gid;
    int set_uid;
    unsigned int uid;
    int set_mode;
    unsigned int umask;
    int set_entry_timeout;
    double entry_timeout;
    int set_attr_timeout;
    double attr_timeout;
    int set_negative_timeout;
    double negative_timeout;
    int debug;
    int hard_remove;
    int use_ino;
    int readdir_ino;
    int direct_io;
    int kernel_cache;
    int auto_cache;
    int intr;
    int intr_signal;
    int remember;
    int nullpath_ok;
    int show_help;
    char *modules;
    int clone_fd;
};
#endif

// WinFsp-specific function declarations
void fuse_opt_add_arg(struct fuse_args *args, const char *arg);
int fuse_opt_parse(struct fuse_args *args, void *data,
                  const struct fuse_opt *opts, void (*proc)(void *, const char *, int, struct fuse_args *));
void fuse_opt_free_args(struct fuse_args *args);

// WinFsp-specific macros
#define FUSE_ARGS_INIT(argc, argv) { argc, argv, 0 }

// WinFsp-specific operations
typedef int (*fuse_fill_dir_t) (void *buf, const char *name,
                               const struct stat *stbuf, off_t off,
                               enum fuse_fill_dir_flags flags);

// Forward declarations for structures we don't need to fully define
struct fuse_pollhandle;
struct fuse_bufvec;

// Main FUSE function for WinFsp
int fuse_main(int argc, char *argv[], const struct fuse_operations *op, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* WINFSP_FUSE_H */
