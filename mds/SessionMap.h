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

#ifndef CEPH_MDS_SESSIONMAP_H
#define CEPH_MDS_SESSIONMAP_H

#include <set>
using std::set;

#include "include/unordered_map.h"

#include "include/Context.h"
#include "include/xlist.h"
#include "include/elist.h"
#include "include/interval_set.h"
#include "mdstypes.h"
#include "mds/MDSAuthCaps.h"

class CInode;
struct MDRequestImpl;

#include "CInode.h"
#include "Capability.h"
#include "msg/Message.h"


/* 
 * session
 */

class Session : public RefCountedObject {
  // -- state etc --
public:
  /*
                    
        <deleted> <-- closed <------------+
             ^         |                  |
             |         v                  |
          killing <-- opening <----+      |
             ^         |           |      |
             |         v           |      |
           stale <--> open --> closing ---+

    + additional dimension of 'importing' (with counter)

  */
  enum {
    STATE_CLOSED = 0,
    STATE_OPENING = 1,   // journaling open
    STATE_OPEN = 2,
    STATE_CLOSING = 3,   // journaling close
    STATE_STALE = 4,
    STATE_KILLING = 5
  };

  const char *get_state_name(int s) {
    switch (s) {
    case STATE_CLOSED: return "closed";
    case STATE_OPENING: return "opening";
    case STATE_OPEN: return "open";
    case STATE_CLOSING: return "closing";
    case STATE_STALE: return "stale";
    case STATE_KILLING: return "killing";
    default: return "???";
    }
  }

private:
  int state;
  uint64_t state_seq;
  int importing_count;
  friend class SessionMap;

  // Human (friendly) name is soft state generated from client metadata
  void _update_human_name();
  std::string human_name;

public:

  inline int get_state() const {return state;}
  void set_state(int new_state)
  {
    if (state != new_state) {
      state = new_state;
      state_seq++;
    }
  }
  void decode(bufferlist::iterator &p);
  void set_client_metadata(std::map<std::string, std::string> const &meta);
  std::string get_human_name() const {return human_name;}

  // Ephemeral state for tracking progress of capability recalls
  utime_t recalled_at;  // When was I asked to SESSION_RECALL?
  uint32_t recall_count;  // How many caps was I asked to SESSION_RECALL?
  uint32_t recall_release_count;  // How many caps have I actually revoked?

  session_info_t info;                         ///< durable bits

  MDSAuthCaps auth_caps;

  ConnectionRef connection;
  xlist<Session*>::item item_session_list;

  list<Message*> preopen_out_queue;  ///< messages for client, queued before they connect

  elist<MDRequestImpl*> requests;
  size_t get_request_count();

  interval_set<inodeno_t> pending_prealloc_inos; // journaling prealloc, will be added to prealloc_inos

  void notify_cap_release(size_t n_caps);
  void notify_recall_sent(int const new_limit);

  inodeno_t next_ino() {
    if (info.prealloc_inos.empty())
      return 0;
    return info.prealloc_inos.range_start();
  }
  inodeno_t take_ino(inodeno_t ino = 0) {
    assert(!info.prealloc_inos.empty());

    if (ino) {
      if (info.prealloc_inos.contains(ino))
	info.prealloc_inos.erase(ino);
      else
	ino = 0;
    }
    if (!ino) {
      ino = info.prealloc_inos.range_start();
      info.prealloc_inos.erase(ino);
    }
    info.used_inos.insert(ino, 1);
    return ino;
  }
  int get_num_projected_prealloc_inos() {
    return info.prealloc_inos.size() + pending_prealloc_inos.size();
  }

  client_t get_client() {
    return info.get_client();
  }

  int get_state() { return state; }
  const char *get_state_name() { return get_state_name(state); }
  uint64_t get_state_seq() { return state_seq; }
  bool is_closed() { return state == STATE_CLOSED; }
  bool is_opening() { return state == STATE_OPENING; }
  bool is_open() { return state == STATE_OPEN; }
  bool is_closing() { return state == STATE_CLOSING; }
  bool is_stale() { return state == STATE_STALE; }
  bool is_killing() { return state == STATE_KILLING; }

  void inc_importing() {
    ++importing_count;
  }
  void dec_importing() {
    assert(importing_count);
    --importing_count;
  }
  bool is_importing() { return importing_count > 0; }

  // -- caps --
private:
  version_t cap_push_seq;        // cap push seq #
  map<version_t, list<MDSInternalContextBase*> > waitfor_flush; // flush session messages
public:
  xlist<Capability*> caps;     // inodes with caps; front=most recently used
  xlist<ClientLease*> leases;  // metadata leases to clients
  utime_t last_cap_renew;

public:
  version_t inc_push_seq() { return ++cap_push_seq; }
  version_t get_push_seq() const { return cap_push_seq; }

  version_t wait_for_flush(MDSInternalContextBase* c) {
    waitfor_flush[get_push_seq()].push_back(c);
    return get_push_seq();
  }
  void finish_flush(version_t seq, list<MDSInternalContextBase*>& ls) {
    while (!waitfor_flush.empty()) {
      if (waitfor_flush.begin()->first > seq)
	break;
      ls.splice(ls.end(), waitfor_flush.begin()->second);
      waitfor_flush.erase(waitfor_flush.begin());
    }
  }

  void add_cap(Capability *cap) {
    caps.push_back(&cap->item_session_caps);
  }
  void touch_lease(ClientLease *r) {
    leases.push_back(&r->item_session_lease);
  }

  // -- leases --
  uint32_t lease_seq;

  // -- completed requests --
private:


public:
  void add_completed_request(ceph_tid_t t, inodeno_t created) {
    info.completed_requests[t] = created;
  }
  void trim_completed_requests(ceph_tid_t mintid) {
    // trim
    while (!info.completed_requests.empty() && 
	   (mintid == 0 || info.completed_requests.begin()->first < mintid))
      info.completed_requests.erase(info.completed_requests.begin());
  }
  bool have_completed_request(ceph_tid_t tid, inodeno_t *pcreated) const {
    map<ceph_tid_t,inodeno_t>::const_iterator p = info.completed_requests.find(tid);
    if (p == info.completed_requests.end())
      return false;
    if (pcreated)
      *pcreated = p->second;
    return true;
  }


  Session() : 
    state(STATE_CLOSED), state_seq(0), importing_count(0),
    recalled_at(), recall_count(0), recall_release_count(0),
    connection(NULL), item_session_list(this),
    requests(0),  // member_offset passed to front() manually
    cap_push_seq(0),
    lease_seq(0) { }
  ~Session() {
    assert(!item_session_list.is_on_list());
    while (!preopen_out_queue.empty()) {
      preopen_out_queue.front()->put();
      preopen_out_queue.pop_front();
    }
  }

  void clear() {
    pending_prealloc_inos.clear();
    info.clear_meta();

    cap_push_seq = 0;
    last_cap_renew = utime_t();

  }

};

/*
 * session map
 */

class MDS;

/**
 * Encapsulate the serialized state associated with SessionMap.  Allows
 * encode/decode outside of live MDS instance.
 */
class SessionMapStore {
public:
  ceph::unordered_map<entity_name_t, Session*> session_map;
  version_t version;
  mds_rank_t rank;

  virtual void encode(bufferlist& bl) const;
  virtual void decode(bufferlist::iterator& blp);
  void dump(Formatter *f) const;

  void set_rank(mds_rank_t r)
  {
    rank = r;
  }

  Session* get_or_add_session(const entity_inst_t& i) {
    Session *s;
    if (session_map.count(i.name)) {
      s = session_map[i.name];
    } else {
      s = session_map[i.name] = new Session;
      s->info.inst = i;
      s->last_cap_renew = ceph_clock_now(g_ceph_context);
    }

    return s;
  }

  static void generate_test_instances(list<SessionMapStore*>& ls);

  void reset_state()
  {
    session_map.clear();
  }

  SessionMapStore() : version(0), rank(MDS_RANK_NONE) {}
  virtual ~SessionMapStore() {};
};

class SessionMap : public SessionMapStore {
public:
  MDS *mds;

public:  // i am lazy
  version_t projected, committing, committed;
  map<int,xlist<Session*>* > by_state;
  uint64_t set_state(Session *session, int state);
  map<version_t, list<MDSInternalContextBase*> > commit_waiters;

  SessionMap(MDS *m) : mds(m),
		       projected(0), committing(0), committed(0) 
  { }

  // sessions
  void decode(bufferlist::iterator& blp);
  bool empty() { return session_map.empty(); }
  const ceph::unordered_map<entity_name_t, Session*> &get_sessions() const
  {
    return session_map;
  }

  bool is_any_state(int state) {
    map<int,xlist<Session*>* >::iterator p = by_state.find(state);
    if (p == by_state.end() || p->second->empty())
      return false;
    return true;
  }

  bool have_unclosed_sessions() {
    return
      is_any_state(Session::STATE_OPENING) ||
      is_any_state(Session::STATE_OPENING) ||
      is_any_state(Session::STATE_OPEN) ||
      is_any_state(Session::STATE_CLOSING) ||
      is_any_state(Session::STATE_STALE) ||
      is_any_state(Session::STATE_KILLING);
  }
  bool have_session(entity_name_t w) {
    return session_map.count(w);
  }
  Session* get_session(entity_name_t w) {
    if (session_map.count(w))
      return session_map[w];
    return 0;
  }
  const Session* get_session(entity_name_t w) const {
    ceph::unordered_map<entity_name_t, Session*>::const_iterator p = session_map.find(w);
    if (p == session_map.end()) {
      return NULL;
    } else {
      return p->second;
    }
  }

  void add_session(Session *s);
  void remove_session(Session *s);
  void touch_session(Session *session);

  Session *get_oldest_session(int state) {
    if (by_state.count(state) == 0 || by_state[state]->empty())
      return 0;
    return by_state[state]->front();
  }

  void dump();

  void get_client_set(set<client_t>& s) {
    for (ceph::unordered_map<entity_name_t,Session*>::iterator p = session_map.begin();
	 p != session_map.end();
	 ++p)
      if (p->second->info.inst.name.is_client())
	s.insert(p->second->info.inst.name.num());
  }
  void get_client_session_set(set<Session*>& s) const {
    for (ceph::unordered_map<entity_name_t,Session*>::const_iterator p = session_map.begin();
	 p != session_map.end();
	 ++p)
      if (p->second->info.inst.name.is_client())
	s.insert(p->second);
  }

  void open_sessions(map<client_t,entity_inst_t>& client_map) {
    for (map<client_t,entity_inst_t>::iterator p = client_map.begin(); 
	 p != client_map.end(); 
	 ++p) {
      Session *s = get_or_add_session(p->second);
      set_state(s, Session::STATE_OPEN);
    }
    version++;
  }

  // helpers
  entity_inst_t& get_inst(entity_name_t w) {
    assert(session_map.count(w));
    return session_map[w]->info.inst;
  }
  version_t inc_push_seq(client_t client) {
    return get_session(entity_name_t::CLIENT(client.v))->inc_push_seq();
  }
  version_t get_push_seq(client_t client) {
    return get_session(entity_name_t::CLIENT(client.v))->get_push_seq();
  }
  bool have_completed_request(metareqid_t rid) {
    Session *session = get_session(rid.name);
    return session && session->have_completed_request(rid.tid, NULL);
  }
  void trim_completed_requests(entity_name_t c, ceph_tid_t tid) {
    Session *session = get_session(c);
    assert(session);
    session->trim_completed_requests(tid);
  }

  void wipe();
  void wipe_ino_prealloc();

  // -- loading, saving --
  inodeno_t ino;
  list<MDSInternalContextBase*> waiting_for_load;

  object_t get_object_name();

  void load(MDSInternalContextBase *onload);
  void _load_finish(int r, bufferlist &bl);
  void save(MDSInternalContextBase *onsave, version_t needv=0);
  void _save_finish(version_t v);
 
};


#endif
