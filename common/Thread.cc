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
 */

#include "common/Thread.h"
#include "common/code_environment.h"
#include "common/debug.h"
#include "common/signal.h"
#include "common/io_priority.h"

#include <dirent.h>
#include <errno.h>
#include <iostream>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>


Thread::Thread()
  : thread_id(0),
    pid(0),
    ioprio_class(-1),
    ioprio_priority(-1)
{
}

Thread::~Thread()
{
}

void *Thread::_entry_func(void *arg) {
  void *r = ((Thread*)arg)->entry_wrapper();
  return r;
}

void *Thread::entry_wrapper()
{
  int p = ceph_gettid(); // may return -ENOSYS on other platforms
  if (p > 0)
    pid = p;
  if (pid &&
      ioprio_class >= 0 &&
      ioprio_priority >= 0) {
    ceph_ioprio_set(IOPRIO_WHO_PROCESS,
		    pid,
		    IOPRIO_PRIO_VALUE(ioprio_class, ioprio_priority));
  }
  return entry();
}

const pthread_t &Thread::get_thread_id()
{
  return thread_id;
}

bool Thread::is_started() const
{
  return thread_id != 0;
}

bool Thread::am_self()
{
  return (pthread_self() == thread_id);
}

int Thread::kill(int signal)
{
  if (thread_id)
    return pthread_kill(thread_id, signal);
  else
    return -EINVAL;
}

int Thread::try_create(size_t stacksize)
{
  pthread_attr_t *thread_attr = NULL;
  stacksize &= CEPH_PAGE_MASK;  // must be multiple of page
  if (stacksize) {
    thread_attr = (pthread_attr_t*) malloc(sizeof(pthread_attr_t));
    if (!thread_attr)
      return -ENOMEM;
    pthread_attr_init(thread_attr);
    pthread_attr_setstacksize(thread_attr, stacksize);
  }

  int r;

  // The child thread will inherit our signal mask.  Set our signal mask to
  // the set of signals we want to block.  (It's ok to block signals more
  // signals than usual for a little while-- they will just be delivered to
  // another thread or delieverd to this thread later.)
  sigset_t old_sigset;
  if (g_code_env == CODE_ENVIRONMENT_LIBRARY) {
    block_signals(NULL, &old_sigset);
  }
  else {
    int to_block[] = { SIGPIPE , 0 };
    block_signals(to_block, &old_sigset);
  }
  r = pthread_create(&thread_id, thread_attr, _entry_func, (void*)this);
  restore_sigset(&old_sigset);

  if (thread_attr)
    free(thread_attr);
  return r;
}

void Thread::create(size_t stacksize)
{
  int ret = try_create(stacksize);
  if (ret != 0) {
    char buf[256];
    snprintf(buf, sizeof(buf), "Thread::try_create(): pthread_create "
	     "failed with error %d", ret);
    dout_emergency(buf);
    assert(ret == 0);
  }
}

int Thread::join(void **prval)
{
  if (thread_id == 0) {
    assert("join on thread that was never started" == 0);
    return -EINVAL;
  }

  int status = pthread_join(thread_id, prval);
  assert(status == 0);
  thread_id = 0;
  return status;
}

int Thread::detach()
{
  return pthread_detach(thread_id);
}

int Thread::set_ioprio(int cls, int prio)
{
  // fixme, maybe: this can race with create()
  ioprio_class = cls;
  ioprio_priority = prio;
  if (pid && cls >= 0 && prio >= 0)
    return ceph_ioprio_set(IOPRIO_WHO_PROCESS,
			   pid,
			   IOPRIO_PRIO_VALUE(cls, prio));
  return 0;
}
