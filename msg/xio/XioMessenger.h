// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Portions Copyright (C) 2013 CohortFS, LLC
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef XIO_MESSENGER_H
#define XIO_MESSENGER_H

#include "msg/SimplePolicyMessenger.h"
extern "C" {
#include "libxio.h"
}
#include "XioConnection.h"
#include "XioPortal.h"
#include "QueueStrategy.h"
#include "include/atomic.h"
#include "common/Thread.h"
#include "common/Mutex.h"
#include "include/Spinlock.h"

class XioMessenger : public SimplePolicyMessenger
{
private:
  static atomic_t nInstances;
  atomic_t nsessions;
  atomic_t shutdown_called;
  Spinlock conns_sp;
  XioConnection::ConnList conns_list;
  XioConnection::EntitySet conns_entity_map;
  XioPortals portals;
  DispatchStrategy* dispatch_strategy;
  XioLoopbackConnection loop_con;
  uint32_t special_handling;
  Mutex sh_mtx;
  Cond sh_cond;

  friend class XioConnection;

public:
  XioMessenger(CephContext *cct, entity_name_t name,
	       string mname, uint64_t nonce,
	       DispatchStrategy* ds = new QueueStrategy(1));

  virtual ~XioMessenger();

  XioPortal* default_portal() { return portals.get_portal0(); }

  virtual void set_myaddr(const entity_addr_t& a) {
    Messenger::set_myaddr(a);
    loop_con.set_peer_addr(a);
  }

  int _send_message(Message *m, const entity_inst_t &dest);
  int _send_message(Message *m, Connection *con);
  int _send_message_impl(Message *m, XioConnection *xcon);

  uint32_t get_magic() { return magic; }
  void set_magic(int _magic) { magic = _magic; }
  uint32_t get_special_handling() { return special_handling; }
  void set_special_handling(int n) { special_handling = n; }
  int pool_hint(uint32_t size);
  void try_insert(XioConnection *xcon);

  /* xio hooks */
  int new_session(struct xio_session *session,
		  struct xio_new_session_req *req,
		  void *cb_user_context);

  int session_event(struct xio_session *session,
		    struct xio_session_event_data *event_data,
		    void *cb_user_context);

  /* Messenger interface */
  virtual void set_addr_unknowns(entity_addr_t &addr)
    { } /* XXX applicable? */

  virtual int get_dispatch_queue_len()
    { return 0; } /* XXX bogus? */

  virtual double get_dispatch_queue_max_age(utime_t now)
    { return 0; } /* XXX bogus? */

  virtual void set_cluster_protocol(int p)
    { }

  virtual int bind(const entity_addr_t& addr);

  virtual int rebind(const set<int>& avoid_ports);

  virtual int start();

  virtual void wait();

  virtual int shutdown();

  virtual int send_message(Message *m, const entity_inst_t &dest) {
    return _send_message(m, dest);
  }

  virtual int lazy_send_message(Message *m, const entity_inst_t& dest)
    { return EINVAL; }

  virtual int lazy_send_message(Message *m, Connection *con)
    { return EINVAL; }

  virtual ConnectionRef get_connection(const entity_inst_t& dest);

  virtual ConnectionRef get_loopback_connection();

  virtual int send_keepalive(const entity_inst_t& dest)
    { return EINVAL; }

  virtual int send_keepalive(Connection *con)
    { return EINVAL; }

  virtual void mark_down(const entity_addr_t& a);
  virtual void mark_down(Connection *con);
  virtual void mark_down_all();
  virtual void mark_down_on_empty(Connection *con);
  virtual void mark_disposable(Connection *con);

  void ds_dispatch(Message *m)
    { dispatch_strategy->ds_dispatch(m); }

protected:
  virtual void ready()
    { }

public:
};

#endif /* XIO_MESSENGER_H */
