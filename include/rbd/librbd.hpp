// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.	See file COPYING.
 *
 */

#ifndef __LIBRBD_HPP
#define __LIBRBD_HPP

#include <stdbool.h>
#include <string>
#include <list>
#include <map>
#include <vector>
#include "../rados/buffer.h"
#include "../rados/librados.hpp"
#include "librbd.h"

namespace librbd {

  using librados::IoCtx;

  class Image;
  typedef void *image_ctx_t;
  typedef void *completion_t;
  typedef void (*callback_t)(completion_t cb, void *arg);

  typedef struct {
    uint64_t id;
    uint64_t size;
    std::string name;
  } snap_info_t;

  typedef struct {
    std::string client;
    std::string cookie;
    std::string address;
  } locker_t;

  typedef rbd_image_info_t image_info_t;

  class CEPH_RBD_API ProgressContext
  {
  public:
    virtual ~ProgressContext();
    virtual int update_progress(uint64_t offset, uint64_t total) = 0;
  };

class CEPH_RBD_API RBD
{
public:
  RBD();
  ~RBD();

  // This must be dynamically allocated with new, and
  // must be released with release().
  // Do not use delete.
  struct AioCompletion {
    void *pc;
    AioCompletion(void *cb_arg, callback_t complete_cb);
    bool is_complete();
    int wait_for_complete();
    ssize_t get_return_value();
    void release();
  };

  void version(int *major, int *minor, int *extra);

  int open(IoCtx& io_ctx, Image& image, const char *name);
  int open(IoCtx& io_ctx, Image& image, const char *name, const char *snapname);
  // see librbd.h
  int open_read_only(IoCtx& io_ctx, Image& image, const char *name,
		     const char *snapname);
  int list(IoCtx& io_ctx, std::vector<std::string>& names);
  int create(IoCtx& io_ctx, const char *name, uint64_t size, int *order);
  int create2(IoCtx& io_ctx, const char *name, uint64_t size,
	      uint64_t features, int *order);
  int create3(IoCtx& io_ctx, const char *name, uint64_t size,
	      uint64_t features, int *order,
	      uint64_t stripe_unit, uint64_t stripe_count);
  int clone(IoCtx& p_ioctx, const char *p_name, const char *p_snapname,
	       IoCtx& c_ioctx, const char *c_name, uint64_t features,
	       int *c_order);
  int clone2(IoCtx& p_ioctx, const char *p_name, const char *p_snapname,
	     IoCtx& c_ioctx, const char *c_name, uint64_t features,
	     int *c_order, uint64_t stripe_unit, int stripe_count);
  int remove(IoCtx& io_ctx, const char *name);
  int remove_with_progress(IoCtx& io_ctx, const char *name, ProgressContext& pctx);
  int rename(IoCtx& src_io_ctx, const char *srcname, const char *destname);

private:
  /* We don't allow assignment or copying */
  RBD(const RBD& rhs);
  const RBD& operator=(const RBD& rhs);
};

class CEPH_RBD_API Image
{
public:
  Image();
  ~Image();

  int resize(uint64_t size);
  int resize_with_progress(uint64_t size, ProgressContext& pctx);
  int stat(image_info_t &info, size_t infosize);
  int parent_info(std::string *parent_poolname, std::string *parent_name,
		      std::string *parent_snapname);
  int old_format(uint8_t *old);
  int size(uint64_t *size);
  int features(uint64_t *features);
  int overlap(uint64_t *overlap);

  /* exclusive lock feature */
  int is_exclusive_lock_owner(bool *is_owner);

  int copy(IoCtx& dest_io_ctx, const char *destname);
  int copy2(Image& dest);
  int copy_with_progress(IoCtx& dest_io_ctx, const char *destname,
			 ProgressContext &prog_ctx);
  int copy_with_progress2(Image& dest, ProgressContext &prog_ctx);

  /* striping */
  uint64_t get_stripe_unit() const;
  uint64_t get_stripe_count() const;

  int flatten();
  int flatten_with_progress(ProgressContext &prog_ctx);
  /**
   * Returns a pair of poolname, imagename for each clone
   * of this image at the currently set snapshot.
   */
  int list_children(std::set<std::pair<std::string, std::string> > *children);

  /* advisory locking (see librbd.h for details) */
  int list_lockers(std::list<locker_t> *lockers,
		   bool *exclusive, std::string *tag);
  int lock_exclusive(const std::string& cookie);
  int lock_shared(const std::string& cookie, const std::string& tag);
  int unlock(const std::string& cookie);
  int break_lock(const std::string& client, const std::string& cookie);

  /* snapshots */
  int snap_list(std::vector<snap_info_t>& snaps);
  bool snap_exists(const char *snapname);
  int snap_create(const char *snapname);
  int snap_remove(const char *snapname);
  int snap_rollback(const char *snap_name);
  int snap_rollback_with_progress(const char *snap_name, ProgressContext& pctx);
  int snap_protect(const char *snap_name);
  int snap_unprotect(const char *snap_name);
  int snap_is_protected(const char *snap_name, bool *is_protected);
  int snap_set(const char *snap_name);

  /* I/O */
  ssize_t read(uint64_t ofs, size_t len, ceph::bufferlist& bl);
  /* @parmam op_flags see librados.h constants beginning with LIBRADOS_OP_FLAG */
  ssize_t read2(uint64_t ofs, size_t len, ceph::bufferlist& bl, int op_flags);
  int64_t read_iterate(uint64_t ofs, size_t len,
		       int (*cb)(uint64_t, size_t, const char *, void *), void *arg);
  int read_iterate2(uint64_t ofs, uint64_t len,
		    int (*cb)(uint64_t, size_t, const char *, void *), void *arg);
  /**
   * get difference between two versions of an image
   *
   * This will return the differences between two versions of an image
   * via a callback, which gets the offset and length and a flag
   * indicating whether the extent exists (1), or is known/defined to
   * be zeros (a hole, 0).  If the source snapshot name is NULL, we
   * interpret that as the beginning of time and return all allocated
   * regions of the image.  The end version is whatever is currently
   * selected for the image handle (either a snapshot or the writeable
   * head).
   *
   * @param fromsnapname start snapshot name, or NULL
   * @param ofs start offset
   * @param len len in bytes of region to report on
   * @param cb callback to call for each allocated region
   * @param arg argument to pass to the callback
   * @returns 0 on success, or negative error code on error
   */
  int diff_iterate(const char *fromsnapname,
		   uint64_t ofs, uint64_t len,
		   int (*cb)(uint64_t, size_t, int, void *), void *arg);
  ssize_t write(uint64_t ofs, size_t len, ceph::bufferlist& bl);
  /* @parmam op_flags see librados.h constants beginning with LIBRADOS_OP_FLAG */
  ssize_t write2(uint64_t ofs, size_t len, ceph::bufferlist& bl, int op_flags);
  int discard(uint64_t ofs, uint64_t len);

  int aio_write(uint64_t off, size_t len, ceph::bufferlist& bl, RBD::AioCompletion *c);
  /* @parmam op_flags see librados.h constants beginning with LIBRADOS_OP_FLAG */
  int aio_write2(uint64_t off, size_t len, ceph::bufferlist& bl,
		  RBD::AioCompletion *c, int op_flags);
  /**
   * read async from image
   *
   * The target bufferlist is populated with references to buffers
   * that contain the data for the given extent of the image.
   *
   * NOTE: If caching is enabled, the bufferlist will directly
   * reference buffers in the cache to avoid an unnecessary data copy.
   * As a result, if the user intends to modify the buffer contents
   * directly, they should make a copy first (unconditionally, or when
   * the reference count on ther underlying buffer is more than 1).
   *
   * @param off offset in image
   * @param len length of read
   * @param bl bufferlist to read into
   * @param c aio completion to notify when read is complete
   */
  int aio_read(uint64_t off, size_t len, ceph::bufferlist& bl, RBD::AioCompletion *c);
  /* @parmam op_flags see librados.h constants beginning with LIBRADOS_OP_FLAG */
  int aio_read2(uint64_t off, size_t len, ceph::bufferlist& bl,
		  RBD::AioCompletion *c, int op_flags);
  int aio_discard(uint64_t off, uint64_t len, RBD::AioCompletion *c);

  int flush();
  /**
   * Start a flush if caching is enabled. Get a callback when
   * the currently pending writes are on disk.
   *
   * @param image the image to flush writes to
   * @param c what to call when flushing is complete
   * @returns 0 on success, negative error code on failure
   */
  int aio_flush(RBD::AioCompletion *c);

  /**
   * Drop any cached data for an image
   *
   * @param image the image to invalidate cached data for
   * @returns 0 on success, negative error code on failure
   */
  int invalidate_cache();

private:
  friend class RBD;

  Image(const Image& rhs);
  const Image& operator=(const Image& rhs);

  image_ctx_t ctx;
};

}

#endif
