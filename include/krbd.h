/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Inktank Storage, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_KRBD_H
#define CEPH_KRBD_H

#ifdef __cplusplus
extern "C" {
#endif

struct krbd_ctx;
struct CephContext;

int krbd_create_from_context(struct CephContext *cct, struct krbd_ctx **pctx);
void krbd_destroy(struct krbd_ctx *ctx);

int krbd_map(struct krbd_ctx *ctx, const char *pool, const char *image,
             const char *snap, const char *options, char **pdevnode);

int krbd_unmap(struct krbd_ctx *ctx, const char *devnode);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

namespace ceph {
  class Formatter;
}

int krbd_showmapped(struct krbd_ctx *ctx, Formatter *f);

#endif /* __cplusplus */

#endif /* CEPH_KRBD_H */
