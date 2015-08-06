#ifndef CEPH_POSIX_ACL_H
#define CEPH_POSIX_ACL_H

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "libcephfs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XATTR_MAX_SIZE 8192

typedef mode_t umode_t;

#define MAY_EXEC        0x00000001
#define MAY_WRITE        0x00000002
#define MAY_READ        0x00000004
#define MAY_APPEND        0x00000008
#define MAY_ACCESS        0x00000010
#define MAY_OPEN        0x00000020
#define MAY_CHDIR        0x00000040
/* called from RCU mode, don't block */
#define MAY_NOT_BLOCK        0x00000080

#define PERM_WALK_CHECK_READ   MAY_READ
#define PERM_WALK_CHECK_WRITE  MAY_WRITE
#define PERM_WALK_CHECK_EXEC   MAY_EXEC

#define POSIX_ACL_XATTR_ACCESS     "system.posix_acl_access"
#define POSIX_ACL_XATTR_DEFAULT    "system.posix_acl_default"

#define ACL_UNDEFINED_ID    (-1)

/* a_type field in acl_user_posix_entry_t */
#define ACL_TYPE_ACCESS        (0x8000)
#define ACL_TYPE_DEFAULT    (0x4000)

/* e_tag entry in struct posix_acl_entry */
#define ACL_USER_OBJ        (0x01)
#define ACL_USER        (0x02)
#define ACL_GROUP_OBJ        (0x04)
#define ACL_GROUP        (0x08)
#define ACL_MASK        (0x10)
#define ACL_OTHER        (0x20)

/* permissions in the e_perm field */
#define ACL_READ        (0x04)
#define ACL_WRITE        (0x02)
#define ACL_EXECUTE        (0x01)
//#define ACL_ADD        (0x08)
//#define ACL_DELETE        (0x10)

#ifdef S_IRWXU
#undef S_IRWXU
#define S_IRWXU 00700
#endif

#ifdef S_IRUSR
#undef S_IRUSR
#define S_IRUSR 00400
#endif

#ifdef S_IWUSR
#undef S_IWUSR
#define S_IWUSR 00200
#endif

#ifdef S_IXUSR
#undef S_IXUSR
#define S_IXUSR 00100
#endif

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

#define S_IRWXUGO    (S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO    (S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO        (S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO        (S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO        (S_IXUSR|S_IXGRP|S_IXOTH)

#define FOREACH_ACL_ENTRY(pa, acl, pe) \
    for(pa=(acl)->a_entries, pe=pa+(acl)->a_count; pa<pe; pa++)

typedef unsigned long long uid_t;
typedef unsigned long long gid_t;


/****************************************************/
#define __force
typedef unsigned gfp_t;
#define ___GFP_WAIT        0x10u
#define ___GFP_IO        0x40u
#define __GFP_WAIT    ((__force gfp_t)___GFP_WAIT)    /* Can wait and reschedule? */
#define __GFP_IO    ((__force gfp_t)___GFP_IO)    /* Can start physical IO? */
#define GFP_NOIO    (__GFP_WAIT)
#define GFP_NOFS    (__GFP_WAIT | __GFP_IO)

typedef uid_t kuid_t;
typedef gid_t kgid_t;


struct callback_head {
    struct callback_head *next;
    void (*func)(struct callback_head *head);
};
#define rcu_head callback_head

typedef struct {
    int counter;
} atomic_t;

struct posix_acl_entry {
    short            e_tag;
    unsigned short        e_perm;
    union {
        kuid_t        e_uid;
        kgid_t        e_gid;
#ifndef CONFIG_UIDGID_STRICT_TYPE_CHECKS
        unsigned int    e_id;
#endif
    };
};

struct posix_acl {
    union {
        atomic_t        a_refcount;
        struct rcu_head        a_rcu;
    };
    unsigned int        a_count;
    struct posix_acl_entry    a_entries[0];
};

#define MAX_ERRNO    4095
#define IS_ERR_VALUE(x) ((x) >= (unsigned long)-MAX_ERRNO)

#define __must_check 
//inline void * __must_check ERR_PTR(long error)
//{
//    return (void *) error;
//}
#define ERR_PTR(error) (void *)error
#define PTR_ERR(ptr) (long)ptr
#define IS_ERR(ptr) IS_ERR_VALUE((unsigned long)ptr)
#define IS_ERR_OR_NULL(ptr) (!ptr || IS_ERR_VALUE((unsigned long)ptr))

//inline long __must_check PTR_ERR(const void *ptr)
//{
//    return (long) ptr;
//}
//
//inline long __must_check IS_ERR(const void *ptr)
//{
//    return IS_ERR_VALUE((unsigned long)ptr);
//}

//inline long __must_check IS_ERR_OR_NULL(const void *ptr)
//{
//    return !ptr || IS_ERR_VALUE((unsigned long)ptr);
//}

struct posix_acl *
posix_acl_from_xattr(const void *value, size_t size);
struct posix_acl *
posix_acl_alloc(int count, gfp_t flags);
int
posix_acl_to_xattr(const struct posix_acl *acl, void *buffer, size_t size);
int
posix_acl_valid(const struct posix_acl *acl);
/****************************************************/

int permission_walk(struct ceph_mount_info *cmount, const char *path, uid_t uid, gid_t gid, int perm_chk);
int permission_walk_parent(struct ceph_mount_info *cmount, const char *path, uid_t uid, gid_t gid, int perm_chk);
int fuse_init_acl(struct ceph_mount_info *cmount, const char *path, umode_t i_mode);
int fuse_disable_acl_mask(struct ceph_mount_info *cmount, const char *path);
int fuse_inherit_acl(struct ceph_mount_info *cmount, const char *path);
/*posix_acl.cpp end*/

#ifdef __cplusplus
}
#endif

#endif
