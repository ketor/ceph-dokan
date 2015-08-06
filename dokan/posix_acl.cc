/* Extended attribute names */

#include "posix_acl.h"

#include <string>

#define    EPERM         1    /* Operation not permitted */
#define    ENOENT        2    /* No such file or directory */
#define    ESRCH         3    /* No such process */
#define    EINTR         4    /* Interrupted system call */
#define    EIO           5    /* I/O error */
#define    ENXIO         6    /* No such device or address */
#define    E2BIG         7    /* Argument list too long */
#define    ENOEXEC       8    /* Exec format error */
#define    EBADF         9    /* Bad file number */
#define    ECHILD        10    /* No child processes */
#define    EAGAIN        11    /* Try again */
#define    ENOMEM        12    /* Out of memory */
#define    EACCES        13    /* Permission denied */
#define    EFAULT        14    /* Bad address */
#define    ENOTBLK       15    /* Block device required */
#define    EBUSY         16    /* Device or resource busy */
#define    EEXIST        17    /* File exists */
#define    EXDEV         18    /* Cross-device link */
#define    ENODEV        19    /* No such device */
#define    ENOTDIR       20    /* Not a directory */
#define    EISDIR        21    /* Is a directory */
#define    EINVAL        22    /* Invalid argument */
#define    ENFILE        23    /* File table overflow */
#define    EMFILE        24    /* Too many open files */
#define    ENOTTY        25    /* Not a typewriter */
#define    ETXTBSY       26    /* Text file busy */
#define    EFBIG         27    /* File too large */
#define    ENOSPC        28    /* No space left on device */
#define    ESPIPE        29    /* Illegal seek */
#define    EROFS         30    /* Read-only file system */
#define    EMLINK        31    /* Too many links */
#define    EPIPE         32    /* Broken pipe */
#define    EDOM          33    /* Math argument out of domain of func */
#define    ERANGE        34    /* Math result not representable */
#define    EOPNOTSUPP    95    /* Operation not supported on transport endpoint */
/*
 * The virtio configuration space is defined to be little-endian.  x86 is
 * little-endian too, but it's nice to be explicit so we have these helpers.
 */
#define cpu_to_le16(v16) (v16)
#define cpu_to_le32(v32) (v32)
#define cpu_to_le64(v64) (v64)
#define le16_to_cpu(v16) (v16)
#define le32_to_cpu(v32) (v32)
#define le64_to_cpu(v64) (v64)

typedef unsigned int u32;

/**
 * ERR_CAST - Explicitly cast an error-valued pointer to another pointer type
 * @ptr: The pointer to cast.
 *
 * Explicitly cast an error-valued pointer to another pointer type in such a
 * way as to make it clear that's what's going on.
 */
inline void * __must_check ERR_CAST(const void *ptr)
{
    /* cast away the const */
    return (void *) ptr;
}

inline int __must_check PTR_RET(const void *ptr)
{
    if (IS_ERR(ptr))
        return PTR_ERR(ptr);
    else
        return 0;
}

#define UID_GID_MAP_MAX_EXTENTS 5

struct uid_gid_map {    /* 64 bytes -- 1 cache line */
    u32 nr_extents;
    struct uid_gid_extent {
        u32 first;
        u32 lower_first;
        u32 count;
    } extent[UID_GID_MAP_MAX_EXTENTS];
};

/*********************
typedef struct {
    int counter;
} atomic_t;
**********************/


struct kref {
    atomic_t refcount;
};

/********************
typedef uid_t kuid_t;
typedef gid_t kgid_t;
*********************/




/********************************************
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
*********************************************/




/**
 * struct callback_head - callback structure for use with RCU and task_work
 * @next: next update requests in a list
 * @func: actual update function to call after the grace period.
 */
 /******************************************
struct callback_head {
    struct callback_head *next;
    void (*func)(struct callback_head *head);
};
#define rcu_head callback_head



struct posix_acl {
    union {
        atomic_t        a_refcount;
        struct rcu_head        a_rcu;
    };
    unsigned int        a_count;
    struct posix_acl_entry    a_entries[0];
};
************************************************/

#define KUIDT_INIT(value) ((kuid_t) value )
#define KGIDT_INIT(value) ((kgid_t) value )

#define smp_read_barrier_depends() do { } while(0)
u32 map_id_down(struct uid_gid_map *map, u32 id)
{
    unsigned idx, extents;
    u32 first, last;

    /* Find the matching extent */
    extents = map->nr_extents;
    smp_read_barrier_depends();
    for (idx = 0; idx < extents; idx++) {
        first = map->extent[idx].first;
        last = first + map->extent[idx].count - 1;
        if (id >= first && id <= last)
            break;
    }
    /* Map the id or note failure */
    if (idx < extents)
        id = (id - first) + map->extent[idx].lower_first;
    else
        id = (u32) -1;

    return id;
}

inline kuid_t make_kuid(uid_t uid)
{
    return KUIDT_INIT(uid);
}

inline kgid_t make_kgid(gid_t gid)
{
    return KGIDT_INIT(gid);
}

inline uid_t from_kuid(kuid_t kuid)
{
    return (uid_t)kuid;
}

inline gid_t from_kgid(kgid_t kgid)
{
    return (gid_t)kgid;
}

/*
 * Free an ACL handle.
 */
inline void
posix_acl_release(struct posix_acl *acl)
{
    //if (acl && atomic_dec_and_test(&acl->a_refcount))
    //    kfree_rcu(acl, a_rcu);            
    if(acl) free(acl);
}

#define __bitwise
typedef unsigned short __u16;
typedef unsigned int   __u32;

typedef __u16 __bitwise __le16;
typedef __u16 __bitwise __be16;
typedef __u32 __bitwise __le32;
typedef __u32 __bitwise __be32;

typedef struct {
    __le16            e_tag;
    __le16            e_perm;
    __le32            e_id;
} posix_acl_xattr_entry;

typedef struct {
    __le32            a_version;
    posix_acl_xattr_entry    a_entries[0];
} posix_acl_xattr_header;

/* Supported ACL a_version fields */
#define POSIX_ACL_XATTR_VERSION    0x0002

inline int
posix_acl_xattr_count(size_t size)
{
    if (size < sizeof(posix_acl_xattr_header))
        return -1;
    size -= sizeof(posix_acl_xattr_header);
    if (size % sizeof(posix_acl_xattr_entry))
        return -1;
    return size / sizeof(posix_acl_xattr_entry);
}

#define __force
typedef unsigned gfp_t;
#define ___GFP_WAIT        0x10u
#define ___GFP_IO        0x40u
#define __GFP_WAIT    ((__force gfp_t)___GFP_WAIT)    /* Can wait and reschedule? */
#define __GFP_IO    ((__force gfp_t)___GFP_IO)    /* Can start physical IO? */
#define GFP_NOIO    (__GFP_WAIT)
#define GFP_NOFS    (__GFP_WAIT | __GFP_IO)

#define kmalloc(a,b) malloc(a)

#define atomic_set(v,i)        ((v)->counter = (i))
/*
 * Init a fresh posix_acl
 */
void
posix_acl_init(struct posix_acl *acl, int count)
{
    atomic_set(&acl->a_refcount, 1);
    acl->a_count = count;
}

/*
 * Allocate a new ACL with the specified number of entries.
 */
struct posix_acl *
posix_acl_alloc(int count, gfp_t flags)
{
    const size_t size = sizeof(struct posix_acl) +
                        count * sizeof(struct posix_acl_entry);
    struct posix_acl *acl = (struct posix_acl *)kmalloc(size, flags);
    if (acl)
        posix_acl_init(acl, count);
    return acl;
}

#define INVALID_UID KUIDT_INIT(-1)
#define INVALID_GID KGIDT_INIT(-1)

#define __kuid_val(a) a
#define __kgid_val(a) a

inline bool uid_eq(kuid_t left, kuid_t right)
{
    return __kuid_val(left) == __kuid_val(right);
}

inline bool gid_eq(kgid_t left, kgid_t right)
{
    return __kgid_val(left) == __kgid_val(right);
}

inline bool uid_valid(kuid_t uid)
{
    return !uid_eq(uid, INVALID_UID);
}

inline bool gid_valid(kgid_t gid)
{
    return !gid_eq(gid, INVALID_GID);
}

static inline bool uid_lte(kuid_t left, kuid_t right)
{
	return __kuid_val(left) <= __kuid_val(right);
}

static inline bool gid_lte(kgid_t left, kgid_t right)
{
	return __kgid_val(left) <= __kgid_val(right);
}

/**
 * kmemdup - duplicate region of memory
 *
 * @src: memory region to duplicate
 * @len: memory region length
 * @gfp: GFP mask to use
 */
void *kmemdup(const void *src, size_t len, gfp_t gfp)
{
    void *p;

    //p = kmalloc_track_caller(len, gfp);
    p = malloc(len);
    if (p)
        memcpy(p, src, len);
    return p;
}

/*
 * Clone an ACL.
 */
struct posix_acl *
posix_acl_clone(const struct posix_acl *acl, gfp_t flags)
{
    struct posix_acl *clone = NULL;

    if (acl) {
        int size = sizeof(struct posix_acl) + acl->a_count *
                   sizeof(struct posix_acl_entry);
        clone = (struct posix_acl *)kmemdup(acl, size, flags);
        if (clone)
            atomic_set(&clone->a_refcount, 1);
    }
    return clone;
}

/*
 * Modify acl when creating a new inode. The caller must ensure the acl is
 * only referenced once.
 *
 * mode_p initially must contain the mode parameter to the open() / creat()
 * system calls. All permissions that are not granted by the acl are removed.
 * The permissions in the acl are changed to reflect the mode_p parameter.
 */
int posix_acl_create_masq(struct posix_acl *acl, umode_t *mode_p)
{
    struct posix_acl_entry *pa, *pe;
    struct posix_acl_entry *group_obj = NULL, *mask_obj = NULL;
    umode_t mode = *mode_p;
    int not_equiv = 0;

    /* assert(atomic_read(acl->a_refcount) == 1); */

    FOREACH_ACL_ENTRY(pa, acl, pe) {
                switch(pa->e_tag) {
                        case ACL_USER_OBJ:
                pa->e_perm &= (mode >> 6) | ~S_IRWXO;
                mode &= (pa->e_perm << 6) | ~S_IRWXU;
                break;

            case ACL_USER:
            case ACL_GROUP:
                not_equiv = 1;
                break;

                        case ACL_GROUP_OBJ:
                group_obj = pa;
                                break;

                        case ACL_OTHER:
                pa->e_perm &= mode | ~S_IRWXO;
                mode &= pa->e_perm | ~S_IRWXO;
                                break;

                        case ACL_MASK:
                mask_obj = pa;
                not_equiv = 1;
                                break;

            default:
                return -EIO;
                }
        }

    if (mask_obj) {
        mask_obj->e_perm &= (mode >> 3) | ~S_IRWXO;
        mode &= (mask_obj->e_perm << 3) | ~S_IRWXG;
    } else {
        if (!group_obj)
            return -EIO;
        group_obj->e_perm &= (mode >> 3) | ~S_IRWXO;
        mode &= (group_obj->e_perm << 3) | ~S_IRWXG;
    }

    *mode_p = (*mode_p & ~S_IRWXUGO) | mode;
        return not_equiv;
}


int
posix_acl_create(struct posix_acl **acl, gfp_t gfp, umode_t *mode_p)
{
    struct posix_acl *clone = posix_acl_clone(*acl, gfp);
    int err = -ENOMEM;
    if (clone) {
        err = posix_acl_create_masq(clone, mode_p);
        if (err < 0) {
            posix_acl_release(clone);
            clone = NULL;
        }
    }
    posix_acl_release(*acl);
    *acl = clone;
    return err;
}

inline size_t
posix_acl_xattr_size(int count)
{
    return (sizeof(posix_acl_xattr_header) +
        (count * sizeof(posix_acl_xattr_entry)));
}

/*
 * Convert from extended attribute to in-memory representation.
 */
struct posix_acl *
posix_acl_from_xattr(const void *value, size_t size)
{
    posix_acl_xattr_header *header = (posix_acl_xattr_header *)value;
    posix_acl_xattr_entry *entry = (posix_acl_xattr_entry *)(header+1), *end;
    int count;
    struct posix_acl *acl;
    struct posix_acl_entry *acl_e;

    if (!value)
        return NULL;
    if (size < sizeof(posix_acl_xattr_header))
         return (posix_acl * )ERR_PTR(-EINVAL);
    if (header->a_version != cpu_to_le32(POSIX_ACL_XATTR_VERSION))
        return (posix_acl * )ERR_PTR(-EOPNOTSUPP);

    count = posix_acl_xattr_count(size);
    if (count < 0)
        return (posix_acl * )ERR_PTR(-EINVAL);
    if (count == 0)
        return NULL;
    
    acl = posix_acl_alloc(count, GFP_NOFS);
    if (!acl)
        return (posix_acl * )ERR_PTR(-ENOMEM);
    acl_e = acl->a_entries;
    
    for (end = entry + count; entry != end; acl_e++, entry++) {
        acl_e->e_tag  = le16_to_cpu(entry->e_tag);
        acl_e->e_perm = le16_to_cpu(entry->e_perm);

        switch(acl_e->e_tag) {
            case ACL_USER_OBJ:
            case ACL_GROUP_OBJ:
            case ACL_MASK:
            case ACL_OTHER:
                break;

            case ACL_USER:
                acl_e->e_uid =
                    make_kuid(le32_to_cpu(entry->e_id));
                if (!uid_valid(acl_e->e_uid))
                    goto fail;
                break;
            case ACL_GROUP:
                acl_e->e_gid =
                    make_kgid(le32_to_cpu(entry->e_id));
                if (!gid_valid(acl_e->e_gid))
                    goto fail;
                break;

            default:
                goto fail;
        }
    }
    return acl;

fail:
    posix_acl_release(acl);
    return (posix_acl * )ERR_PTR(-EINVAL);
}

/*
 * Convert from in-memory to extended attribute representation.
 */
int
posix_acl_to_xattr(const struct posix_acl *acl,
           void *buffer, size_t size)
{
    posix_acl_xattr_header *ext_acl = (posix_acl_xattr_header *)buffer;
    posix_acl_xattr_entry *ext_entry = ext_acl->a_entries;
    size_t real_size, n;

    real_size = posix_acl_xattr_size(acl->a_count);
    if (!buffer)
        return real_size;
    if (real_size > size)
        return -ERANGE;
    
    ext_acl->a_version = cpu_to_le32(POSIX_ACL_XATTR_VERSION);

    for (n=0; n < acl->a_count; n++, ext_entry++) {
        const struct posix_acl_entry *acl_e = &acl->a_entries[n];
        ext_entry->e_tag  = cpu_to_le16(acl_e->e_tag);
        ext_entry->e_perm = cpu_to_le16(acl_e->e_perm);
        switch(acl_e->e_tag) {
        case ACL_USER:
            ext_entry->e_id =
                cpu_to_le32(from_kuid(acl_e->e_uid));
            break;
        case ACL_GROUP:
            ext_entry->e_id =
                cpu_to_le32(from_kgid(acl_e->e_gid));
            break;
        default:
            ext_entry->e_id = cpu_to_le32(ACL_UNDEFINED_ID);
            break;
        }
    }
    return real_size;
}

/*
 * Keep mostly read-only and often accessed (especially for
 * the RCU path lookup and 'stat' data) fields at the beginning
 * of the 'struct inode_cxt'
 */
struct inode_cxt {
    kuid_t            i_uid;
    kgid_t            i_gid;
};

#define in_group_p(a)   1

/*
 * Return 0 if current is granted want access to the inode_cxt
 * by the acl. Returns -E... otherwise.
 */
int
posix_acl_permission(struct inode_cxt *inode_cxt, struct inode_cxt *env_cxt, const struct posix_acl *acl, int want)
{
    const struct posix_acl_entry *pa, *pe, *mask_obj;
    int found = 0;

    want &= MAY_READ | MAY_WRITE | MAY_EXEC | MAY_NOT_BLOCK;

    FOREACH_ACL_ENTRY(pa, acl, pe) {
                switch(pa->e_tag) {
                        case ACL_USER_OBJ:
                /* (May have been checked already) */
                if (uid_eq(inode_cxt->i_uid, env_cxt->i_uid))
                                        goto check_perm;
                                break;
                        case ACL_USER:
                if (uid_eq(pa->e_uid, env_cxt->i_uid))
                                        goto mask;
                break;
                        case ACL_GROUP_OBJ:
                                if (gid_eq(inode_cxt->i_gid, env_cxt->i_gid)) {
                    found = 1;
                    if ((pa->e_perm & want) == want)
                        goto mask;
                                }
                break;
                        case ACL_GROUP:
                if (gid_eq(pa->e_gid, env_cxt->i_gid)) {
                    found = 1;
                    if ((pa->e_perm & want) == want)
                        goto mask;
                                }
                                break;
                        case ACL_MASK:
                                break;
                        case ACL_OTHER:
                if (found)
                    return -EACCES;
                else
                    goto check_perm;
            default:
                return -EIO;
                }
        }
    return -EIO;

mask:
    for (mask_obj = pa+1; mask_obj != pe; mask_obj++) {
        if (mask_obj->e_tag == ACL_MASK) {
            if ((pa->e_perm & mask_obj->e_perm & want) == want)
                return 0;
            return -EACCES;
        }
    }

check_perm:
    if ((pa->e_perm & want) == want)
        return 0;
    return -EACCES;
}

int permission_walk_ugo(struct ceph_mount_info *cmount, const char *path, uid_t uid, gid_t gid,
                           int perm_chk, int readlink = 0){
    int rt;
    //I'm root~~
    if(uid == 0){
        return 0;
    }
    
    struct stat stbuf;
    int chk = perm_chk;
    int res = ceph_stat(cmount, path, &stbuf);
    if(res){
        rt =  res;
        goto __free_quit;
    }

    int mr,mw,mx;
    if(stbuf.st_uid == uid){
        mr = S_IRUSR;
        mw = S_IWUSR;
        mx = S_IXUSR;
    }else if(stbuf.st_gid == gid){
        mr = S_IRGRP;
        mw = S_IWGRP;
        mx = S_IXGRP;
    }else{
        mr = S_IROTH;
        mw = S_IWOTH;
        mx = S_IXOTH;
    }
    if(chk & PERM_WALK_CHECK_READ){
        if(!(stbuf.st_mode & mr)){
            rt = -EACCES;
            goto __free_quit;
        }
    }
    if(chk & PERM_WALK_CHECK_WRITE){
        if(!(stbuf.st_mode & mw)){
            rt = -EACCES;
            goto __free_quit;
        }
    }
    if(chk & PERM_WALK_CHECK_EXEC){
        if(!(stbuf.st_mode & mx)){
            rt = -EACCES;
            goto __free_quit;
        }
    }

    rt = 0;
__free_quit:
    return rt;
}

int fuse_check_acl(struct ceph_mount_info *cmount, const char *path, const char *acl_xattr, int length, kuid_t uid, kgid_t gid, int mask)
{
    int error=-EAGAIN;
        
    struct posix_acl *acl;
    acl = posix_acl_from_xattr(acl_xattr, length);
    if(IS_ERR(acl)) {
        error = PTR_ERR(acl);
        printf("error1 = %d\n", error);
        return error;
    }
    
    struct inode_cxt inode;
    struct stat stbuf;
    int ret = ceph_lstat(cmount, path, &stbuf);
    if(ret == -1){
        return -101;
    }
    inode.i_uid = stbuf.st_uid;
    inode.i_gid = stbuf.st_gid;
    
    struct inode_cxt evn_cxt;
    evn_cxt.i_uid = uid;
    evn_cxt.i_gid = gid;
    
    error = posix_acl_permission(&inode, &evn_cxt, acl, mask);
    posix_acl_release(acl);
    
    return error;
}

/*check ACL first, if ACL does not exists, check UGO*/
int permission_walk(struct ceph_mount_info *cmount, const char *path, uid_t uid, gid_t gid, int perm_chk)
{
    //I'm root~~
    if(uid == 0){
        return 0;
    }
    
    char acl_xattr[XATTR_MAX_SIZE];
    memset(acl_xattr, 0x00, sizeof(acl_xattr));
    
    int length = 0;
    length = ceph_getxattr(cmount, path, POSIX_ACL_XATTR_ACCESS, acl_xattr, XATTR_MAX_SIZE);
    if(length <= 0){
        return permission_walk_ugo(cmount, path, uid, gid, perm_chk, 0);
    }
    else{
        return fuse_check_acl(cmount, path, acl_xattr, length, uid, gid, perm_chk);
    }
}

int permission_walk_parent(struct ceph_mount_info *cmount, const char *path, uid_t uid, gid_t gid, int perm_chk)
{
    int l = strlen(path);
    while(--l)
        if(path[l] == '/')
            break;
    return permission_walk(cmount, std::string(path, l).c_str(), uid, gid, perm_chk);
}

int fuse_init_acl(struct ceph_mount_info *cmount, const char *path, umode_t i_mode)
{
    //fprintf(stderr, "%s %d\n", path, i_mode);
    int error=-EAGAIN;
    
    /*get parent dir's default ACL*/
    int l = strlen(path);
    while(--l)
        if(path[l] == '/')
            break;
    
    char acl_xattr[XATTR_MAX_SIZE];
    memset(acl_xattr, 0x00, sizeof(acl_xattr));
    
    int length = 0;
    length = ceph_getxattr(cmount, std::string(path, l).c_str(), POSIX_ACL_XATTR_DEFAULT, acl_xattr, XATTR_MAX_SIZE);
    if(length <= 0){
        return 0;
    }
    
    struct posix_acl *acl;
    acl = posix_acl_from_xattr(acl_xattr, length);
    if(IS_ERR(acl)) {
        error = PTR_ERR(acl);
        printf("error1 = %d\n", error);
        return error;
    }
    
    /*make acl from parent's default acl*/
    if (acl) {
        char buffer[XATTR_MAX_SIZE];

        if (S_ISDIR(i_mode)) {
            //error = ext4_set_acl(handle, inode,
            //             ACL_TYPE_DEFAULT, acl);
            memset(buffer, 0x00, sizeof(buffer));
            int real_len = posix_acl_to_xattr(acl, buffer, XATTR_MAX_SIZE);
            error = ceph_setxattr(cmount, path, POSIX_ACL_XATTR_DEFAULT, buffer, real_len, 0);
            if (error){
                fprintf(stderr, "ceph_setxattr1 error %s %d\n", path, error);
                goto cleanup;
            }
        }
        error = posix_acl_create(&acl, GFP_NOFS, &i_mode);
        if (error < 0)
            return error;

        if (error > 0) {
            /* This is an extended ACL */
            //error = ext4_set_acl(handle, inode, ACL_TYPE_ACCESS, acl);
            memset(buffer, 0x00, sizeof(buffer));
            int real_len = posix_acl_to_xattr(acl, buffer, XATTR_MAX_SIZE);
            error = ceph_setxattr(cmount, path, POSIX_ACL_XATTR_ACCESS, buffer, real_len, 0);
            if (error){
                fprintf(stderr, "ceph_setxattr2 error %s %d\n", path, error);
                goto cleanup;
            }
        }
    }
cleanup:
    posix_acl_release(acl);
    return error;
}

int fuse_disable_acl_mask(struct ceph_mount_info *cmount, const char *path)
{
    int error=-EAGAIN;
    
    char acl_xattr[XATTR_MAX_SIZE];
    memset(acl_xattr, 0x00, sizeof(acl_xattr));
    
    int length = ceph_getxattr(cmount, path, POSIX_ACL_XATTR_ACCESS, acl_xattr, XATTR_MAX_SIZE);
    if(length <= 0){
        return 0;
    }
    
    struct posix_acl *acl;
    acl = posix_acl_from_xattr(acl_xattr, length);
    if(IS_ERR(acl)) {
        error = PTR_ERR(acl);
        return error;
    }
    
    if (acl) {
        struct posix_acl_entry *pa, *pe;
        
        FOREACH_ACL_ENTRY(pa, acl, pe) {
            switch(pa->e_tag) {
                case ACL_USER_OBJ:
                case ACL_USER:
                case ACL_GROUP_OBJ:
                case ACL_GROUP:
                case ACL_OTHER:
                    break;
                case ACL_MASK:
                    pa->e_perm = 7;
                    break;
                default:
                    break;
            }
        }
        
        char buffer[XATTR_MAX_SIZE];
        memset(buffer, 0x00, sizeof(buffer));
        
        int real_len = posix_acl_to_xattr(acl, buffer, XATTR_MAX_SIZE);
        error = ceph_setxattr(cmount, path, POSIX_ACL_XATTR_ACCESS, buffer, real_len, 0);
        if (error) goto cleanup;
    }
cleanup:
    posix_acl_release(acl);
    return error;
}

/*
path need to be full-path
*/
int fuse_inherit_acl(struct ceph_mount_info *cmount, const char *path)
{
    int error=-EAGAIN;
    
    /*get parent dir's default ACL*/
    int l = strlen(path);
    while(--l)
        if(path[l] == '/')
            break;
    
    char acl_xattr[XATTR_MAX_SIZE];
    memset(acl_xattr, 0x00, sizeof(acl_xattr));
    
    int length = ceph_getxattr(cmount, std::string(path, l).c_str(), POSIX_ACL_XATTR_ACCESS, acl_xattr, XATTR_MAX_SIZE);
    if(length <= 0){
        return 0;
    }
    
    struct posix_acl *acl;
    acl = posix_acl_from_xattr(acl_xattr, length);
    if(IS_ERR(acl)) {
        error = PTR_ERR(acl);
        return error;
    }
    
    /*make acl from parent's default acl*/
    if (acl) {
        char buffer[XATTR_MAX_SIZE];
        memset(buffer, 0x00, sizeof(buffer));
        
        int real_len = posix_acl_to_xattr(acl, buffer, XATTR_MAX_SIZE);
        error = ceph_setxattr(cmount, path, POSIX_ACL_XATTR_ACCESS, buffer, real_len, 0);
        if (error)
            goto cleanup;
    }
cleanup:
    posix_acl_release(acl);
    return error;
}

/*
 * Check if an acl is valid. Returns 0 if it is, or -E... otherwise.
 */
int
posix_acl_valid(const struct posix_acl *acl)
{
	const struct posix_acl_entry *pa, *pe;
	int state = ACL_USER_OBJ;
	kuid_t prev_uid = INVALID_UID;
	kgid_t prev_gid = INVALID_GID;
	int needs_mask = 0;

	FOREACH_ACL_ENTRY(pa, acl, pe) {
		if (pa->e_perm & ~(ACL_READ|ACL_WRITE|ACL_EXECUTE))
			return -EINVAL;
		switch (pa->e_tag) {
			case ACL_USER_OBJ:
				if (state == ACL_USER_OBJ) {
					state = ACL_USER;
					break;
				}
				return -EINVAL;

			case ACL_USER:
				if (state != ACL_USER)
					return -EINVAL;
				if (!uid_valid(pa->e_uid))
					return -EINVAL;
				if (uid_valid(prev_uid) &&
				    uid_lte(pa->e_uid, prev_uid))
					return -EINVAL;
				prev_uid = pa->e_uid;
				needs_mask = 1;
				break;

			case ACL_GROUP_OBJ:
				if (state == ACL_USER) {
					state = ACL_GROUP;
					break;
				}
				return -EINVAL;

			case ACL_GROUP:
				if (state != ACL_GROUP)
					return -EINVAL;
				if (!gid_valid(pa->e_gid))
					return -EINVAL;
				if (gid_valid(prev_gid) &&
				    gid_lte(pa->e_gid, prev_gid))
					return -EINVAL;
				prev_gid = pa->e_gid;
				needs_mask = 1;
				break;

			case ACL_MASK:
				if (state != ACL_GROUP)
					return -EINVAL;
				state = ACL_OTHER;
				break;

			case ACL_OTHER:
				if (state == ACL_OTHER ||
				    (state == ACL_GROUP && !needs_mask)) {
					state = 0;
					break;
				}
				return -EINVAL;

			default:
				return -EINVAL;
		}
	}
	if (state == 0)
		return 0;
	return -EINVAL;
}

