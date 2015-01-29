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

#include "XioMsg.h"
#include "XioConnection.h"
#include "XioMessenger.h"
#include "messages/MDataPing.h"

#include "auth/none/AuthNoneProtocol.h" // XXX

#include "include/assert.h"
#include "common/dout.h"

extern struct xio_mempool *xio_msgr_mpool;
extern struct xio_mempool *xio_msgr_noreg_mpool;

#define dout_subsys ceph_subsys_xio

void print_xio_msg_hdr(CephContext *cct, const char *tag,
		       const XioMsgHdr &hdr, const struct xio_msg *msg)
{
  if (msg) {
    ldout(cct,4) << tag <<
      " xio msg:" <<
      " sn: " << msg->sn <<
      " timestamp: " << msg->timestamp <<
      dendl;
  }

  ldout(cct,4) << tag <<
    " ceph header: " <<
    " front_len: " << hdr.hdr->front_len <<
    " seq: " << hdr.hdr->seq <<
    " tid: " << hdr.hdr->tid <<
    " type: " << hdr.hdr->type <<
    " prio: " << hdr.hdr->priority <<
    " name type: " << (int) hdr.hdr->src.type <<
    " name num: " << (int) hdr.hdr->src.num <<
    " version: " << hdr.hdr->version <<
    " compat_version: " << hdr.hdr->compat_version <<
    " front_len: " << hdr.hdr->front_len <<
    " middle_len: " << hdr.hdr->middle_len <<
    " data_len: " << hdr.hdr->data_len <<
    " xio header: " <<
    " msg_cnt: " << hdr.msg_cnt <<
    dendl;

  ldout(cct,4) << tag <<
    " ceph footer: " <<
    " front_crc: " << hdr.ftr->front_crc <<
    " middle_crc: " << hdr.ftr->middle_crc <<
    " data_crc: " << hdr.ftr->data_crc <<
    " sig: " << hdr.ftr->sig <<
    " flags: " << (uint32_t) hdr.ftr->flags <<
    dendl;
}

void print_ceph_msg(CephContext *cct, const char *tag, Message *m)
{
  if (m->get_magic() & (MSG_MAGIC_XIO & MSG_MAGIC_TRACE_DTOR)) {
    ceph_msg_header& header = m->get_header();
    ldout(cct,4) << tag << " header version " << header.version <<
      " compat version " << header.compat_version <<
      dendl;
  }
}

XioConnection::XioConnection(XioMessenger *m, XioConnection::type _type,
			     const entity_inst_t& _peer) :
  Connection(m->cct, m),
  xio_conn_type(_type),
  portal(m->default_portal()),
  connected(false),
  peer(_peer),
  session(NULL),
  conn(NULL),
  magic(m->get_magic()),
  scount(0),
  send_ctr(0),
  in_seq(),
  cstate(this)
{
  pthread_spin_init(&sp, PTHREAD_PROCESS_PRIVATE);
  if (xio_conn_type == XioConnection::ACTIVE)
    peer_addr = peer.addr;
  peer_type = peer.name.type();
  set_peer_addr(peer.addr);

  Messenger::Policy policy;
  int64_t max_msgs = 0, max_bytes = 0, bytes_opt = 0;
  int xopt;

  policy = m->get_policy(peer_type);

  if (policy.throttler_messages) {
    max_msgs = policy.throttler_messages->get_max();
    ldout(m->cct,0) << "XioMessenger throttle_msgs: " << max_msgs << dendl;
  }

  xopt = m->cct->_conf->xio_queue_depth;
  if (max_msgs > xopt)
    xopt = max_msgs;

  /* set high mark for send, reserved 20% for credits */
  q_high_mark = xopt * 4 / 5;
  q_low_mark = q_high_mark/2;

  /* set send & receive msgs queue depth */
  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_SND_QUEUE_DEPTH_MSGS,
             &xopt, sizeof(xopt));
  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_RCV_QUEUE_DEPTH_MSGS,
             &xopt, sizeof(xopt));

  if (policy.throttler_bytes) {
    max_bytes = policy.throttler_bytes->get_max();
    ldout(m->cct,0) << "XioMessenger throttle_bytes: " << max_bytes << dendl;
  }

  bytes_opt = (2 << 28); /* default: 512 MB */
  if (max_bytes > bytes_opt)
    bytes_opt = max_bytes;

  /* set send & receive total bytes throttle */
  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_SND_QUEUE_DEPTH_BYTES,
             &bytes_opt, sizeof(bytes_opt));
  xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_RCV_QUEUE_DEPTH_BYTES,
             &bytes_opt, sizeof(bytes_opt));

  ldout(m->cct,0) << "Peer type: " << peer.name.type_str() <<
        " throttle_msgs: " << xopt << " throttle_bytes: " << bytes_opt << dendl;

  /* XXXX fake features, aieee! */
  set_features(XIO_ALL_FEATURES);
}

int XioConnection::send_message(Message *m)
{
  XioMessenger *ms = static_cast<XioMessenger*>(get_messenger());
  return ms->_send_message(m, this);
}

int XioConnection::passive_setup()
{
  /* XXX passive setup is a placeholder for (potentially active-side
     initiated) feature and auth* negotiation */
  static bufferlist authorizer_reply; /* static because fake */
  static CryptoKey session_key; /* ditto */
  bool authorizer_valid;

  XioMessenger *msgr = static_cast<XioMessenger*>(get_messenger());

  // fake an auth buffer
  EntityName name;
  name.set_type(peer.name.type());

  AuthNoneAuthorizer auth;
  auth.build_authorizer(name, peer.name.num());

  /* XXX fake authorizer! */
  msgr->ms_deliver_verify_authorizer(
    this, peer_type, CEPH_AUTH_NONE,
    auth.bl,
    authorizer_reply,
    authorizer_valid,
    session_key);

  /* notify hook */
  msgr->ms_deliver_handle_accept(this);

  /* try to insert in conns_entity_map */
  msgr->try_insert(this);
  return (0);
}

#define uint_to_timeval(tv, s) ((tv).tv_sec = (s), (tv).tv_usec = 0)

static inline XioDispatchHook* pool_alloc_xio_dispatch_hook(
  XioConnection *xcon, Message *m, XioInSeq& msg_seq)
{
  struct xio_mempool_obj mp_mem;
  int e = xpool_alloc(xio_msgr_noreg_mpool,
		      sizeof(XioDispatchHook), &mp_mem);
  if (!!e)
    return NULL;
  XioDispatchHook *xhook = (XioDispatchHook*) mp_mem.addr;
  new (xhook) XioDispatchHook(xcon, m, msg_seq, mp_mem);
  return xhook;
}

int XioConnection::on_msg_req(struct xio_session *session,
			      struct xio_msg *req,
			      int more_in_batch,
			      void *cb_user_context)
{
  struct xio_msg *treq = req;

  /* XXX Accelio guarantees message ordering at
   * xio_session */

  if (! in_seq.p()) {
    if (!treq->in.header.iov_len) {
	derr << __func__ << " empty header: packet out of sequence?" << dendl;
	xio_release_msg(req);
	return 0;
    }
    XioMsgCnt msg_cnt(
      buffer::create_static(treq->in.header.iov_len,
			    (char*) treq->in.header.iov_base));
    ldout(msgr->cct,10) << __func__ << " receive req " << "treq " << treq
      << " msg_cnt " << msg_cnt.msg_cnt
      << " iov_base " << treq->in.header.iov_base
      << " iov_len " << (int) treq->in.header.iov_len
      << " nents " << treq->in.pdata_iov.nents
      << " conn " << conn << " sess " << session
      << " sn " << treq->sn << dendl;
    assert(session == this->session);
    in_seq.set_count(msg_cnt.msg_cnt);
  } else {
    /* XXX major sequence error */
    assert(! treq->in.header.iov_len);
  }

  in_seq.append(req);
  if (in_seq.count() > 0) {
    return 0;
  }

  XioMessenger *msgr = static_cast<XioMessenger*>(get_messenger());
  XioDispatchHook *m_hook =
    pool_alloc_xio_dispatch_hook(this, NULL /* msg */, in_seq);
  XioInSeq& msg_seq = m_hook->msg_seq;
  in_seq.clear();

  ceph_msg_header header;
  ceph_msg_footer footer;
  buffer::list payload, middle, data;

  struct timeval t1, t2;

  ldout(msgr->cct,4) << __func__ << " " << "msg_seq.size()="  << msg_seq.size() <<
    dendl;

  struct xio_msg* msg_iter = msg_seq.begin();
  treq = msg_iter;
  XioMsgHdr hdr(header, footer,
		buffer::create_static(treq->in.header.iov_len,
				      (char*) treq->in.header.iov_base));

  uint_to_timeval(t1, treq->timestamp);

  if (magic & (MSG_MAGIC_TRACE_XCON)) {
    if (hdr.hdr->type == 43) {
      print_xio_msg_hdr(msgr->cct, "on_msg_req", hdr, NULL);
    }
  }

  unsigned int ix, blen, iov_len;
  struct xio_iovec_ex *msg_iov, *iovs;
  uint32_t take_len, left_len = 0;
  char *left_base = NULL;

  ix = 0;
  blen = header.front_len;

  while (blen && (msg_iter != msg_seq.end())) {
    treq = msg_iter;
    iov_len = vmsg_sglist_nents(&treq->in);
    iovs = vmsg_sglist(&treq->in);
    for (; blen && (ix < iov_len); ++ix) {
      msg_iov = &iovs[ix];

      /* XXX need to detect any buffer which needs to be
       * split due to coalescing of a segment (front, middle,
       * data) boundary */

      take_len = MIN(blen, msg_iov->iov_len);
      payload.append(
	buffer::create_msg(
	  take_len, (char*) msg_iov->iov_base, m_hook));
      blen -= take_len;
      if (! blen) {
	left_len = msg_iov->iov_len - take_len;
	if (left_len) {
	  left_base = ((char*) msg_iov->iov_base) + take_len;
	}
      }
    }
    /* XXX as above, if a buffer is split, then we needed to track
     * the new start (carry) and not advance */
    if (ix == iov_len) {
      msg_seq.next(&msg_iter);
      ix = 0;
    }
  }

  if (magic & (MSG_MAGIC_TRACE_XCON)) {
    if (hdr.hdr->type == 43) {
      ldout(msgr->cct,4) << "front (payload) dump:";
      payload.hexdump( *_dout );
      *_dout << dendl;
    }
  }

  blen = header.middle_len;

  if (blen && left_len) {
    middle.append(
      buffer::create_msg(left_len, left_base, m_hook));
    left_len = 0;
  }

  while (blen && (msg_iter != msg_seq.end())) {
    treq = msg_iter;
    iov_len = vmsg_sglist_nents(&treq->in);
    iovs = vmsg_sglist(&treq->in);
    for (; blen && (ix < iov_len); ++ix) {
      msg_iov = &iovs[ix];
      take_len = MIN(blen, msg_iov->iov_len);
      middle.append(
	buffer::create_msg(
	  take_len, (char*) msg_iov->iov_base, m_hook));
      blen -= take_len;
      if (! blen) {
	left_len = msg_iov->iov_len - take_len;
	if (left_len) {
	  left_base = ((char*) msg_iov->iov_base) + take_len;
	}
      }
    }
    if (ix == iov_len) {
      msg_seq.next(&msg_iter);
      ix = 0;
    }
  }

  blen = header.data_len;

  if (blen && left_len) {
    data.append(
      buffer::create_msg(left_len, left_base, m_hook));
    left_len = 0;
  }

  while (blen && (msg_iter != msg_seq.end())) {
    treq = msg_iter;
    iov_len = vmsg_sglist_nents(&treq->in);
    iovs = vmsg_sglist(&treq->in);
    for (; blen && (ix < iov_len); ++ix) {
      msg_iov = &iovs[ix];
      data.append(
	buffer::create_msg(
	  msg_iov->iov_len, (char*) msg_iov->iov_base, m_hook));
      blen -= msg_iov->iov_len;
    }
    if (ix == iov_len) {
      msg_seq.next(&msg_iter);
      ix = 0;
    }
  }

  uint_to_timeval(t2, treq->timestamp);

  /* update connection timestamp */
  recv.set(treq->timestamp);

  Message *m =
    decode_message(msgr->cct, msgr->crcflags, header, footer, payload, middle,
		   data);

  if (m) {
    /* completion */
    m->set_connection(this);

    /* reply hook */
    m_hook->set_message(m);
    m->set_completion_hook(m_hook);

    /* trace flag */
    m->set_magic(magic);

    /* update timestamps */
    m->set_recv_stamp(t1);
    m->set_recv_complete_stamp(t2);
    m->set_seq(header.seq);

    /* MP-SAFE */
    state.set_in_seq(header.seq);

    /* XXXX validate peer type */
    if (peer_type != (int) hdr.peer_type) { /* XXX isn't peer_type -1? */
      peer_type = hdr.peer_type;
      peer_addr = hdr.addr;
      peer.addr = peer_addr;
      peer.name = hdr.hdr->src;
      if (xio_conn_type == XioConnection::PASSIVE) {
	/* XXX kick off feature/authn/authz negotiation
	 * nb:  very possibly the active side should initiate this, but
	 * for now, call a passive hook so OSD and friends can create
	 * sessions without actually negotiating
	 */
	passive_setup();
      }
    }

    if (magic & (MSG_MAGIC_TRACE_XCON)) {
      ldout(msgr->cct,4) << "decode m is " << m->get_type() << dendl;
    }

    /* dispatch it */
    msgr->ds_dispatch(m);
  } else {
    /* responds for undecoded messages and frees hook */
    ldout(msgr->cct,4) << "decode m failed" << dendl;
    m_hook->on_err_finalize(this);
  }

  return 0;
}

int XioConnection::on_ow_msg_send_complete(struct xio_session *session,
					   struct xio_msg *req,
					   void *conn_user_context)
{
  /* requester send complete (one-way) */
  uint64_t rc = ++scount;

  XioMsg* xmsg = static_cast<XioMsg*>(req->user_context);
  if (unlikely(magic & MSG_MAGIC_TRACE_CTR)) {
    if (unlikely((rc % 1000000) == 0)) {
      std::cout << "xio finished " << rc << " " << time(0) << std::endl;
    }
  } /* trace ctr */

  ldout(msgr->cct,11) << "on_msg_delivered xcon: " << xmsg->xcon <<
    " session: " << session << " msg: " << req << " sn: " << req->sn <<
    " type: " << xmsg->m->get_type() << " tid: " << xmsg->m->get_tid() <<
    " seq: " << xmsg->m->get_seq() << dendl;

  --send_ctr; /* atomic, because portal thread */

  /* unblock flow-controlled connections, avoid oscillation */
  if (unlikely(cstate.session_state.read() ==
	       XioConnection::FLOW_CONTROLLED)) {
    if ((send_ctr <= uint32_t(xio_qdepth_low_mark())) &&
	(1 /* XXX memory <= memory low-water mark */))  {
      cstate.state_up_ready(XioConnection::CState::OP_FLAG_NONE);
      ldout(msgr->cct,2) << "on_msg_delivered xcon: " << xmsg->xcon <<
        " session: " << session << " up_ready from flow_controlled" << dendl;
    }
  }

  xmsg->put();

  return 0;
}  /* on_msg_delivered */

void XioConnection::msg_send_fail(XioMsg *xmsg, int code)
{
  ldout(msgr->cct,2) << "xio_send_msg FAILED xcon: " << this <<
    " xmsg: " << &xmsg->req_0.msg << " code=" << code <<
    " (" << xio_strerror(code) << ")" << dendl;
  /* return refs taken for each xio_msg */
  xmsg->put_msg_refs();
} /* msg_send_fail */

void XioConnection::msg_release_fail(struct xio_msg *msg, int code)
{
  ldout(msgr->cct,2) << "xio_release_msg FAILED xcon: " << this <<
    " xmsg: " << msg <<  "code=" << code <<
    " (" << xio_strerror(code) << ")" << dendl;
} /* msg_release_fail */

int XioConnection::flush_input_queue(uint32_t flags) {
  XioMessenger* msgr = static_cast<XioMessenger*>(get_messenger());
  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_lock(&sp);

  // send deferred 1 (direct backpresssure)
  if (outgoing.requeue.size() > 0)
    portal->requeue(this, outgoing.requeue);

  // send deferred 2 (sent while deferred)
  int ix, q_size = outgoing.mqueue.size();
  for (ix = 0; ix < q_size; ++ix) {
    Message::Queue::iterator q_iter = outgoing.mqueue.begin();
    Message* m = &(*q_iter);
    outgoing.mqueue.erase(q_iter);
    msgr->_send_message_impl(m, this);
  }
  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&sp);
  return 0;
}

int XioConnection::discard_input_queue(uint32_t flags)
{
  Message::Queue disc_q;
  XioSubmit::Queue deferred_q;

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_lock(&sp);

  /* the two send queues contain different objects:
   * - anything on the mqueue is a Message
   * - anything on the requeue is an XioMsg
   */
  Message::Queue::const_iterator i1 = disc_q.end();
  disc_q.splice(i1, outgoing.mqueue);

  XioSubmit::Queue::const_iterator i2 = deferred_q.end();
  deferred_q.splice(i2, outgoing.requeue);

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&sp);

  // mqueue
  int ix, q_size =  disc_q.size();
  for (ix = 0; ix < q_size; ++ix) {
    Message::Queue::iterator q_iter = disc_q.begin();
    Message* m = &(*q_iter);
    disc_q.erase(q_iter);
    m->put();
  }

  // requeue
  q_size =  deferred_q.size();
  for (ix = 0; ix < q_size; ++ix) {
    XioSubmit::Queue::iterator q_iter = deferred_q.begin();
    XioSubmit* xs = &(*q_iter);
    assert(xs->type == XioSubmit::OUTGOING_MSG);
    XioMsg* xmsg = static_cast<XioMsg*>(xs);
    deferred_q.erase(q_iter);
    // release once for each chained xio_msg
    for (ix = 0; ix < int(xmsg->hdr.msg_cnt); ++ix)
      xmsg->put();
  }

  return 0;
}

int XioConnection::adjust_clru(uint32_t flags)
{
  if (flags & CState::OP_FLAG_LOCKED)
    pthread_spin_unlock(&sp);

  XioMessenger* msgr = static_cast<XioMessenger*>(get_messenger());
  msgr->conns_sp.lock();
  pthread_spin_lock(&sp);

  if (cstate.flags & CState::FLAG_MAPPED) {
    XioConnection::ConnList::iterator citer =
      XioConnection::ConnList::s_iterator_to(*this);
    msgr->conns_list.erase(citer);
    msgr->conns_list.push_front(*this); // LRU
  }

  msgr->conns_sp.unlock();

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&sp);

  return 0;
}

int XioConnection::on_msg_error(struct xio_session *session,
				enum xio_status error,
				struct xio_msg  *msg,
				void *conn_user_context)
{
  XioMsg *xmsg = static_cast<XioMsg*>(msg->user_context);
  if (xmsg)
    xmsg->put();

  --send_ctr; /* atomic, because portal thread */
  return 0;
} /* on_msg_error */

void XioConnection::mark_down()
{
  _mark_down(XioConnection::CState::OP_FLAG_NONE);
}

int XioConnection::_mark_down(uint32_t flags)
{
  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_lock(&sp);

  // per interface comment, we only stage a remote reset if the
  // current policy required it
  if (cstate.policy.resetcheck)
    cstate.flags |= CState::FLAG_RESET;

  // Accelio disconnect
  xio_disconnect(conn);

  /* XXX this will almost certainly be called again from
   * on_disconnect_event() */
  discard_input_queue(flags|CState::OP_FLAG_LOCKED);

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&sp);

  return 0;
}

void XioConnection::mark_disposable()
{
  _mark_disposable(XioConnection::CState::OP_FLAG_NONE);
}

int XioConnection::_mark_disposable(uint32_t flags)
{
  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_lock(&sp);

  cstate.policy.lossy = true;

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&sp);

  return 0;
}

int XioConnection::CState::state_up_ready(uint32_t flags)
{
  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_lock(&xcon->sp);

  xcon->flush_input_queue(flags|CState::OP_FLAG_LOCKED);

  session_state.set(UP);
  startup_state.set(READY);

  if (! (flags & CState::OP_FLAG_LOCKED))
    pthread_spin_unlock(&xcon->sp);

  return (0);
}

int XioConnection::CState::state_discon()
{
  session_state.set(DISCONNECTED);
  startup_state.set(IDLE);

  return 0;
}

int XioConnection::CState::state_flow_controlled(uint32_t flags) {
  dout(11) << __func__ << " ENTER " << dendl;

  if (! (flags & OP_FLAG_LOCKED))
    pthread_spin_lock(&xcon->sp);

  session_state.set(FLOW_CONTROLLED);

  if (! (flags & OP_FLAG_LOCKED))
    pthread_spin_unlock(&xcon->sp);

  return (0);
}

int XioConnection::CState::state_fail(Message* m, uint32_t flags)
{
  if (! (flags & OP_FLAG_LOCKED))
    pthread_spin_lock(&xcon->sp);

  // advance to state FAIL, drop queued, msgs, adjust LRU
  session_state.set(DISCONNECTED);
  startup_state.set(FAIL);

  xcon->discard_input_queue(flags|OP_FLAG_LOCKED);
  xcon->adjust_clru(flags|OP_FLAG_LOCKED|OP_FLAG_LRU);

  // Accelio disconnect
  xio_disconnect(xcon->conn);

  if (! (flags & OP_FLAG_LOCKED))
    pthread_spin_unlock(&xcon->sp);

  // notify ULP
  XioMessenger* msgr = static_cast<XioMessenger*>(xcon->get_messenger());
  msgr->ms_deliver_handle_reset(xcon);
  m->put();

  return 0;
}


int XioLoopbackConnection::send_message(Message *m)
{
  XioMessenger *ms = static_cast<XioMessenger*>(get_messenger());
  m->set_connection(this);
  m->set_seq(next_seq());
  m->set_src(ms->get_myinst().name);
  ms->ds_dispatch(m);
  return 0;
}
