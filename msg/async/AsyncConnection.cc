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

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include "include/Context.h"
#include "common/errno.h"
#include "AsyncMessenger.h"
#include "AsyncConnection.h"

// Constant to limit starting sequence number to 2^31.  Nothing special about it, just a big number.  PLR
#define SEQ_MASK  0x7fffffff 

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix _conn_prefix(_dout)
ostream& AsyncConnection::_conn_prefix(std::ostream *_dout) {
  return *_dout << "-- " << async_msgr->get_myinst().addr << " >> " << peer_addr << " conn(" << this
                << " sd=" << sd << " :" << port
                << " s=" << get_state_name(state)
                << " pgs=" << peer_global_seq
                << " cs=" << connect_seq
                << " l=" << policy.lossy
                << ").";
}

const int AsyncConnection::TCP_PREFETCH_MIN_SIZE = 512;

class C_time_wakeup : public EventCallback {
  AsyncConnectionRef conn;

 public:
  C_time_wakeup(AsyncConnectionRef c): conn(c) {}
  void do_request(int fd_or_id) {
    conn->wakeup_from(fd_or_id);
  }
};

class C_handle_read : public EventCallback {
  AsyncConnectionRef conn;

 public:
  C_handle_read(AsyncConnectionRef c): conn(c) {}
  void do_request(int fd_or_id) {
    conn->process();
  }
};

class C_handle_write : public EventCallback {
  AsyncConnectionRef conn;

 public:
  C_handle_write(AsyncConnectionRef c): conn(c) {}
  void do_request(int fd) {
    conn->handle_write();
  }
};

class C_handle_reset : public EventCallback {
  AsyncMessenger *msgr;
  AsyncConnectionRef conn;

 public:
  C_handle_reset(AsyncMessenger *m, AsyncConnectionRef c): msgr(m), conn(c) {}
  void do_request(int id) {
    msgr->ms_deliver_handle_reset(conn.get());
  }
};

class C_handle_remote_reset : public EventCallback {
  AsyncMessenger *msgr;
  AsyncConnectionRef conn;

 public:
  C_handle_remote_reset(AsyncMessenger *m, AsyncConnectionRef c): msgr(m), conn(c) {}
  void do_request(int id) {
    msgr->ms_deliver_handle_remote_reset(conn.get());
  }
};

class C_handle_dispatch : public EventCallback {
  AsyncMessenger *msgr;
  Message *m;

 public:
  C_handle_dispatch(AsyncMessenger *msgr, Message *m): msgr(msgr), m(m) {}
  void do_request(int id) {
    //msgr->ms_fast_preprocess(m);
    //if (msgr->ms_can_fast_dispatch(m)) {
    //  msgr->ms_fast_dispatch(m);
    //} else {
      msgr->ms_deliver_dispatch(m);
    //}
  }
};

class C_deliver_connect : public EventCallback {
  AsyncMessenger *msgr;
  AsyncConnectionRef conn;

 public:
  C_deliver_connect(AsyncMessenger *msgr, AsyncConnectionRef c): msgr(msgr), conn(c) {}
  void do_request(int id) {
    msgr->ms_deliver_handle_connect(conn.get());
  }
};

class C_deliver_accept : public EventCallback {
  AsyncMessenger *msgr;
  AsyncConnectionRef conn;

 public:
  C_deliver_accept(AsyncMessenger *msgr, AsyncConnectionRef c): msgr(msgr), conn(c) {}
  void do_request(int id) {
    msgr->ms_deliver_handle_accept(conn.get());
  }
};

class C_local_deliver : public EventCallback {
  AsyncConnectionRef conn;
 public:
  C_local_deliver(AsyncConnectionRef c): conn(c) {}
  void do_request(int id) {
    conn->local_deliver();
  }
};


class C_clean_handler : public EventCallback {
  AsyncConnectionRef conn;
 public:
  C_clean_handler(AsyncConnectionRef c): conn(c) {}
  void do_request(int id) {
    conn->cleanup_handler();
  }
};

static void alloc_aligned_buffer(bufferlist& data, unsigned len, unsigned off)
{
  // create a buffer to read into that matches the data alignment
  unsigned left = len;
  if (off & ~CEPH_PAGE_MASK) {
    // head
    unsigned head = 0;
    head = MIN(CEPH_PAGE_SIZE - (off & ~CEPH_PAGE_MASK), left);
    bufferptr bp = buffer::create(head);
    data.push_back(bp);
    left -= head;
  }
  unsigned middle = left & CEPH_PAGE_MASK;
  if (middle > 0) {
    bufferptr bp = buffer::create_page_aligned(middle);
    data.push_back(bp);
    left -= middle;
  }
  if (left) {
    bufferptr bp = buffer::create(left);
    data.push_back(bp);
  }
}

AsyncConnection::AsyncConnection(CephContext *cct, AsyncMessenger *m, EventCenter *c)
  : Connection(cct, m), async_msgr(m), global_seq(0), connect_seq(0), peer_global_seq(0),
    out_seq(0), in_seq(0), in_seq_acked(0), state(STATE_NONE), state_after_send(0), sd(-1),
    port(-1), lock("AsyncConnection::lock"), open_write(false), keepalive(false), recv_buf(NULL),
    recv_max_prefetch(MIN(msgr->cct->_conf->ms_tcp_prefetch_max_size, TCP_PREFETCH_MIN_SIZE)),
    recv_start(0), recv_end(0), stop_lock("AsyncConnection::stop_lock"),
    got_bad_auth(false), authorizer(NULL), replacing(false), once_session_reset(false),
    state_buffer(NULL), state_offset(0), net(cct), center(c)
{
  read_handler.reset(new C_handle_read(this));
  write_handler.reset(new C_handle_write(this));
  reset_handler.reset(new C_handle_reset(async_msgr, this));
  remote_reset_handler.reset(new C_handle_remote_reset(async_msgr, this));
  connect_handler.reset(new C_deliver_connect(async_msgr, this));
  accept_handler.reset(new C_deliver_accept(async_msgr, this));
  local_deliver_handler.reset(new C_local_deliver(this));
  memset(msgvec, 0, sizeof(msgvec));
  // double recv_max_prefetch see "read_until"
  recv_buf = new char[2*recv_max_prefetch];
  state_buffer = new char[4096];
}

AsyncConnection::~AsyncConnection()
{
  assert(out_q.empty());
  assert(sent.empty());
  assert(!authorizer);
  if (recv_buf)
    delete[] recv_buf;
  if (state_buffer)
    delete[] state_buffer;
}

/* return -1 means `fd` occurs error or closed, it should be closed
 * return 0 means EAGAIN or EINTR */
int AsyncConnection::read_bulk(int fd, char *buf, int len)
{
  int nread = ::read(fd, buf, len);
  if (nread == -1) {
    if (errno == EAGAIN || errno == EINTR) {
      nread = 0;
    } else {
      ldout(async_msgr->cct, 1) << __func__ << " reading from fd=" << fd
                          << " : "<< strerror(errno) << dendl;
      return -1;
    }
  } else if (nread == 0) {
    ldout(async_msgr->cct, 1) << __func__ << " peer close file descriptor "
                              << fd << dendl;
    return -1;
  }
  return nread;
}

// return the length of msg needed to be sent,
// < 0 means error occured
int AsyncConnection::do_sendmsg(struct msghdr &msg, int len, bool more)
{
  while (len > 0) {
    int r = ::sendmsg(sd, &msg, MSG_NOSIGNAL | (more ? MSG_MORE : 0));

    if (r == 0) {
      ldout(async_msgr->cct, 10) << __func__ << " sendmsg got r==0!" << dendl;
    } else if (r < 0) {
      if (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN) {
        break;
      } else {
        ldout(async_msgr->cct, 1) << __func__ << " sendmsg error: " << cpp_strerror(errno) << dendl;
        return r;
      }
    }

    len -= r;
    if (len == 0) break;

    // hrmph. drain r bytes from the front of our message.
    ldout(async_msgr->cct, 20) << __func__ << " short write did " << r << ", still have " << len << dendl;
    while (r > 0) {
      if (msg.msg_iov[0].iov_len <= (size_t)r) {
        // drain this whole item
        r -= msg.msg_iov[0].iov_len;
        msg.msg_iov++;
        msg.msg_iovlen--;
      } else {
        msg.msg_iov[0].iov_base = (char *)msg.msg_iov[0].iov_base + r;
        msg.msg_iov[0].iov_len -= r;
        break;
      }
    }
  }
  return len;
}

// return the remaining bytes, it may larger than the length of ptr
// else return < 0 means error
int AsyncConnection::_try_send(bufferlist send_bl, bool send)
{
  if (send_bl.length()) {
    if (outcoming_bl.length())
      outcoming_bl.claim_append(send_bl);
    else
      outcoming_bl.swap(send_bl);
  }

  if (!send)
    return 0;

  // standby?
  if (is_queued() && state == STATE_STANDBY && !policy.server) {
    assert(!outcoming_bl.length());
    connect_seq++;
    state = STATE_CONNECTING;
    center->dispatch_event_external(read_handler);
    return 0;
  }

  if (state == STATE_STANDBY) {
    ldout(async_msgr->cct, 1) << __func__ << " connection is standby" << dendl;
    return 0;
  }
  if (state == STATE_CLOSED) {
    ldout(async_msgr->cct, 1) << __func__ << " connection is closed" << dendl;
    return -EINTR;
  }

  if (async_msgr->cct->_conf->ms_inject_socket_failures && sd >= 0) {
    if (rand() % async_msgr->cct->_conf->ms_inject_socket_failures == 0) {
      ldout(async_msgr->cct, 0) << __func__ << " injecting socket failure" << dendl;
      ::shutdown(sd, SHUT_RDWR);
    }
  }

  uint64_t sent = 0;
  list<bufferptr>::const_iterator pb = outcoming_bl.buffers().begin();
  uint64_t left_pbrs = outcoming_bl.buffers().size();
  while (left_pbrs) {
    struct msghdr msg;
    uint64_t size = MIN(left_pbrs, sizeof(msgvec));
    left_pbrs -= size;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iovlen = 0;
    msg.msg_iov = msgvec;
    int msglen = 0;
    while (size > 0) {
      msgvec[msg.msg_iovlen].iov_base = (void*)(pb->c_str());
      msgvec[msg.msg_iovlen].iov_len = pb->length();
      msg.msg_iovlen++;
      msglen += pb->length();
      ++pb;
      size--;
    }

    int r = do_sendmsg(msg, msglen, false);
    if (r < 0)
      return r;

    // "r" is the remaining length
    sent += msglen - r;
    if (r > 0) {
      ldout(async_msgr->cct, 5) << __func__ << " remaining " << r
                          << " needed to be sent, creating event for writing"
                          << dendl;
      break;
    }
    // only "r" == 0 continue
  }

  // trim already sent for outcoming_bl
  if (sent) {
    bufferlist bl;
    if (sent < outcoming_bl.length())
      outcoming_bl.splice(sent, outcoming_bl.length()-sent, &bl);
    bl.swap(outcoming_bl);
  }

  ldout(async_msgr->cct, 20) << __func__ << " sent bytes " << sent
                             << " remaining bytes " << outcoming_bl.length() << dendl;

  if (!open_write && is_queued()) {
    center->create_file_event(sd, EVENT_WRITABLE, write_handler);
    open_write = true;
  }

  if (open_write && !is_queued()) {
    center->delete_file_event(sd, EVENT_WRITABLE);
    open_write = false;
  }

  return outcoming_bl.length();
}

// Because this func will be called multi times to populate
// the needed buffer, so the passed in bufferptr must be the same.
// Normally, only "read_message" will pass existing bufferptr in
//
// And it will uses readahead method to reduce small read overhead,
// "recv_buf" is used to store read buffer
//
// return the remaining bytes, 0 means this buffer is finished
// else return < 0 means error
int AsyncConnection::read_until(uint64_t len, char *p)
{
  assert(len);
  ldout(async_msgr->cct, 20) << __func__ << " len is " << len << " state_offset is "
                             << state_offset << dendl;

  if (async_msgr->cct->_conf->ms_inject_socket_failures && sd >= 0) {
    if (rand() % async_msgr->cct->_conf->ms_inject_socket_failures == 0) {
      ldout(async_msgr->cct, 0) << __func__ << " injecting socket failure" << dendl;
      ::shutdown(sd, SHUT_RDWR);
    }
  }

  int r = 0;
  uint64_t left = len - state_offset;
  if (recv_end > recv_start) {
    assert(state_offset == 0);
    uint64_t to_read = MIN(recv_end - recv_start, left);
    memcpy(p, recv_buf+recv_start, to_read);
    recv_start += to_read;
    left -= to_read;
    ldout(async_msgr->cct, 20) << __func__ << " got " << to_read << " in buffer "
                               << " left is " << left << " buffer still has "
                               << recv_end - recv_start << dendl;
    if (left == 0) {
      return 0;
    }
    state_offset += to_read;
  }

  assert(recv_end == recv_start);
  recv_end = recv_start = 0;
  /* nothing left in the prefetch buffer */
  if (len > recv_max_prefetch) {
    /* this was a large read, we don't prefetch for these */
    do {
      r = read_bulk(sd, p+state_offset, left);
      ldout(async_msgr->cct, 20) << __func__ << " read_bulk left is " << left << " got " << r << dendl;
      if (r < 0) {
        ldout(async_msgr->cct, 1) << __func__ << " read failed, state is " << get_state_name(state) << dendl;
        return -1;
      } else if (r == static_cast<int>(left)) {
        state_offset = 0;
        return 0;
      }
      state_offset += r;
      left -= r;
    } while (r > 0);
  } else {
    do {
      r = read_bulk(sd, recv_buf+recv_end, recv_max_prefetch);
      ldout(async_msgr->cct, 20) << __func__ << " read_bulk recv_end is " << recv_end
                                 << " left is " << left << " got " << r << dendl;
      if (r < 0) {
        ldout(async_msgr->cct, 1) << __func__ << " read failed, state is " << get_state_name(state) << dendl;
        return -1;
      }
      recv_end += r;
      if (r >= static_cast<int>(left)) {
        recv_start = len - state_offset;
        memcpy(p+state_offset, recv_buf, recv_start);
        state_offset = 0;
        return 0;
      }
      left -= r;
    } while (r > 0);
    memcpy(p+state_offset, recv_buf, recv_end-recv_start);
    state_offset += (recv_end - recv_start);
    recv_end = recv_start = 0;
  }
  ldout(async_msgr->cct, 20) << __func__ << " need len " << len << " remaining "
                             << len - state_offset << " bytes, state is "
                             << get_state_name(state) << dendl;
  return len - state_offset;
}

void AsyncConnection::process()
{
  int r = 0;
  int prev_state = state;
  Mutex::Locker l(lock);
  do {
    ldout(async_msgr->cct, 20) << __func__ << " state is " << get_state_name(state)
                               << ", prev state is " << get_state_name(prev_state) << dendl;
    prev_state = state;
    switch (state) {
      case STATE_OPEN:
        {
          char tag = -1;
          r = read_until(sizeof(tag), &tag);
          if (r < 0) {
            ldout(async_msgr->cct, 1) << __func__ << " read tag failed, state is "
                                      << get_state_name(state) << dendl;
            goto fail;
          } else if (r > 0) {
            break;
          }

          if (tag == CEPH_MSGR_TAG_KEEPALIVE) {
            ldout(async_msgr->cct, 20) << __func__ << " got KEEPALIVE" << dendl;
          } else if (tag == CEPH_MSGR_TAG_KEEPALIVE2) {
            state = STATE_OPEN_KEEPALIVE2;
          } else if (tag == CEPH_MSGR_TAG_KEEPALIVE2_ACK) {
            state = STATE_OPEN_KEEPALIVE2_ACK;
          } else if (tag == CEPH_MSGR_TAG_ACK) {
            state = STATE_OPEN_TAG_ACK;
          } else if (tag == CEPH_MSGR_TAG_MSG) {
            state = STATE_OPEN_MESSAGE_HEADER;
          } else if (tag == CEPH_MSGR_TAG_CLOSE) {
            state = STATE_OPEN_TAG_CLOSE;
          } else {
            ldout(async_msgr->cct, 0) << __func__ << " bad tag " << (int)tag << dendl;
            goto fail;
          }

          break;
        }

      case STATE_OPEN_KEEPALIVE2:
        {
          ceph_timespec *t;
          r = read_until(sizeof(*t), state_buffer);
          if (r < 0) {
            ldout(async_msgr->cct, 1) << __func__ << " read keeplive timespec failed" << dendl;
            goto fail;
          } else if (r > 0) {
            break;
          }

          ldout(async_msgr->cct, 30) << __func__ << " got KEEPALIVE2 tag ..." << dendl;
          t = (ceph_timespec*)state_buffer;
          utime_t kp_t = utime_t(*t);
          _send_keepalive_or_ack(true, &kp_t);
          ldout(async_msgr->cct, 20) << __func__ << " got KEEPALIVE2 " << kp_t << dendl;
          state = STATE_OPEN;
          break;
        }

      case STATE_OPEN_KEEPALIVE2_ACK:
        {
          ceph_timespec *t;
          r = read_until(sizeof(*t), state_buffer);
          if (r < 0) {
            ldout(async_msgr->cct, 1) << __func__ << " read keeplive timespec failed" << dendl;
            goto fail;
          } else if (r > 0) {
            break;
          }

          t = (ceph_timespec*)state_buffer;
          last_keepalive_ack = utime_t(*t);
          ldout(async_msgr->cct, 20) << __func__ << " got KEEPALIVE_ACK" << dendl;
          state = STATE_OPEN;
          break;
        }

      case STATE_OPEN_TAG_ACK:
        {
          ceph_le64 *seq;
          r = read_until(sizeof(seq), state_buffer);
          if (r < 0) {
            ldout(async_msgr->cct, 1) << __func__ << " read ack seq failed" << dendl;
            goto fail;
          } else if (r > 0) {
            break;
          }

          seq = (ceph_le64*)state_buffer;
          ldout(async_msgr->cct, 20) << __func__ << " got ACK" << dendl;
          handle_ack(*seq);
          state = STATE_OPEN;
          break;
        }

      case STATE_OPEN_MESSAGE_HEADER:
        {
          ldout(async_msgr->cct, 20) << __func__ << " begin MSG" << dendl;
          ceph_msg_header header;
          ceph_msg_header_old oldheader;
          __u32 header_crc;
          int len;
          if (has_feature(CEPH_FEATURE_NOSRCADDR))
            len = sizeof(header);
          else
            len = sizeof(oldheader);

          r = read_until(len, state_buffer);
          if (r < 0) {
            ldout(async_msgr->cct, 1) << __func__ << " read message header failed" << dendl;
            goto fail;
          } else if (r > 0) {
            break;
          }

          ldout(async_msgr->cct, 20) << __func__ << " got MSG header" << dendl;

          if (has_feature(CEPH_FEATURE_NOSRCADDR)) {
            header = *((ceph_msg_header*)state_buffer);
            header_crc = ceph_crc32c(0, (unsigned char *)&header,
                                    sizeof(header) - sizeof(header.crc));
          } else {
            oldheader = *((ceph_msg_header_old*)state_buffer);
            // this is fugly
            memcpy(&header, &oldheader, sizeof(header));
            header.src = oldheader.src.name;
            header.reserved = oldheader.reserved;
            header.crc = oldheader.crc;
            header_crc = ceph_crc32c(0, (unsigned char *)&oldheader, sizeof(oldheader) - sizeof(oldheader.crc));
          }

          ldout(async_msgr->cct, 20) << __func__ << " got envelope type=" << header.type
                              << " src " << entity_name_t(header.src)
                              << " front=" << header.front_len
                              << " data=" << header.data_len
                              << " off " << header.data_off << dendl;

          // verify header crc
          if (header_crc != header.crc) {
            ldout(async_msgr->cct,0) << __func__ << " reader got bad header crc "
                              << header_crc << " != " << header.crc << dendl;
            goto fail;
          }

          // Reset state
          data_buf.clear();
          front.clear();
          middle.clear();
          data.clear();
          recv_stamp = ceph_clock_now(async_msgr->cct);
          current_header = header;
          state = STATE_OPEN_MESSAGE_THROTTLE_MESSAGE;
          break;
        }

      case STATE_OPEN_MESSAGE_THROTTLE_MESSAGE:
        {
          if (policy.throttler_messages) {
            ldout(async_msgr->cct,10) << __func__ << " wants " << 1 << " message from policy throttler "
                                << policy.throttler_messages->get_current() << "/"
                                << policy.throttler_messages->get_max() << dendl;
            // FIXME: may block
            policy.throttler_messages->get();
          }

          state = STATE_OPEN_MESSAGE_THROTTLE_BYTES;
          break;
        }

      case STATE_OPEN_MESSAGE_THROTTLE_BYTES:
        {
          uint64_t message_size = current_header.front_len + current_header.middle_len + current_header.data_len;
          if (message_size) {
            if (policy.throttler_bytes) {
              ldout(async_msgr->cct,10) << __func__ << " wants " << message_size << " bytes from policy throttler "
                  << policy.throttler_bytes->get_current() << "/"
                  << policy.throttler_bytes->get_max() << dendl;
              // FIXME: may block
              policy.throttler_bytes->get(message_size);
            }
          }

          throttle_stamp = ceph_clock_now(msgr->cct);
          state = STATE_OPEN_MESSAGE_READ_FRONT;
          break;
        }

      case STATE_OPEN_MESSAGE_READ_FRONT:
        {
          // read front
          int front_len = current_header.front_len;
          if (front_len) {
            if (!front.length()) {
              bufferptr ptr = buffer::create(front_len);
              front.push_back(ptr);
            }
            r = read_until(front_len, front.c_str());
            if (r < 0) {
              ldout(async_msgr->cct, 1) << __func__ << " read message front failed" << dendl;
              goto fail;
            } else if (r > 0) {
              break;
            }

            ldout(async_msgr->cct, 20) << __func__ << " got front " << front.length() << dendl;
          }
          state = STATE_OPEN_MESSAGE_READ_MIDDLE;
          break;
        }

      case STATE_OPEN_MESSAGE_READ_MIDDLE:
        {
          // read middle
          int middle_len = current_header.middle_len;
          if (middle_len) {
            if (!middle.length()) {
              bufferptr ptr = buffer::create(middle_len);
              middle.push_back(ptr);
            }
            r = read_until(middle_len, middle.c_str());
            if (r < 0) {
              ldout(async_msgr->cct, 1) << __func__ << " read message middle failed" << dendl;
              goto fail;
            } else if (r > 0) {
              break;
            }
            ldout(async_msgr->cct, 20) << __func__ << " got middle " << middle.length() << dendl;
          }

          state = STATE_OPEN_MESSAGE_READ_DATA_PREPARE;
          break;
        }

      case STATE_OPEN_MESSAGE_READ_DATA_PREPARE:
        {
          // read data
          uint64_t data_len = le32_to_cpu(current_header.data_len);
          int data_off = le32_to_cpu(current_header.data_off);
          if (data_len) {
            // get a buffer
            map<ceph_tid_t,pair<bufferlist,int> >::iterator p = rx_buffers.find(current_header.tid);
            if (p != rx_buffers.end()) {
              ldout(async_msgr->cct,10) << __func__ << " seleting rx buffer v " << p->second.second
                                  << " at offset " << data_off
                                  << " len " << p->second.first.length() << dendl;
              data_buf = p->second.first;
              // make sure it's big enough
              if (data_buf.length() < data_len)
                data_buf.push_back(buffer::create(data_len - data_buf.length()));
              data_blp = data_buf.begin();
            } else {
              ldout(async_msgr->cct,20) << __func__ << " allocating new rx buffer at offset " << data_off << dendl;
              alloc_aligned_buffer(data_buf, data_len, data_off);
              data_blp = data_buf.begin();
            }
          }

          msg_left = data_len;
          state = STATE_OPEN_MESSAGE_READ_DATA;
          break;
        }

      case STATE_OPEN_MESSAGE_READ_DATA:
        {
          while (msg_left > 0) {
            bufferptr bp = data_blp.get_current_ptr();
            uint64_t read = MIN(bp.length(), msg_left);
            r = read_until(read, bp.c_str());
            if (r < 0) {
              ldout(async_msgr->cct, 1) << __func__ << " read data error " << dendl;
              goto fail;
            } else if (r > 0) {
              break;
            }

            data_blp.advance(read);
            data.append(bp, 0, read);
            msg_left -= read;
          }

          if (msg_left == 0)
            state = STATE_OPEN_MESSAGE_READ_FOOTER_AND_DISPATCH;

          break;
        }

      case STATE_OPEN_MESSAGE_READ_FOOTER_AND_DISPATCH:
        {
          ceph_msg_footer footer;
          ceph_msg_footer_old old_footer;
          int len;
          // footer
          if (has_feature(CEPH_FEATURE_MSG_AUTH))
            len = sizeof(footer);
          else
            len = sizeof(old_footer);

          r = read_until(len, state_buffer);
          if (r < 0) {
            ldout(async_msgr->cct, 1) << __func__ << " read footer data error " << dendl;
            goto fail;
          } else if (r > 0) {
            break;
          }

          if (has_feature(CEPH_FEATURE_MSG_AUTH)) {
            footer = *((ceph_msg_footer*)state_buffer);
          } else {
            old_footer = *((ceph_msg_footer_old*)state_buffer);
            footer.front_crc = old_footer.front_crc;
            footer.middle_crc = old_footer.middle_crc;
            footer.data_crc = old_footer.data_crc;
            footer.sig = 0;
            footer.flags = old_footer.flags;
          }
          int aborted = (footer.flags & CEPH_MSG_FOOTER_COMPLETE) == 0;
          ldout(async_msgr->cct, 10) << __func__ << " aborted = " << aborted << dendl;
          if (aborted) {
            ldout(async_msgr->cct, 0) << __func__ << " got " << front.length() << " + " << middle.length() << " + " << data.length()
                                << " byte message.. ABORTED" << dendl;
            goto fail;
          }

          ldout(async_msgr->cct, 20) << __func__ << " got " << front.length() << " + " << middle.length()
                              << " + " << data.length() << " byte message" << dendl;
          Message *message = decode_message(async_msgr->cct, async_msgr->crcflags, current_header, footer, front, middle, data);
          if (!message) {
            ldout(async_msgr->cct, 1) << __func__ << " decode message failed " << dendl;
            goto fail;
          }

          //
          //  Check the signature if one should be present.  A zero return indicates success. PLR
          //

          if (session_security.get() == NULL) {
            ldout(async_msgr->cct, 10) << __func__ << " no session security set" << dendl;
          } else {
            if (session_security->check_message_signature(message)) {
              ldout(async_msgr->cct, 0) << __func__ << "Signature check failed" << dendl;
              message->put();
              goto fail;
            }
          }
          message->set_byte_throttler(policy.throttler_bytes);
          message->set_message_throttler(policy.throttler_messages);

          // store reservation size in message, so we don't get confused
          // by messages entering the dispatch queue through other paths.
          uint64_t message_size = current_header.front_len + current_header.middle_len + current_header.data_len;
          message->set_dispatch_throttle_size(message_size);

          message->set_recv_stamp(recv_stamp);
          message->set_throttle_stamp(throttle_stamp);
          message->set_recv_complete_stamp(ceph_clock_now(async_msgr->cct));

          // check received seq#.  if it is old, drop the message.  
          // note that incoming messages may skip ahead.  this is convenient for the client
          // side queueing because messages can't be renumbered, but the (kernel) client will
          // occasionally pull a message out of the sent queue to send elsewhere.  in that case
          // it doesn't matter if we "got" it or not.
          if (message->get_seq() <= in_seq) {
            ldout(async_msgr->cct,0) << __func__ << " got old message "
                    << message->get_seq() << " <= " << in_seq << " " << message << " " << *message
                    << ", discarding" << dendl;
            message->put();
            if (has_feature(CEPH_FEATURE_RECONNECT_SEQ) && async_msgr->cct->_conf->ms_die_on_old_message)
              assert(0 == "old msgs despite reconnect_seq feature");
            goto fail;
          }
          message->set_connection(this);

          // note last received message.
          in_seq = message->get_seq();
          ldout(async_msgr->cct, 10) << __func__ << " got message " << message->get_seq()
                               << " " << message << " " << *message << dendl;

          // if send_message always successfully send, it may have no
          // opportunity to send seq ack. 10 is a experience value.
          if (in_seq > in_seq_acked + 10) {
            center->dispatch_event_external(write_handler);
          }

          state = STATE_OPEN;

          async_msgr->ms_fast_preprocess(message);
          if (async_msgr->ms_can_fast_dispatch(message)) {
            lock.Unlock();
            async_msgr->ms_fast_dispatch(message);
            lock.Lock();
          } else {
            center->dispatch_event_external(EventCallbackRef(new C_handle_dispatch(async_msgr, message)));
          }

          break;
        }

      case STATE_OPEN_TAG_CLOSE:
        {
          ldout(async_msgr->cct, 20) << __func__ << " got CLOSE" << dendl;
          _stop();
          return ;
        }

      case STATE_STANDBY:
        {
          ldout(async_msgr->cct, 20) << __func__ << " enter STANDY" << dendl;

          break;
        }

      case STATE_CLOSED:
        {
          if (sd > 0)
            center->delete_file_event(sd, EVENT_READABLE);
          ldout(async_msgr->cct, 20) << __func__ << " socket closed" << dendl;
          break;
        }

      case STATE_WAIT:
        {
          ldout(async_msgr->cct, 20) << __func__ << " enter wait state" << dendl;
          break;
        }

      default:
        {
          if (_process_connection() < 0)
            goto fail;
          break;
        }
    }

    continue;

fail:
    // clean up state internal variables and states
    if (state >= STATE_CONNECTING_SEND_CONNECT_MSG &&
        state <= STATE_CONNECTING_READY) {
      delete authorizer;
      authorizer = NULL;
      got_bad_auth = false;
    }

    if (state > STATE_OPEN_MESSAGE_THROTTLE_MESSAGE &&
        state <= STATE_OPEN_MESSAGE_READ_FOOTER_AND_DISPATCH
        && policy.throttler_messages) {
      ldout(async_msgr->cct,10) << __func__ << " releasing " << 1
                          << " message to policy throttler "
                          << policy.throttler_messages->get_current() << "/"
                          << policy.throttler_messages->get_max() << dendl;
      policy.throttler_messages->put();
    }
    if (state > STATE_OPEN_MESSAGE_THROTTLE_BYTES &&
        state <= STATE_OPEN_MESSAGE_READ_FOOTER_AND_DISPATCH) {
      uint64_t message_size = current_header.front_len + current_header.middle_len + current_header.data_len;
      if (policy.throttler_bytes) {
        ldout(async_msgr->cct,10) << __func__ << " releasing " << message_size
                            << " bytes to policy throttler "
                            << policy.throttler_bytes->get_current() << "/"
                            << policy.throttler_bytes->get_max() << dendl;
        policy.throttler_bytes->put(message_size);
      }
    }
    fault();
  } while (prev_state != state);
}

int AsyncConnection::_process_connection()
{
  int r = 0;

  switch(state) {
    case STATE_WAIT_SEND:
      {
        if (!outcoming_bl.length()) {
          assert(state_after_send);
          state = state_after_send;
          state_after_send = 0;
        }
        break;
      }

    case STATE_CONNECTING:
      {
        assert(!policy.server);

        // reset connect state variables
        got_bad_auth = false;
        delete authorizer;
        authorizer = NULL;
        memset(&connect_msg, 0, sizeof(connect_msg));
        memset(&connect_reply, 0, sizeof(connect_reply));

        global_seq = async_msgr->get_global_seq();
        // close old socket.  this is safe because we stopped the reader thread above.
        if (sd >= 0) {
          center->delete_file_event(sd, EVENT_READABLE|EVENT_WRITABLE);
          ::close(sd);
        }

        sd = net.connect(get_peer_addr());
        if (sd < 0) {
          goto fail;
        }
        r = net.set_nonblock(sd);
        if (r < 0) {
          goto fail;
        }
        net.set_socket_options(sd);

        center->create_file_event(sd, EVENT_READABLE, read_handler);
        state = STATE_CONNECTING_WAIT_BANNER;
        break;
      }

    case STATE_CONNECTING_WAIT_BANNER:
      {
        r = read_until(strlen(CEPH_BANNER), state_buffer);
        if (r < 0) {
          ldout(async_msgr->cct, 1) << __func__ << " read banner failed" << dendl;
          goto fail;
        } else if (r > 0) {
          break;
        }

        if (memcmp(state_buffer, CEPH_BANNER, strlen(CEPH_BANNER))) {
          ldout(async_msgr->cct, 0) << __func__ << " connect protocol error (bad banner) on peer "
                              << get_peer_addr() << dendl;
          goto fail;
        }

        ldout(async_msgr->cct, 10) << __func__ << " get banner, ready to send banner" << dendl;

        bufferlist bl;
        bl.append(state_buffer, strlen(CEPH_BANNER));
        r = _try_send(bl);
        if (r == 0) {
          state = STATE_CONNECTING_WAIT_IDENTIFY_PEER;
          ldout(async_msgr->cct, 10) << __func__ << " connect write banner done: "
                               << get_peer_addr() << dendl;
        } else if (r > 0) {
          state = STATE_WAIT_SEND;
          state_after_send = STATE_CONNECTING_WAIT_IDENTIFY_PEER;
          ldout(async_msgr->cct, 10) << __func__ << " connect wait for write banner: "
                               << get_peer_addr() << dendl;
        } else {
          goto fail;
        }
        break;
      }

    case STATE_CONNECTING_WAIT_IDENTIFY_PEER:
      {
        entity_addr_t paddr, peer_addr_for_me;
        bufferlist myaddrbl;

        r = read_until(sizeof(paddr)*2, state_buffer);
        if (r < 0) {
          ldout(async_msgr->cct, 1) << __func__ << " read identify peeraddr failed" << dendl;
          goto fail;
        } else if (r > 0) {
          break;
        }

        bufferlist bl;
        bl.append(state_buffer, sizeof(paddr)*2);
        bufferlist::iterator p = bl.begin();
        try {
          ::decode(paddr, p);
          ::decode(peer_addr_for_me, p);
        } catch (const buffer::error& e) {
          lderr(async_msgr->cct) << __func__ <<  " decode peer addr failed " << dendl;
          goto fail;
        }
        ldout(async_msgr->cct, 20) << __func__ <<  " connect read peer addr "
                             << paddr << " on socket " << sd << dendl;
        if (peer_addr != paddr) {
          if (paddr.is_blank_ip() && peer_addr.get_port() == paddr.get_port() &&
              peer_addr.get_nonce() == paddr.get_nonce()) {
            ldout(async_msgr->cct, 0) << __func__ <<  " connect claims to be " << paddr
                                << " not " << peer_addr
                                << " - presumably this is the same node!" << dendl;
          } else {
            ldout(async_msgr->cct, 0) << __func__ << " connect claims to be "
                                << paddr << " not " << peer_addr << " - wrong node!" << dendl;
            goto fail;
          }
        }

        ldout(async_msgr->cct, 20) << __func__ << " connect peer addr for me is " << peer_addr_for_me << dendl;
        // TODO: it's tricky that exit loop if exist AsyncMessenger waiting for
        // mark_down. Otherwise, it will be deadlock while
        // AsyncMessenger::mark_down_all already hold lock.
        if (stopping.read())
          break;
        async_msgr->learned_addr(peer_addr_for_me);
        ::encode(async_msgr->get_myaddr(), myaddrbl);
        r = _try_send(myaddrbl);
        if (r == 0) {
          state = STATE_CONNECTING_SEND_CONNECT_MSG;
          ldout(async_msgr->cct, 10) << __func__ << " connect sent my addr "
              << async_msgr->get_myaddr() << dendl;
        } else if (r > 0) {
          state = STATE_WAIT_SEND;
          state_after_send = STATE_CONNECTING_SEND_CONNECT_MSG;
          ldout(async_msgr->cct, 10) << __func__ << " connect send my addr done: "
              << async_msgr->get_myaddr() << dendl;
        } else {
          ldout(async_msgr->cct, 2) << __func__ << " connect couldn't write my addr, "
              << cpp_strerror(errno) << dendl;
          goto fail;
        }

        break;
      }

    case STATE_CONNECTING_SEND_CONNECT_MSG:
      {
        if (!got_bad_auth) {
          delete authorizer;
          authorizer = async_msgr->get_authorizer(peer_type, false);
        }
        bufferlist bl;

        connect_msg.features = policy.features_supported;
        connect_msg.host_type = async_msgr->get_myinst().name.type();
        connect_msg.global_seq = global_seq;
        connect_msg.connect_seq = connect_seq;
        connect_msg.protocol_version = async_msgr->get_proto_version(peer_type, true);
        connect_msg.authorizer_protocol = authorizer ? authorizer->protocol : 0;
        connect_msg.authorizer_len = authorizer ? authorizer->bl.length() : 0;
        if (authorizer)
          ldout(async_msgr->cct, 10) << __func__ <<  "connect_msg.authorizer_len="
              << connect_msg.authorizer_len << " protocol="
              << connect_msg.authorizer_protocol << dendl;
        connect_msg.flags = 0;
        if (policy.lossy)
          connect_msg.flags |= CEPH_MSG_CONNECT_LOSSY;  // this is fyi, actually, server decides!
        bl.append((char*)&connect_msg, sizeof(connect_msg));
        if (authorizer) {
          bl.append(authorizer->bl.c_str(), authorizer->bl.length());
        }
        ldout(async_msgr->cct, 10) << __func__ << " connect sending gseq=" << global_seq << " cseq="
            << connect_seq << " proto=" << connect_msg.protocol_version << dendl;

        r = _try_send(bl);
        if (r == 0) {
          state = STATE_CONNECTING_WAIT_CONNECT_REPLY;
          ldout(async_msgr->cct,20) << __func__ << " connect wrote (self +) cseq, waiting for reply" << dendl;
        } else if (r > 0) {
          state = STATE_WAIT_SEND;
          state_after_send = STATE_CONNECTING_WAIT_CONNECT_REPLY;
          ldout(async_msgr->cct, 10) << __func__ << " continue send reply " << dendl;
        } else {
          ldout(async_msgr->cct, 2) << __func__ << " connect couldn't send reply "
              << cpp_strerror(errno) << dendl;
          goto fail;
        }

        break;
      }

    case STATE_CONNECTING_WAIT_CONNECT_REPLY:
      {
        r = read_until(sizeof(connect_reply), state_buffer);
        if (r < 0) {
          ldout(async_msgr->cct, 1) << __func__ << " read connect reply failed" << dendl;
          goto fail;
        } else if (r > 0) {
          break;
        }

        connect_reply = *((ceph_msg_connect_reply*)state_buffer);
        connect_reply.features = ceph_sanitize_features(connect_reply.features);

        ldout(async_msgr->cct, 20) << __func__ << " connect got reply tag " << (int)connect_reply.tag
                             << " connect_seq " << connect_reply.connect_seq << " global_seq "
                             << connect_reply.global_seq << " proto " << connect_reply.protocol_version
                             << " flags " << (int)connect_reply.flags << " features "
                             << connect_reply.features << dendl;
        state = STATE_CONNECTING_WAIT_CONNECT_REPLY_AUTH;

        break;
      }

    case STATE_CONNECTING_WAIT_CONNECT_REPLY_AUTH:
      {
        bufferlist authorizer_reply;
        if (connect_reply.authorizer_len) {
          ldout(async_msgr->cct, 10) << __func__ << " reply.authorizer_len=" << connect_reply.authorizer_len << dendl;
          assert(connect_reply.authorizer_len < 4096);
          r = read_until(connect_reply.authorizer_len, state_buffer);
          if (r < 0) {
            ldout(async_msgr->cct, 1) << __func__ << " read connect reply authorizer failed" << dendl;
            goto fail;
          } else if (r > 0) {
            break;
          }

          authorizer_reply.append(state_buffer, connect_reply.authorizer_len);
          bufferlist::iterator iter = authorizer_reply.begin();
          if (authorizer && !authorizer->verify_reply(iter)) {
            ldout(async_msgr->cct, 0) << __func__ << " failed verifying authorize reply" << dendl;
            goto fail;
          }
        }
        r = handle_connect_reply(connect_msg, connect_reply);
        if (r < 0)
          goto fail;

        // state must be changed!
        assert(state != STATE_CONNECTING_WAIT_CONNECT_REPLY_AUTH);
        break;
      }

    case STATE_CONNECTING_WAIT_ACK_SEQ:
      {
        uint64_t newly_acked_seq = 0;
        bufferlist bl;

        r = read_until(sizeof(newly_acked_seq), state_buffer);
        if (r < 0) {
          ldout(async_msgr->cct, 1) << __func__ << " read connect ack seq failed" << dendl;
          goto fail;
        } else if (r > 0) {
          break;
        }

        newly_acked_seq = *((uint64_t*)state_buffer);
        ldout(async_msgr->cct, 2) << __func__ << " got newly_acked_seq " << newly_acked_seq
                            << " vs out_seq " << out_seq << dendl;
        while (newly_acked_seq > out_seq) {
          Message *m = _get_next_outgoing();
          assert(m);
          ldout(async_msgr->cct, 2) << __func__ << " discarding previously sent " << m->get_seq()
                              << " " << *m << dendl;
          assert(m->get_seq() <= newly_acked_seq);
          m->put();
          ++out_seq;
        }

        bl.append((char*)&in_seq, sizeof(in_seq));
        r = _try_send(bl);
        if (r == 0) {
          state = STATE_CONNECTING_READY;
          ldout(async_msgr->cct, 10) << __func__ << " send in_seq done " << dendl;
        } else if (r > 0) {
          state_after_send = STATE_CONNECTING_READY;
          state = STATE_WAIT_SEND;
          ldout(async_msgr->cct, 10) << __func__ << " continue send in_seq " << dendl;
        } else {
          goto fail;
        }
        break;
      }

    case STATE_CONNECTING_READY:
      {
        // hooray!
        peer_global_seq = connect_reply.global_seq;
        policy.lossy = connect_reply.flags & CEPH_MSG_CONNECT_LOSSY;
        state = STATE_OPEN;
        connect_seq += 1;
        assert(connect_seq == connect_reply.connect_seq);
        backoff = utime_t();
        set_features((uint64_t)connect_reply.features & (uint64_t)connect_msg.features);
        ldout(async_msgr->cct, 10) << __func__ << " connect success " << connect_seq
                                   << ", lossy = " << policy.lossy << ", features "
                                   << get_features() << dendl;

        // If we have an authorizer, get a new AuthSessionHandler to deal with ongoing security of the
        // connection.  PLR
        if (authorizer != NULL) {
          session_security.reset(
              get_auth_session_handler(async_msgr->cct,
                                       authorizer->protocol,
                                       authorizer->session_key,
                                       get_features()));
        } else {
          // We have no authorizer, so we shouldn't be applying security to messages in this AsyncConnection.  PLR
          session_security.reset();
        }

        center->dispatch_event_external(connect_handler);
        async_msgr->ms_deliver_handle_fast_connect(this);

        // message may in queue between last _try_send and connection ready
        // write event may already notify and we need to force scheduler again
        if (is_queued())
          center->dispatch_event_external(write_handler);

        break;
      }

    case STATE_ACCEPTING:
      {
        bufferlist bl;

        if (net.set_nonblock(sd) < 0)
          goto fail;

        net.set_socket_options(sd);

        bl.append(CEPH_BANNER, strlen(CEPH_BANNER));

        ::encode(async_msgr->get_myaddr(), bl);
        port = async_msgr->get_myaddr().get_port();
        // and peer's socket addr (they might not know their ip)
        socklen_t len = sizeof(socket_addr.ss_addr());
        r = ::getpeername(sd, (sockaddr*)&socket_addr.ss_addr(), &len);
        if (r < 0) {
          ldout(async_msgr->cct, 0) << __func__ << " failed to getpeername "
                              << cpp_strerror(errno) << dendl;
          goto fail;
        }
        ::encode(socket_addr, bl);
        ldout(async_msgr->cct, 1) << __func__ << " sd=" << sd << " " << socket_addr << dendl;

        r = _try_send(bl);
        if (r == 0) {
          state = STATE_ACCEPTING_WAIT_BANNER_ADDR;
          ldout(async_msgr->cct, 10) << __func__ << " write banner and addr done: "
            << get_peer_addr() << dendl;
        } else if (r > 0) {
          state = STATE_WAIT_SEND;
          state_after_send = STATE_ACCEPTING_WAIT_BANNER_ADDR;
          ldout(async_msgr->cct, 10) << __func__ << " wait for write banner and addr: "
                              << get_peer_addr() << dendl;
        } else {
          goto fail;
        }

        break;
      }
    case STATE_ACCEPTING_WAIT_BANNER_ADDR:
      {
        bufferlist addr_bl;
        entity_addr_t peer_addr;

        r = read_until(strlen(CEPH_BANNER) + sizeof(peer_addr), state_buffer);
        if (r < 0) {
          ldout(async_msgr->cct, 1) << __func__ << " read peer banner and addr failed" << dendl;
          goto fail;
        } else if (r > 0) {
          break;
        }

        if (memcmp(state_buffer, CEPH_BANNER, strlen(CEPH_BANNER))) {
          ldout(async_msgr->cct, 1) << __func__ << " accept peer sent bad banner '" << state_buffer
                                    << "' (should be '" << CEPH_BANNER << "')" << dendl;
          goto fail;
        }

        addr_bl.append(state_buffer+strlen(CEPH_BANNER), sizeof(peer_addr));
        {
          bufferlist::iterator ti = addr_bl.begin();
          ::decode(peer_addr, ti);
        }

        ldout(async_msgr->cct, 10) << __func__ << " accept peer addr is " << peer_addr << dendl;
        if (peer_addr.is_blank_ip()) {
          // peer apparently doesn't know what ip they have; figure it out for them.
          int port = peer_addr.get_port();
          peer_addr.addr = socket_addr.addr;
          peer_addr.set_port(port);
          ldout(async_msgr->cct, 0) << __func__ << " accept peer addr is really " << peer_addr
                             << " (socket is " << socket_addr << ")" << dendl;
        }
        set_peer_addr(peer_addr);  // so that connection_state gets set up
        state = STATE_ACCEPTING_WAIT_CONNECT_MSG;
        break;
      }

    case STATE_ACCEPTING_WAIT_CONNECT_MSG:
      {
        r = read_until(sizeof(connect_msg), state_buffer);
        if (r < 0) {
          ldout(async_msgr->cct, 1) << __func__ << " read connect msg failed" << dendl;
          goto fail;
        } else if (r > 0) {
          break;
        }

        connect_msg = *((ceph_msg_connect*)state_buffer);
        // sanitize features
        connect_msg.features = ceph_sanitize_features(connect_msg.features);
        state = STATE_ACCEPTING_WAIT_CONNECT_MSG_AUTH;
        break;
      }

    case STATE_ACCEPTING_WAIT_CONNECT_MSG_AUTH:
      {
        bufferlist authorizer_bl, authorizer_reply;

        if (connect_msg.authorizer_len) {
          r = read_until(connect_msg.authorizer_len, state_buffer);
          if (r < 0) {
            ldout(async_msgr->cct, 1) << __func__ << " read connect msg failed" << dendl;
            goto fail;
          } else if (r > 0) {
            break;
          }
          authorizer_bl.append(state_buffer, connect_msg.authorizer_len);
        }

        ldout(async_msgr->cct, 20) << __func__ << " accept got peer connect_seq "
                             << connect_msg.connect_seq << " global_seq "
                             << connect_msg.global_seq << dendl;
        set_peer_type(connect_msg.host_type);
        policy = async_msgr->get_policy(connect_msg.host_type);
        ldout(async_msgr->cct, 10) << __func__ << " accept of host_type " << connect_msg.host_type
                                   << ", policy.lossy=" << policy.lossy << " policy.server="
                                   << policy.server << " policy.standby=" << policy.standby
                                   << " policy.resetcheck=" << policy.resetcheck << dendl;

        r = handle_connect_msg(connect_msg, authorizer_bl, authorizer_reply);
        if (r < 0)
          goto fail;

        // state is changed by "handle_connect_msg"
        assert(state != STATE_ACCEPTING_WAIT_CONNECT_MSG_AUTH);
        break;
      }

    case STATE_ACCEPTING_WAIT_SEQ:
      {
        uint64_t newly_acked_seq;
        r = read_until(sizeof(newly_acked_seq), state_buffer);
        if (r < 0) {
          ldout(async_msgr->cct, 1) << __func__ << " read ack seq failed" << dendl;
          goto fail;
        } else if (r > 0) {
          break;
        }

        newly_acked_seq = *((uint64_t*)state_buffer);
        ldout(async_msgr->cct, 2) << __func__ << " accept get newly_acked_seq " << newly_acked_seq << dendl;
        discard_requeued_up_to(newly_acked_seq);
        state = STATE_ACCEPTING_READY;
        break;
      }

    case STATE_ACCEPTING_READY:
      {
        ldout(async_msgr->cct, 20) << __func__ << " accept done" << dendl;
        state = STATE_OPEN;
        memset(&connect_msg, 0, sizeof(connect_msg));
        break;
      }

    default:
      {
        lderr(async_msgr->cct) << __func__ << " bad state: " << get_state_name(state) << dendl;
        assert(0);
      }
  }

  return 0;

fail:
  return -1;
}

int AsyncConnection::handle_connect_reply(ceph_msg_connect &connect, ceph_msg_connect_reply &reply)
{
  uint64_t feat_missing;
  if (reply.tag == CEPH_MSGR_TAG_FEATURES) {
    ldout(async_msgr->cct, 0) << __func__ << " connect protocol feature mismatch, my "
                        << std::hex << connect.features << " < peer "
                        << reply.features << " missing "
                        << (reply.features & ~policy.features_supported)
                        << std::dec << dendl;
    goto fail;
  }

  if (reply.tag == CEPH_MSGR_TAG_BADPROTOVER) {
    ldout(async_msgr->cct, 0) << __func__ << " connect protocol version mismatch, my "
                        << connect.protocol_version << " != " << reply.protocol_version
                        << dendl;
    goto fail;
  }

  if (reply.tag == CEPH_MSGR_TAG_BADAUTHORIZER) {
    ldout(async_msgr->cct,0) << __func__ << " connect got BADAUTHORIZER" << dendl;
    if (got_bad_auth)
      goto fail;
    got_bad_auth = true;
    delete authorizer;
    authorizer = async_msgr->get_authorizer(peer_type, true);  // try harder
    state = STATE_CONNECTING_SEND_CONNECT_MSG;
  }
  if (reply.tag == CEPH_MSGR_TAG_RESETSESSION) {
    ldout(async_msgr->cct, 0) << __func__ << "connect got RESETSESSION" << dendl;
    was_session_reset();
    state = STATE_CONNECTING_SEND_CONNECT_MSG;
  }
  if (reply.tag == CEPH_MSGR_TAG_RETRY_GLOBAL) {
    global_seq = async_msgr->get_global_seq(reply.global_seq);
    ldout(async_msgr->cct, 10) << __func__ << " connect got RETRY_GLOBAL "
                         << reply.global_seq << " chose new "
                         << global_seq << dendl;
    state = STATE_CONNECTING_SEND_CONNECT_MSG;
  }
  if (reply.tag == CEPH_MSGR_TAG_RETRY_SESSION) {
    assert(reply.connect_seq > connect_seq);
    connect_seq = reply.connect_seq;
    ldout(async_msgr->cct, 10) << __func__ << " connect got RETRY_SESSION "
                         << connect_seq << " -> "
                         << reply.connect_seq << dendl;
    state = STATE_CONNECTING_SEND_CONNECT_MSG;
  }
  if (reply.tag == CEPH_MSGR_TAG_WAIT) {
    ldout(async_msgr->cct, 3) << __func__ << " connect got WAIT (connection race)" << dendl;
    state = STATE_WAIT;
  }

  feat_missing = policy.features_required & ~(uint64_t)connect_reply.features;
  if (feat_missing) {
    ldout(async_msgr->cct, 1) << __func__ << " missing required features " << std::hex
                              << feat_missing << std::dec << dendl;
    goto fail;
  }

  if (reply.tag == CEPH_MSGR_TAG_SEQ) {
    ldout(async_msgr->cct, 10) << __func__ << " got CEPH_MSGR_TAG_SEQ, reading acked_seq and writing in_seq" << dendl;
    state = STATE_CONNECTING_WAIT_ACK_SEQ;
  }
  if (reply.tag == CEPH_MSGR_TAG_READY) {
    ldout(async_msgr->cct, 10) << __func__ << " got CEPH_MSGR_TAG_READY " << dendl;
    state = STATE_CONNECTING_READY;
  }

  return 0;

 fail:
  return -1;
}

int AsyncConnection::handle_connect_msg(ceph_msg_connect &connect, bufferlist &authorizer_bl,
                                        bufferlist &authorizer_reply)
{
  int r = 0;
  ceph_msg_connect_reply reply;
  bufferlist reply_bl;
  bool is_reset_from_peer = false;

  memset(&reply, 0, sizeof(reply));
  reply.protocol_version = async_msgr->get_proto_version(peer_type, false);

  // mismatch?
  ldout(async_msgr->cct, 10) << __func__ << " accept my proto " << reply.protocol_version
                      << ", their proto " << connect.protocol_version << dendl;
  if (connect.protocol_version != reply.protocol_version) {
    return _reply_accept(CEPH_MSGR_TAG_BADPROTOVER, connect, reply, authorizer_reply);
  }
  // require signatures for cephx?
  if (connect.authorizer_protocol == CEPH_AUTH_CEPHX) {
    if (peer_type == CEPH_ENTITY_TYPE_OSD ||
        peer_type == CEPH_ENTITY_TYPE_MDS) {
      if (async_msgr->cct->_conf->cephx_require_signatures ||
          async_msgr->cct->_conf->cephx_cluster_require_signatures) {
        ldout(async_msgr->cct, 10) << __func__ << " using cephx, requiring MSG_AUTH feature bit for cluster" << dendl;
        policy.features_required |= CEPH_FEATURE_MSG_AUTH;
      }
    } else {
      if (async_msgr->cct->_conf->cephx_require_signatures ||
          async_msgr->cct->_conf->cephx_service_require_signatures) {
        ldout(async_msgr->cct, 10) << __func__ << " using cephx, requiring MSG_AUTH feature bit for service" << dendl;
        policy.features_required |= CEPH_FEATURE_MSG_AUTH;
      }
    }
  }
  uint64_t feat_missing = policy.features_required & ~(uint64_t)connect.features;
  if (feat_missing) {
    ldout(async_msgr->cct, 1) << __func__ << " peer missing required features "
                        << std::hex << feat_missing << std::dec << dendl;
    return _reply_accept(CEPH_MSGR_TAG_FEATURES, connect, reply, authorizer_reply);
  }

  bool authorizer_valid;
  if (!async_msgr->verify_authorizer(this, peer_type, connect.authorizer_protocol, authorizer_bl,
                               authorizer_reply, authorizer_valid, session_key) || !authorizer_valid) {
    ldout(async_msgr->cct,0) << __func__ << ": got bad authorizer" << dendl;
    session_security.reset();
    return _reply_accept(CEPH_MSGR_TAG_BADAUTHORIZER, connect, reply, authorizer_reply);
  }

  // We've verified the authorizer for this AsyncConnection, so set up the session security structure.  PLR
  ldout(async_msgr->cct, 10) << __func__ << " accept setting up session_security." << dendl;

  // existing?
  lock.Unlock();
  AsyncConnectionRef existing = async_msgr->lookup_conn(peer_addr);

  if (async_msgr->cct->_conf->ms_inject_internal_delays) {
    ldout(msgr->cct, 10) << __func__ << " sleep for "
                         << async_msgr->cct->_conf->ms_inject_internal_delays << dendl;
    utime_t t;
    t.set_from_double(async_msgr->cct->_conf->ms_inject_internal_delays);
    t.sleep();
  }

  lock.Lock();
  if (state != STATE_ACCEPTING_WAIT_CONNECT_MSG_AUTH) {
    ldout(async_msgr->cct, 1) << __func__ << " state changed while accept, it must be mark_down, state="
                              << get_state_name(state) << dendl;
    assert(state == STATE_CLOSED);
    goto fail;
  }

  if (existing == this)
    existing = NULL;
  if (existing) {
    if (connect.global_seq < existing->peer_global_seq) {
      ldout(async_msgr->cct, 10) << __func__ << " accept existing " << existing
                           << ".gseq " << existing->peer_global_seq << " > "
                           << connect.global_seq << ", RETRY_GLOBAL" << dendl;
      reply.global_seq = existing->peer_global_seq;  // so we can send it below..
      return _reply_accept(CEPH_MSGR_TAG_RETRY_GLOBAL, connect, reply, authorizer_reply);
    } else {
      ldout(async_msgr->cct, 10) << __func__ << " accept existing " << existing
                           << ".gseq " << existing->peer_global_seq
                           << " <= " << connect.global_seq << ", looks ok" << dendl;
    }

    if (existing->policy.lossy) {
      ldout(async_msgr->cct, 0) << __func__ << " accept replacing existing (lossy) channel (new one lossy="
                          << policy.lossy << ")" << dendl;
      existing->was_session_reset();
      goto replace;
    }

    ldout(async_msgr->cct, 0) << __func__ << " accept connect_seq " << connect.connect_seq
                              << " vs existing csq=" << existing->connect_seq << " state="
                              << get_state_name(existing->state) << dendl;

    if (connect.connect_seq == 0 && existing->connect_seq > 0) {
      ldout(async_msgr->cct,0) << __func__ << " accept peer reset, then tried to connect to us, replacing" << dendl;
      // this is a hard reset from peer
      is_reset_from_peer = true;
      if (policy.resetcheck)
        existing->was_session_reset(); // this resets out_queue, msg_ and connect_seq #'s
      goto replace;
    }

    if (connect.connect_seq < existing->connect_seq) {
      // old attempt, or we sent READY but they didn't get it.
      ldout(async_msgr->cct, 10) << __func__ << " accept existing " << existing << ".cseq "
                           << existing->connect_seq << " > " << connect.connect_seq
                           << ", RETRY_SESSION" << dendl;
      reply.connect_seq = existing->connect_seq + 1;
      return _reply_accept(CEPH_MSGR_TAG_RETRY_SESSION, connect, reply, authorizer_reply);
    }

    if (connect.connect_seq == existing->connect_seq) {
      // if the existing connection successfully opened, and/or
      // subsequently went to standby, then the peer should bump
      // their connect_seq and retry: this is not a connection race
      // we need to resolve here.
      if (existing->state == STATE_OPEN ||
          existing->state == STATE_STANDBY) {
        ldout(async_msgr->cct, 10) << __func__ << " accept connection race, existing " << existing
                             << ".cseq " << existing->connect_seq << " == "
                             << connect.connect_seq << ", OPEN|STANDBY, RETRY_SESSION" << dendl;
        reply.connect_seq = existing->connect_seq + 1;
        return _reply_accept(CEPH_MSGR_TAG_RETRY_SESSION, connect, reply, authorizer_reply);
      }

      // connection race?
      if (peer_addr < async_msgr->get_myaddr() || existing->policy.server) {
        // incoming wins
        ldout(async_msgr->cct, 10) << __func__ << " accept connection race, existing " << existing
                             << ".cseq " << existing->connect_seq << " == " << connect.connect_seq
                             << ", or we are server, replacing my attempt" << dendl;
        goto replace;
      } else {
        // our existing outgoing wins
        ldout(async_msgr->cct,10) << __func__ << "accept connection race, existing "
                            << existing << ".cseq " << existing->connect_seq
                            << " == " << connect.connect_seq << ", sending WAIT" << dendl;
        assert(peer_addr > async_msgr->get_myaddr());
        // make sure our outgoing connection will follow through
        existing->send_keepalive();
        return _reply_accept(CEPH_MSGR_TAG_WAIT, connect, reply, authorizer_reply);
      }
    }

    assert(connect.connect_seq > existing->connect_seq);
    assert(connect.global_seq >= existing->peer_global_seq);
    if (policy.resetcheck &&   // RESETSESSION only used by servers; peers do not reset each other
        existing->connect_seq == 0 && once_session_reset) {
      ldout(async_msgr->cct, 0) << __func__ << " accept we reset (peer sent cseq "
                          << connect.connect_seq << ", " << existing << ".cseq = "
                          << existing->connect_seq << "), sending RESETSESSION" << dendl;
      once_session_reset = false;
      return _reply_accept(CEPH_MSGR_TAG_RESETSESSION, connect, reply, authorizer_reply);
    }

    // reconnect
    ldout(async_msgr->cct, 10) << __func__ << " accept peer sent cseq " << connect.connect_seq
                         << " > " << existing->connect_seq << dendl;
    goto replace;
  } // existing
  else if (!replacing && connect.connect_seq > 0) {
    // we reset, and they are opening a new session
    ldout(async_msgr->cct, 0) << __func__ << " accept we reset (peer sent cseq "
                        << connect.connect_seq << "), sending RESETSESSION" << dendl;
    return _reply_accept(CEPH_MSGR_TAG_RESETSESSION, connect, reply, authorizer_reply);
  } else {
    // new session
    ldout(async_msgr->cct, 10) << __func__ << " accept new session" << dendl;
    existing = NULL;
    goto open;
  }
  assert(0);

 replace:
  ldout(async_msgr->cct, 10) << __func__ << " accept replacing " << existing << dendl;

  if (async_msgr->cct->_conf->ms_inject_internal_delays) {
    ldout(msgr->cct, 10) << __func__ << " sleep for "
                         << async_msgr->cct->_conf->ms_inject_internal_delays << dendl;
    utime_t t;
    t.set_from_double(async_msgr->cct->_conf->ms_inject_internal_delays);
    t.sleep();
  }

  // There is no possible that existing connection will acquire this lock
  existing->lock.Lock();

  if (existing->replacing || existing->state == STATE_CLOSED) {
    ldout(async_msgr->cct, 1) << __func__ << " existing racing replace or mark_down happened while replacing."
                              << " state=" << get_state_name(existing->state) << dendl;
    reply.connect_seq = connect.connect_seq + 1;
    r = _reply_accept(CEPH_MSGR_TAG_RETRY_SESSION, connect, reply, authorizer_reply);
    existing->lock.Unlock();
    if (r < 0)
      goto fail;
    return 0;
  }

  if (existing->policy.lossy) {
    // disconnect from the Connection
    existing->center->dispatch_event_external(existing->reset_handler);
    existing->_stop();
  } else {
    // queue a reset on the new connection, which we're dumping for the old
    center->dispatch_event_external(reset_handler);

    // reset the in_seq if this is a hard reset from peer,
    // otherwise we respect our original connection's value
    if (is_reset_from_peer)
      existing->in_seq = 0;

    // Now existing connection will be alive and the current connection will
    // exchange socket with existing connection because we want to maintain
    // original "connection_state"
    existing->center->delete_file_event(existing->sd, EVENT_READABLE|EVENT_WRITABLE);
    center->delete_file_event(sd, EVENT_READABLE|EVENT_WRITABLE);
    existing->center->create_file_event(sd, EVENT_READABLE, existing->read_handler);

    reply.connect_seq = connect.connect_seq + 1;

    // Clean up output buffer
    existing->outcoming_bl.clear();
    existing->requeue_sent();

    swap(existing->sd, sd);
    existing->open_write = false;
    existing->replacing = true;
    existing->state_offset = 0;
    existing->state = STATE_ACCEPTING_WAIT_CONNECT_MSG;
    // there should exist any buffer
    assert(recv_start == recv_end);

    if (existing->_reply_accept(CEPH_MSGR_TAG_RETRY_SESSION, connect, reply, authorizer_reply) < 0) {
      // handle error
      existing->center->dispatch_event_external(existing->write_handler);
    }

    _stop();
    existing->lock.Unlock();
    return 0;
  }
  existing->lock.Unlock();

 open:
  connect_seq = connect.connect_seq + 1;
  peer_global_seq = connect.global_seq;
  ldout(async_msgr->cct, 10) << __func__ << " accept success, connect_seq = "
                             << connect_seq << " in_seq=" << in_seq << ", sending READY" << dendl;

  int next_state;

  // if it is a hard reset from peer(in_seq == 0), we don't need a round-trip to negotiate in/out sequence
  if ((connect.features & CEPH_FEATURE_RECONNECT_SEQ) && in_seq) {
    reply.tag = CEPH_MSGR_TAG_SEQ;
    next_state = STATE_ACCEPTING_WAIT_SEQ;
  } else {
    reply.tag = CEPH_MSGR_TAG_READY;
    next_state = STATE_ACCEPTING_READY;
    discard_requeued_up_to(0);
  }

  // send READY reply
  reply.features = policy.features_supported;
  reply.global_seq = async_msgr->get_global_seq();
  reply.connect_seq = connect_seq;
  reply.flags = 0;
  reply.authorizer_len = authorizer_reply.length();
  if (policy.lossy)
    reply.flags = reply.flags | CEPH_MSG_CONNECT_LOSSY;

  set_features((uint64_t)reply.features & (uint64_t)connect.features);
  ldout(async_msgr->cct, 10) << __func__ << " accept features " << get_features() << dendl;

  session_security.reset(
      get_auth_session_handler(async_msgr->cct, connect.authorizer_protocol,
                               session_key, get_features()));

  reply_bl.append((char*)&reply, sizeof(reply));

  if (reply.authorizer_len)
    reply_bl.append(authorizer_reply.c_str(), authorizer_reply.length());

  if (reply.tag == CEPH_MSGR_TAG_SEQ)
    reply_bl.append((char*)&in_seq, sizeof(in_seq));

  lock.Unlock();
  // Because "replacing" will prevent other connections preempt this addr,
  // it's safe that here we don't acquire Connection's lock
  r = async_msgr->accept_conn(this);

  if (async_msgr->cct->_conf->ms_inject_internal_delays) {
    ldout(msgr->cct, 10) << __func__ << " sleep for "
                         << async_msgr->cct->_conf->ms_inject_internal_delays << dendl;
    utime_t t;
    t.set_from_double(async_msgr->cct->_conf->ms_inject_internal_delays);
    t.sleep();
  }

  lock.Lock();
  replacing = false;
  if (r < 0) {
    ldout(async_msgr->cct, 1) << __func__ << " existing race replacing process for addr=" << peer_addr
                              << " just fail later one(this)" << dendl;
    goto fail_registered;
  }
  if (state != STATE_ACCEPTING_WAIT_CONNECT_MSG_AUTH) {
    ldout(async_msgr->cct, 1) << __func__ << " state changed while accept_conn, it must be mark_down, state="
                              << get_state_name(state) << dendl;
    assert(state == STATE_CLOSED);
    goto fail_registered;
  }

  // notify
  center->dispatch_event_external(accept_handler);
  async_msgr->ms_deliver_handle_fast_accept(this);


  r = _try_send(reply_bl);
  if (r < 0)
    goto fail_registered;

  if (r == 0) {
    state = next_state;
    ldout(async_msgr->cct, 2) << __func__ << " accept write reply msg done" << dendl;
  } else {
    state = STATE_WAIT_SEND;
    state_after_send = next_state;
  }

  return 0;

 fail_registered:
  ldout(async_msgr->cct, 10) << __func__ << " accept fault after register" << dendl;

  if (async_msgr->cct->_conf->ms_inject_internal_delays) {
    ldout(async_msgr->cct, 10) << __func__ << " sleep for "
                               << async_msgr->cct->_conf->ms_inject_internal_delays
                               << dendl;
    utime_t t;
    t.set_from_double(async_msgr->cct->_conf->ms_inject_internal_delays);
    t.sleep();
  }

 fail:
  ldout(async_msgr->cct, 10) << __func__ << " failed to accept." << dendl;
  return -1;
}

void AsyncConnection::_connect()
{
  ldout(async_msgr->cct, 10) << __func__ << " csq=" << connect_seq << dendl;

  state = STATE_CONNECTING;
  stopping.set(0);
  // rescheduler connection in order to avoid lock dep
  // may called by external thread(send_message)
  center->dispatch_event_external(read_handler);
}

void AsyncConnection::accept(int incoming)
{
  ldout(async_msgr->cct, 10) << __func__ << " sd=" << incoming << dendl;
  assert(sd < 0);

  sd = incoming;
  state = STATE_ACCEPTING;
  center->create_file_event(sd, EVENT_READABLE, read_handler);
  // rescheduler connection in order to avoid lock dep
  process();
}

int AsyncConnection::send_message(Message *m)
{
  ldout(async_msgr->cct, 10) << __func__ << dendl;
  m->get_header().src = async_msgr->get_myname();
  if (!m->get_priority())
    m->set_priority(async_msgr->get_default_send_priority());

  Mutex::Locker l(lock);
  if (!is_queued() && state >= STATE_OPEN && state <= STATE_OPEN_TAG_CLOSE) {
    ldout(async_msgr->cct, 10) << __func__ << " try send msg " << m << dendl;
    int r = _send(m);
    if (r < 0) {
      ldout(async_msgr->cct, 1) << __func__ << " send msg failed" << dendl;
      // we want to handle fault within internal thread
      center->dispatch_event_external(write_handler);
    }
  } else if (state == STATE_CLOSED) {
      ldout(async_msgr->cct, 10) << __func__ << " connection closed."
                                 << " Drop message " << m << dendl;
  } else if (async_msgr->get_myaddr() == get_peer_addr()) { //loopback connection
      ldout(async_msgr->cct, 20) << __func__ << " " << *m << " local" << dendl;
      local_messages.push_back(m);
      center->dispatch_event_external(local_deliver_handler);
  } else {
    out_q[m->get_priority()].push_back(m);
    if (state == STATE_STANDBY && !policy.server) {
      ldout(async_msgr->cct, 10) << __func__ << " state is " << get_state_name(state)
                                 << " policy.server is false" << dendl;
      _connect();
    } else if (sd > 0 && !open_write) {
      center->dispatch_event_external(write_handler);
    }
  }
  return 0;
}

void AsyncConnection::requeue_sent()
{
  if (sent.empty())
    return;

  list<Message*>& rq = out_q[CEPH_MSG_PRIO_HIGHEST];
  while (!sent.empty()) {
    Message *m = sent.back();
    sent.pop_back();
    ldout(async_msgr->cct, 10) << __func__ << " " << *m << " for resend seq " << out_seq
                         << " (" << m->get_seq() << ")" << dendl;
    rq.push_front(m);
    out_seq--;
  }
}

void AsyncConnection::discard_requeued_up_to(uint64_t seq)
{
  ldout(async_msgr->cct, 10) << __func__ << " " << seq << dendl;
  if (out_q.count(CEPH_MSG_PRIO_HIGHEST) == 0)
    return;
  list<Message*>& rq = out_q[CEPH_MSG_PRIO_HIGHEST];
  while (!rq.empty()) {
    Message *m = rq.front();
    if (m->get_seq() == 0 || m->get_seq() > seq)
      break;
    ldout(async_msgr->cct, 10) << __func__ << " " << *m << " for resend seq " << out_seq
                         << " <= " << seq << ", discarding" << dendl;
    m->put();
    rq.pop_front();
    out_seq++;
  }
  if (rq.empty())
    out_q.erase(CEPH_MSG_PRIO_HIGHEST);
}

/*
 * Tears down the AsyncConnection's message queues, and removes them from the DispatchQueue
 * Must hold pipe_lock prior to calling.
 */
void AsyncConnection::discard_out_queue()
{
  ldout(async_msgr->cct, 10) << __func__ << " started" << dendl;

  for (list<Message*>::iterator p = sent.begin(); p != sent.end(); ++p) {
    ldout(async_msgr->cct, 20) << __func__ << " discard " << *p << dendl;
    (*p)->put();
  }
  sent.clear();
  for (map<int,list<Message*> >::iterator p = out_q.begin(); p != out_q.end(); ++p)
    for (list<Message*>::iterator r = p->second.begin(); r != p->second.end(); ++r) {
      ldout(async_msgr->cct, 20) << __func__ << " discard " << *r << dendl;
      (*r)->put();
    }
  out_q.clear();
  outcoming_bl.clear();
}

int AsyncConnection::randomize_out_seq()
{
  if (get_features() & CEPH_FEATURE_MSG_AUTH) {
    // Set out_seq to a random value, so CRC won't be predictable.   Don't bother checking seq_error
    // here.  We'll check it on the call.  PLR
    int seq_error = get_random_bytes((char *)&out_seq, sizeof(out_seq));
    out_seq &= SEQ_MASK;
    lsubdout(async_msgr->cct, ms, 10) << __func__ << " randomize_out_seq " << out_seq << dendl;
    return seq_error;
  } else {
    // previously, seq #'s always started at 0.
    out_seq = 0;
    return 0;
  }
}

void AsyncConnection::fault()
{
  if (state == STATE_CLOSED) {
    ldout(async_msgr->cct, 10) << __func__ << " state is already " << get_state_name(state) << dendl;
    center->dispatch_event_external(reset_handler);
    return ;
  }

  if (policy.lossy && !(state >= STATE_CONNECTING && state < STATE_CONNECTING_READY)) {
    ldout(async_msgr->cct, 10) << __func__ << " on lossy channel, failing" << dendl;
    center->dispatch_event_external(reset_handler);
    _stop();
    return ;
  }

  if (sd >= 0) {
    shutdown_socket();
    center->delete_file_event(sd, EVENT_READABLE|EVENT_WRITABLE);
  }
  open_write = false;

  // requeue sent items
  requeue_sent();
  recv_start = recv_end = 0;
  state_offset = 0;
  replacing = false;
  outcoming_bl.clear();
  if (policy.standby && !is_queued()) {
    ldout(async_msgr->cct,0) << __func__ << " with nothing to send, going to standby" << dendl;
    state = STATE_STANDBY;
    return;
  }

  if (!(state >= STATE_CONNECTING && state < STATE_CONNECTING_READY)) {
    // policy maybe empty when state is in accept
    if (policy.server || (state >= STATE_ACCEPTING && state < STATE_ACCEPTING_WAIT_SEQ)) {
      ldout(async_msgr->cct, 0) << __func__ << " server, going to standby" << dendl;
      state = STATE_STANDBY;
    } else {
      ldout(async_msgr->cct, 0) << __func__ << " initiating reconnect" << dendl;
      connect_seq++;
      state = STATE_CONNECTING;
    }
    backoff = utime_t();
  } else {
    if (backoff == utime_t()) {
      backoff.set_from_double(async_msgr->cct->_conf->ms_initial_backoff);
    } else {
      backoff += backoff;
      if (backoff > async_msgr->cct->_conf->ms_max_backoff)
        backoff.set_from_double(async_msgr->cct->_conf->ms_max_backoff);
    }
    state = STATE_CONNECTING;
    ldout(async_msgr->cct, 10) << __func__ << " waiting " << backoff << dendl;
  }

  // woke up again;
  register_time_events.insert(center->create_time_event(
          backoff.to_nsec()/1000, EventCallbackRef(new C_time_wakeup(this))));
}

void AsyncConnection::was_session_reset()
{
  ldout(async_msgr->cct,10) << __func__ << " started" << dendl;
  discard_out_queue();

  center->dispatch_event_external(remote_reset_handler);

  if (randomize_out_seq()) {
    lsubdout(async_msgr->cct,ms,15) << __func__ << " could not get random bytes to set seq number for session reset; set seq number to " << out_seq << dendl;
  }

  in_seq = 0;
  connect_seq = 0;
  in_seq_acked = 0;
  once_session_reset = true;
}

void AsyncConnection::_stop()
{
  assert(lock.is_locked());
  ldout(async_msgr->cct, 10) << __func__ << dendl;
  if (sd > 0)
    center->delete_file_event(sd, EVENT_READABLE|EVENT_WRITABLE);

  discard_out_queue();
  async_msgr->unregister_conn(this);

  if (async_msgr->cct->_conf->ms_inject_internal_delays) {
    ldout(msgr->cct, 10) << __func__ << " sleep for "
                         << async_msgr->cct->_conf->ms_inject_internal_delays
                         << dendl;
    utime_t t;
    t.set_from_double(async_msgr->cct->_conf->ms_inject_internal_delays);
    t.sleep();
  }

  state = STATE_CLOSED;
  shutdown_socket();
  open_write = false;
  state_offset = 0;
  if (sd > 0)
    ::close(sd);
  sd = -1;
  for (set<uint64_t>::iterator it = register_time_events.begin();
       it != register_time_events.end(); ++it)
    center->delete_time_event(*it);
  // Make sure in-queue events will been processed
  center->dispatch_event_external(EventCallbackRef(new C_clean_handler(this)));
}

int AsyncConnection::_send(Message *m)
{
  m->set_seq(++out_seq);
  if (!policy.lossy) {
    // put on sent list
    sent.push_back(m); 
    m->get();
  }

  // associate message with Connection (for benefit of encode_payload)
  m->set_connection(this);

  uint64_t features = get_features();
  if (m->empty_payload())
    ldout(async_msgr->cct, 20) << __func__ << " encoding " << m->get_seq() << " features " << features
                         << " " << m << " " << *m << dendl;
  else
    ldout(async_msgr->cct, 20) << __func__ << " half-reencoding " << m->get_seq() << " features "
                         << features << " " << m << " " << *m << dendl;

  // encode and copy out of *m
  m->encode(features, async_msgr->crcflags);

  // prepare everything
  ceph_msg_header& header = m->get_header();
  ceph_msg_footer& footer = m->get_footer();

  // Now that we have all the crcs calculated, handle the
  // digital signature for the message, if the AsyncConnection has session
  // security set up.  Some session security options do not
  // actually calculate and check the signature, but they should
  // handle the calls to sign_message and check_signature.  PLR
  if (session_security.get() == NULL) {
    ldout(async_msgr->cct, 20) << __func__ << " no session security" << dendl;
  } else {
    if (session_security->sign_message(m)) {
      ldout(async_msgr->cct, 20) << __func__ << " failed to sign seq # "
                           << header.seq << "): sig = " << footer.sig << dendl;
    } else {
      ldout(async_msgr->cct, 20) << __func__ << " signed seq # " << header.seq
                           << "): sig = " << footer.sig << dendl;
    }
  }

  bufferlist blist = m->get_payload();
  blist.append(m->get_middle());
  blist.append(m->get_data());

  ldout(async_msgr->cct, 20) << __func__ << " sending " << m->get_seq()
                       << " " << m << dendl;
  int rc = write_message(header, footer, blist);

  if (rc < 0) {
    ldout(async_msgr->cct, 1) << __func__ << " error sending " << m << ", "
                        << cpp_strerror(errno) << dendl;
  } else if (rc == 0) {
    ldout(async_msgr->cct, 10) << __func__ << " sending " << m << " done." << dendl;
  } else {
    ldout(async_msgr->cct, 10) << __func__ << " sending " << m << " continuely." << dendl;
  }
  m->put();

  return rc;
}

int AsyncConnection::write_message(ceph_msg_header& header, ceph_msg_footer& footer,
                                  bufferlist& blist)
{
  bufferlist bl;
  int ret;

  // send tag
  char tag = CEPH_MSGR_TAG_MSG;
  bl.append(&tag, sizeof(tag));

  // send envelope
  ceph_msg_header_old oldheader;
  if (has_feature(CEPH_FEATURE_NOSRCADDR)) {
    bl.append((char*)&header, sizeof(header));
  } else {
    memcpy(&oldheader, &header, sizeof(header));
    oldheader.src.name = header.src;
    oldheader.src.addr = get_peer_addr();
    oldheader.orig_src = oldheader.src;
    oldheader.reserved = header.reserved;
    oldheader.crc = ceph_crc32c(0, (unsigned char*)&oldheader,
                                sizeof(oldheader) - sizeof(oldheader.crc));
    bl.append((char*)&oldheader, sizeof(oldheader));
  }

  bl.claim_append(blist);

  // send footer; if receiver doesn't support signatures, use the old footer format
  ceph_msg_footer_old old_footer;
  if (has_feature(CEPH_FEATURE_MSG_AUTH)) {
    bl.append((char*)&footer, sizeof(footer));
  } else {
    old_footer.front_crc = footer.front_crc;
    old_footer.middle_crc = footer.middle_crc;
    old_footer.data_crc = footer.data_crc;
    old_footer.flags = footer.flags;
    bl.append((char*)&old_footer, sizeof(old_footer));
  }

  // send
  ret = _try_send(bl);
  if (ret < 0)
    return ret;

  return ret;
}

void AsyncConnection::handle_ack(uint64_t seq)
{
  lsubdout(async_msgr->cct, ms, 15) << __func__ << " got ack seq " << seq << dendl;
  // trim sent list
  while (!sent.empty() && sent.front()->get_seq() <= seq) {
    Message *m = sent.front();
    sent.pop_front();
    lsubdout(async_msgr->cct, ms, 10) << __func__ << "reader got ack seq "
                                << seq << " >= " << m->get_seq() << " on "
                                << m << " " << *m << dendl;
    m->put();
  }
}

void AsyncConnection::send_keepalive()
{
  ldout(async_msgr->cct, 10) << __func__ << " started." << dendl;
  Mutex::Locker l(lock);
  if (state != STATE_CLOSED) {
    keepalive = true;
    center->dispatch_event_external(write_handler);
  }
}

void AsyncConnection::mark_down()
{
  ldout(async_msgr->cct, 10) << __func__ << " started." << dendl;
  stopping.set(1);
  Mutex::Locker l(lock);
  _stop();
}

void AsyncConnection::_send_keepalive_or_ack(bool ack, utime_t *tp)
{
  assert(lock.is_locked());
  bufferlist bl;

  utime_t t = ceph_clock_now(async_msgr->cct);
  struct ceph_timespec ts;
  t.encode_timeval(&ts);
  if (ack) {
    assert(tp);
    tp->encode_timeval(&ts);
    bl.append(CEPH_MSGR_TAG_KEEPALIVE2_ACK);
    bl.append((char*)&ts, sizeof(ts));
  } else if (has_feature(CEPH_FEATURE_MSGR_KEEPALIVE2)) {
    struct ceph_timespec ts;
    t.encode_timeval(&ts);
    bl.append(CEPH_MSGR_TAG_KEEPALIVE2);
    bl.append((char*)&ts, sizeof(ts));
  } else {
    bl.append(CEPH_MSGR_TAG_KEEPALIVE);
  }

  ldout(async_msgr->cct, 10) << __func__ << " try send keepalive or ack" << dendl;
  _try_send(bl, false);
}

void AsyncConnection::handle_write()
{
  ldout(async_msgr->cct, 10) << __func__ << " started." << dendl;
  Mutex::Locker l(lock);
  bufferlist bl;
  int r = 0;
  if (state >= STATE_OPEN && state <= STATE_OPEN_TAG_CLOSE) {
    if (keepalive) {
      _send_keepalive_or_ack();
      keepalive = false;
    }

    while (1) {
      Message *m = _get_next_outgoing();
      if (!m)
        break;

      ldout(async_msgr->cct, 10) << __func__ << " try send msg " << m << dendl;
      r = _send(m);
      if (r < 0) {
        ldout(async_msgr->cct, 1) << __func__ << " send msg failed" << dendl;
        goto fail;
      } else if (r > 0) {
        break;
      }
    }

    if (in_seq > in_seq_acked) {
      ceph_le64 s;
      s = in_seq;
      bl.append(CEPH_MSGR_TAG_ACK);
      bl.append((char*)&s, sizeof(s));
      ldout(async_msgr->cct, 10) << __func__ << " try send msg ack" << dendl;
      in_seq_acked = s;
      r = _try_send(bl);
    } else if (is_queued()) {
      r = _try_send(bl);
    }

    if (r < 0) {
      ldout(async_msgr->cct, 1) << __func__ << " send msg failed" << dendl;
      goto fail;
    }
  } else if (state != STATE_CONNECTING) {
    r = _try_send(bl);
    if (r < 0) {
      ldout(async_msgr->cct, 1) << __func__ << " send outcoming bl failed" << dendl;
      goto fail;
    }
  }

  return ;
 fail:
  fault();
}

void AsyncConnection::wakeup_from(uint64_t id)
{
  lock.Lock();
  register_time_events.erase(id);
  lock.Unlock();
  process();
}

void AsyncConnection::local_deliver()
{
  ldout(async_msgr->cct, 10) << __func__ << dendl;
  Mutex::Locker l(lock);
  while (!local_messages.empty()) {
    Message *m = local_messages.back();
    local_messages.pop_back();
    m->set_connection(this);
    m->set_recv_stamp(ceph_clock_now(async_msgr->cct));
    ldout(async_msgr->cct, 10) << __func__ << " " << *m << " local deliver " << dendl;
    async_msgr->ms_fast_preprocess(m);
    lock.Unlock();
    if (async_msgr->ms_can_fast_dispatch(m)) {
      async_msgr->ms_fast_dispatch(m);
    } else {
      msgr->ms_deliver_dispatch(m);
    }
    lock.Lock();
  }
}
