// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 UnitedStack <haomai@unitedstack.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/errno.h"
#include "EventKqueue.h"

#define dout_subsys ceph_subsys_ms

#undef dout_prefix
#define dout_prefix *_dout << "KqueueDriver."

int KqueueDriver::init(int nevent)
{
  events = (struct kevent*)malloc(sizeof(struct kevent)*nevent);
  if (!events) {
    lderr(cct) << __func__ << " unable to malloc memory: "
                           << cpp_strerror(errno) << dendl;
    return -errno;
  }
  memset(events, 0, sizeof(struct kevent)*nevent);

  kqfd = kqueue();
  if (kqfd < 0) {
    lderr(cct) << __func__ << " unable to do kqueue: "
                           << cpp_strerror(errno) << dendl;
    return -errno;
  }

  size = nevent;

  return 0;
}

int KqueueDriver::add_event(int fd, int cur_mask, int add_mask)
{
  ldout(cct, 20) << __func__ << " add event fd=" << fd << " cur_mask=" << cur_mask
                 << "add_mask" << add_mask << dendl;
  struct kevent ke;
  int filter = 0;
  filter |= add_mask & EVENT_READABLE ? EVFILT_READ : 0;
  filter |= add_mask & EVENT_WRITABLE ? EVFILT_WRITE : 0;

  if (filter) {
    EV_SET(&ke, fd, filter, EV_ADD, 0, 0, NULL);
    if (kevent(kqfd, &ke, 1, NULL, 0, NULL) == -1) {
      lderr(cct) << __func__ << " unable to add event: "
                             << cpp_strerror(errno) << dendl;
      return -1;
    }
  }

  return 0;
}

void KqueueDriver::del_event(int fd, int cur_mask, int delmask)
{
  ldout(cct, 20) << __func__ << " del event fd=" << fd << " cur mask=" << cur_mask
                 << " delmask=" << delmask << dendl;
  struct kevent ee;
  struct kevent ke;
  int filter = 0;
  filter |= delmask & EVENT_READABLE ? EVFILT_READ : 0;
  filter |= delmask & EVENT_WRITABLE ? EVFILT_WRITE : 0;

  if (filter) {
    EV_SET(&ke, fd, filter, EV_DELETE, 0, 0, NULL);
    if (kevent(kqfd, &ke, 1, NULL, 0, NULL) < 0) {
      lderr(cct) << __func__ << " kevent: delete fd=" << fd << " mask=" << filter
                 << " failed." << cpp_strerror(errno) << dendl;
    }
  }
}

int KqueueDriver::resize_events(int newsize)
{
  return 0;
}

int KqueueDriver::event_wait(vector<FiredFileEvent> &fired_events, struct timeval *tvp)
{
  int retval, numevents = 0;
  struct timespec timeout;
  timeout.tv_sec = tvp->tv_sec;
  timeout.tv_nsec = tvp->tv_usec * 1000;

  if (tvp != NULL) {
      timeout.tv_sec = tvp->tv_sec;
      timeout.tv_nsec = tvp->tv_usec * 1000;
      retval = kevent(kqfd, NULL, 0, events, size, &timeout);
  } else {
      retval = kevent(kqfd, NULL, 0, events, size, NULL);
  }

  if (retval > 0) {
    int j;

    numevents = retval;
    fired_events.resize(numevents);
    for (j = 0; j < numevents; j++) {
      int mask = 0;
      struct kevent *e = events + j;

      if (e->filter == EVFILT_READ) mask |= EVENT_READABLE;
      if (e->filter == EVFILT_WRITE) mask |= EVENT_WRITABLE;
      fired_events[j].fd = (int)e->ident;
      fired_events[j].mask = mask;

    }
  }
  return numevents;
}
