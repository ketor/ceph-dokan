// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2012 Inktank
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "fd.h"

#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "debug.h"
#include "errno.h"

void dump_open_fds(CephContext *cct)
{
  return;
}
