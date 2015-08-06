
#include <basetyps.h>
#include <search.h>

#include <time.h>

#ifndef HAVE_STRUCT_TIMESPEC

#ifndef __struct_timespec_defined

#define _FAKE_TIME_H_SOURCED	1
#define __need_struct_timespec	1
#include <parts/time.h>
#undef __need_struct_timespec

#define HAVE_STRUCT_TIMESPEC 1

#endif /* __struct_timespec_defined */
#endif /* HAVE_STRUCT_TIMESPEC */

#ifndef _CEPH_MINGW_TYPE_H
#define _CEPH_MINGW_TYPE_H
typedef __signed__ char __s8;
typedef unsigned char __u8;

typedef __signed__ short __s16;
typedef unsigned short __u16;

typedef __signed__ int __s32;
typedef unsigned int __u32;

typedef __signed__ long long __s64;
typedef unsigned long long __u64;

#define __bitwise__

typedef __u16 __bitwise__ __le16;
typedef __u16 __bitwise__ __be16;
typedef __u32 __bitwise__ __le32;
typedef __u32 __bitwise__ __be32;
typedef __u64 __bitwise__ __le64;
typedef __u64 __bitwise__ __be64;


#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(expression) \
  (__extension__							      \
    ({ long int __result;						      \
       do __result = (long int) (expression);				      \
       while (__result == -1L && errno == EINTR);			      \
       __result; }))
#endif

# define IOV_MAX 1024

struct iovec
{
  void *iov_base;
  size_t iov_len;
};

#ifndef __struct_timespec_defined
#ifndef HAVE_STRUCT_TIMESPEC
#define HAVE_STRUCT_TIMESPEC 1
#ifndef _TIMESPEC_DEFINED
#define _TIMESPEC_DEFINED
struct timespec {
        time_t tv_sec;
        long tv_nsec;
};
#endif /* _TIMESPEC_DEFINED */
#endif /* HAVE_STRUCT_TIMESPEC */
# define __struct_timespec_defined  1
#endif /* __struct_timespec_defined */

#define    EOPNOTSUPP   95    /* Operation not supported on transport endpoint */
#define    EBADE        52    /* Invalid exchange */
#define    ELOOP        40    /* Too many symbolic links encountered */
#define    ECANCELED   140    /* Operation canceled. */
#define    ENODATA      61    /* No data available */
#define    ESTALE      116    /* Stale NFS file handle */
#define    ENOTCONN    107    /* Transport endpoint is not connected */
#define    EISCONN     106    /* Transport endpoint is already connected */

#define strerror_r(errno, a, b) errno

#define S_IFLNK	 0120000

typedef long off_t;
typedef long long loff_t;

typedef unsigned long long __fsblkcnt64_t;
typedef unsigned long long __fsfilcnt64_t;

struct statvfs
  {
    unsigned long int f_bsize;
    unsigned long int f_frsize;

    __fsblkcnt64_t f_blocks;
    __fsblkcnt64_t f_bfree;
    __fsblkcnt64_t f_bavail;
    __fsfilcnt64_t f_files;
    __fsfilcnt64_t f_ffree;
    __fsfilcnt64_t f_favail;

    unsigned long int f_fsid;

    unsigned long int f_flag;
    unsigned long int f_namemax;
    int __f_spare[6];
  };

#define NAME_MAX 255
#define	MAXSYMLINKS	20

#define __dev_t unsigned long long
#define __ino_t unsigned long long
#define __nlink_t unsigned long long
#define __mode_t unsigned int
#define __uid_t unsigned int
#define __gid_t unsigned int
#define __off_t long long
#define __blksize_t long long
#define __blkcnt_t long long

#define O_SYNC	    04010000
#define O_DSYNC	010000	/* Synchronize data.  */
#define O_RSYNC	O_SYNC	/* Synchronize read operations.	 */

#endif
