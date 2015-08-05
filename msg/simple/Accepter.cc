// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <sys/socket.h>
#include <netinet/tcp.h>
#include <sys/uio.h>
#include <limits.h>
#include <poll.h>

#include "msg/Message.h"

#include "Accepter.h"
#include "Pipe.h"
#include "SimpleMessenger.h"

#include "common/debug.h"
#include "common/errno.h"

#define dout_subsys ceph_subsys_ms

#undef dout_prefix
#define dout_prefix *_dout << "accepter."


/********************************************
 * Accepter
 */

int Accepter::bind(const entity_addr_t &bind_addr, const set<int>& avoid_ports)
{
  return 0;
}

int Accepter::rebind(const set<int>& avoid_ports)
{
  return 0;
}

int Accepter::start()
{
  ldout(msgr->cct,1) << "accepter.start" << dendl;

  // start thread
  create();

  return 0;
}

void *Accepter::entry()
{
  return 0;
}

void Accepter::stop()
{
  done = true;
  ldout(msgr->cct,10) << "stop accepter" << dendl;

  //if (listen_sd >= 0) {
  //  ::shutdown(listen_sd, SHUT_RDWR);
  //}

  // wait for thread to stop before closing the socket, to avoid
  // racing against fd re-use.
  join();

  //if (listen_sd >= 0) {
  //  ::close(listen_sd);
  //  listen_sd = -1;
  //}
  done = false;
}




