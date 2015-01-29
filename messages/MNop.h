// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Portions Copyright (C) 2014 CohortFS, LLC
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_MSG_NOP_H
#define CEPH_MSG_NOP_H

#include "msg/Message.h"
#include "msg/msg_types.h"

/*
 * A message with no (remote) effect.
 */
class MNop : public Message {
public:
  static const int HEAD_VERSION = 1;
  static const int COMPAT_VERSION = 1;

  __u32 tag; // ignored tag value

  MNop()
    : Message(MSG_NOP, HEAD_VERSION, COMPAT_VERSION)
    {}

  ~MNop() {}

  void encode_payload(uint64_t _features) {
    ::encode(tag, payload);
  }

  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    ::decode(tag, p);
  }

  const char *get_type_name() const { return "MNop"; }

  void print(ostream& out) const {
    out << get_type_name() << " ";
  }
}; /* MNop */

#endif /* CEPH_MSG_NOP_H */
