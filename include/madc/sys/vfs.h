// madc embedded sys/vfs.h — filesystem statistics
// statfs() is available via dlsym fallback.

#ifndef __MADC_SYS_VFS_H
#define __MADC_SYS_VFS_H 1

#include <sys/types.h>

struct statfs {
    long f_type;
    long f_bsize;
    long f_blocks;
    long f_bfree;
    long f_bavail;
    long f_files;
    long f_ffree;
    long f_fsid[2];
    long f_namelen;
    long f_frsize;
    long f_flags;
    long f_spare[4];
};

extern int statfs(const char *path, struct statfs *buf);

#endif
