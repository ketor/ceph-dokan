// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 * @author Sage Weil <sage@newdream.net>
 */

#ifndef CEPH_ATOMIC_H
#define CEPH_ATOMIC_H

#ifdef __CEPH__
# include "acconfig.h"
#endif

#include <stdlib.h>

/*
 * crappy slow implementation that uses a pthreads spinlock.
 */
#include <winsock2.h>

#ifdef __struct_timespec_defined
#ifndef HAVE_STRUCT_TIMESPEC
#define HAVE_STRUCT_TIMESPEC
#endif
#endif

#include <pthread.h>
#include "include/Spinlock.h"

namespace ceph {
  template <class T>
  class atomic_spinlock_t {
    mutable ceph_spinlock_t lock;
    T val;
  public:
    atomic_spinlock_t(T i=0)
      : val(i) {
      ceph_spin_init(&lock);
    }
    ~atomic_spinlock_t() {
      ceph_spin_destroy(&lock);
    }
    void set(T v) {
      ceph_spin_lock(&lock);
      val = v;
      ceph_spin_unlock(&lock);
    }
    T inc() {
      ceph_spin_lock(&lock);
      T r = ++val;
      ceph_spin_unlock(&lock);
      return r;
    }
    T dec() {
      ceph_spin_lock(&lock);
      T r = --val;
      ceph_spin_unlock(&lock);
      return r;
    }
    void add(T d) {
      ceph_spin_lock(&lock);
      val += d;
      ceph_spin_unlock(&lock);
    }
    void sub(T d) {
      ceph_spin_lock(&lock);
      val -= d;
      ceph_spin_unlock(&lock);
    }
    T read() const {
      T ret;
      ceph_spin_lock(&lock);
      ret = val;
      ceph_spin_unlock(&lock);
      return ret;
    }
  private:
    // forbid copying
    atomic_spinlock_t(const atomic_spinlock_t<T> &other);
    atomic_spinlock_t &operator=(const atomic_spinlock_t<T> &rhs);
  };
}

#ifndef NO_ATOMIC_OPS

// libatomic_ops implementation
#define AO_REQUIRE_CAS

#else
/*
 * crappy slow implementation that uses a pthreads spinlock.
 */
#include "include/Spinlock.h"

namespace ceph {
  typedef atomic_spinlock_t<unsigned> atomic_t;
  typedef atomic_spinlock_t<unsigned long long> atomic64_t;
}

#endif
#endif
