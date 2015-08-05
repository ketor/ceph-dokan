
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
/*
typedef int8_t __s8;
typedef uint8_t __u8;
typedef int16_t __s16;
typedef uint16_t __u16;
typedef int32_t __s32;
typedef uint32_t __u32;
typedef int64_t __s64;
typedef uint64_t __u64;
*/
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

struct stat_ceph {
    __dev_t st_dev;
    __ino_t st_ino;
    __nlink_t st_nlink;
    __mode_t st_mode;
    __uid_t st_uid;
    __gid_t st_gid;
    int __pad0;
    __dev_t st_rdev;
    unsigned short int __pad2;
    __off_t st_size;
    __blksize_t st_blksize;
    __blkcnt_t st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    long long __unused[3];
};

//struct stat_ceph
//  {
//    unsigned int st_dev;		/* Device.  */
//    unsigned long long st_ino;		/* File serial number.	*/
//
//    unsigned int st_nlink;		/* Link count.  */
//    unsigned int st_mode;		/* File mode.  */
//
//    unsigned int st_uid;		/* User ID of the file's owner.	*/
//    unsigned int st_gid;		/* Group ID of the file's group.*/
//    unsigned int st_rdev;		/* Device number, if device.  */
//    unsigned short int __pad2;
//
//    unsigned long long st_size;			/* Size of file, in bytes.  */
//
//    unsigned long long st_blksize;	/* Optimal block size for I/O.  */
//    unsigned long long st_blocks;		/* Number 512-byte blocks allocated. */
//
//    /* Nanosecond resolution timestamps are stored in a format
//       equivalent to 'struct timespec'.  This is the type used
//       whenever possible but the Unix namespace rules do not allow the
//       identifier 'timespec' to appear in the <sys/stat.h> header.
//       Therefore we have to handle the use of this header in strictly
//       standard-compliant sources special.  */
//    struct timespec st_atim;		/* Time of last access.  */
//    struct timespec st_mtim;		/* Time of last modification.  */
//    struct timespec st_ctim;		/* Time of last status change.  */
//    time_t st_atime ;	/* Backward compatibility.  */
//    time_t st_mtime ;
//    time_t st_ctime ;
//    unsigned long long st_ino1;			/* File serial number.	*/
//  };

#define O_SYNC	    04010000
#define O_DSYNC	010000	/* Synchronize data.  */
#define O_RSYNC	O_SYNC	/* Synchronize read operations.	 */

#endif
