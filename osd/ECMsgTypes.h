// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 Inktank Storage, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef ECBMSGTYPES_H
#define ECBMSGTYPES_H

#include "osd_types.h"
#include "include/buffer.h"
#include "os/ObjectStore.h"
#include "boost/tuple/tuple.hpp"

struct ECSubWrite {
  pg_shard_t from;
  ceph_tid_t tid;
  osd_reqid_t reqid;
  hobject_t soid;
  pg_stat_t stats;
  ObjectStore::Transaction t;
  eversion_t at_version;
  eversion_t trim_to;
  eversion_t trim_rollback_to;
  vector<pg_log_entry_t> log_entries;
  set<hobject_t> temp_added;
  set<hobject_t> temp_removed;
  boost::optional<pg_hit_set_history_t> updated_hit_set_history;
  ECSubWrite() {}
  ECSubWrite(
    pg_shard_t from,
    ceph_tid_t tid,
    osd_reqid_t reqid,
    hobject_t soid,
    const pg_stat_t &stats,
    const ObjectStore::Transaction &t,
    eversion_t at_version,
    eversion_t trim_to,
    eversion_t trim_rollback_to,
    vector<pg_log_entry_t> log_entries,
    boost::optional<pg_hit_set_history_t> updated_hit_set_history,
    const set<hobject_t> &temp_added,
    const set<hobject_t> &temp_removed)
    : from(from), tid(tid), reqid(reqid),
      soid(soid), stats(stats), t(t),
      at_version(at_version),
      trim_to(trim_to), trim_rollback_to(trim_rollback_to),
      log_entries(log_entries),
      temp_added(temp_added),
      temp_removed(temp_removed),
      updated_hit_set_history(updated_hit_set_history) {}
  void encode(bufferlist &bl) const;
  void decode(bufferlist::iterator &bl);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<ECSubWrite*>& o);
};
WRITE_CLASS_ENCODER(ECSubWrite)

struct ECSubWriteReply {
  pg_shard_t from;
  ceph_tid_t tid;
  eversion_t last_complete;
  bool committed;
  bool applied;
  ECSubWriteReply() : committed(false), applied(false) {}
  void encode(bufferlist &bl) const;
  void decode(bufferlist::iterator &bl);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<ECSubWriteReply*>& o);
};
WRITE_CLASS_ENCODER(ECSubWriteReply)

struct ECSubRead {
  pg_shard_t from;
  ceph_tid_t tid;
  map<hobject_t, list<boost::tuple<uint64_t, uint64_t, uint32_t> > > to_read;
  set<hobject_t> attrs_to_read;
  void encode(bufferlist &bl, uint64_t features) const;
  void decode(bufferlist::iterator &bl);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<ECSubRead*>& o);
};
WRITE_CLASS_ENCODER_FEATURES(ECSubRead)

struct ECSubReadReply {
  pg_shard_t from;
  ceph_tid_t tid;
  map<hobject_t, list<pair<uint64_t, bufferlist> > > buffers_read;
  map<hobject_t, map<string, bufferlist> > attrs_read;
  map<hobject_t, int> errors;
  void encode(bufferlist &bl) const;
  void decode(bufferlist::iterator &bl);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<ECSubReadReply*>& o);
};
WRITE_CLASS_ENCODER(ECSubReadReply)

std::ostream &operator<<(
  std::ostream &lhs, const ECSubWrite &rhs);
std::ostream &operator<<(
  std::ostream &lhs, const ECSubWriteReply &rhs);
std::ostream &operator<<(
  std::ostream &lhs, const ECSubRead &rhs);
std::ostream &operator<<(
  std::ostream &lhs, const ECSubReadReply &rhs);

#endif
