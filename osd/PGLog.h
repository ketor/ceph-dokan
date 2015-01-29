// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Copyright (C) 2013 Cloudwatt <libre.licensing@cloudwatt.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */
#ifndef CEPH_PG_LOG_H
#define CEPH_PG_LOG_H

// re-include our assert to clobber boost's
#include "include/assert.h" 
#include "osd_types.h"
#include "os/ObjectStore.h"
#include "common/ceph_context.h"
#include <list>
using namespace std;

struct PGLog {
  ////////////////////////////// sub classes //////////////////////////////
  struct LogEntryHandler {
    virtual void rollback(
      const pg_log_entry_t &entry) = 0;
    virtual void remove(
      const hobject_t &hoid) = 0;
    virtual void trim(
      const pg_log_entry_t &entry) = 0;
    virtual ~LogEntryHandler() {}
  };

  /* Exceptions */
  class read_log_error : public buffer::error {
  public:
    explicit read_log_error(const char *what) {
      snprintf(buf, sizeof(buf), "read_log_error: %s", what);
    }
    const char *what() const throw () {
      return buf;
    }
  private:
    char buf[512];
  };

  /**
   * IndexLog - adds in-memory index of the log, by oid.
   * plus some methods to manipulate it all.
   */
  struct IndexedLog : public pg_log_t {
    ceph::unordered_map<hobject_t,pg_log_entry_t*> objects;  // ptrs into log.  be careful!
    ceph::unordered_map<osd_reqid_t,pg_log_entry_t*> caller_ops;

    // recovery pointers
    list<pg_log_entry_t>::iterator complete_to;  // not inclusive of referenced item
    version_t last_requested;           // last object requested by primary

    //
  private:
    /**
     * rollback_info_trimmed_to_riter points to the first log entry <=
     * rollback_info_trimmed_to
     *
     * It's a reverse_iterator because rend() is a natural representation for
     * tail, and rbegin() works nicely for head.
     */
    list<pg_log_entry_t>::reverse_iterator rollback_info_trimmed_to_riter;
  public:
    void advance_rollback_info_trimmed_to(eversion_t to, LogEntryHandler *h);

    /****/
    IndexedLog() :
      complete_to(log.end()),
      last_requested(0),
      rollback_info_trimmed_to_riter(log.rbegin())
      {}

    void claim_log_and_clear_rollback_info(const pg_log_t& o) {
      // we must have already trimmed the old entries
      assert(rollback_info_trimmed_to == head);
      assert(rollback_info_trimmed_to_riter == log.rbegin());

      log = o.log;
      head = o.head;
      rollback_info_trimmed_to = head;
      tail = o.tail;
      index();
    }

    void split_into(
      pg_t child_pgid,
      unsigned split_bits,
      IndexedLog *olog);

    void zero() {
      // we must have already trimmed the old entries
      assert(rollback_info_trimmed_to == head);
      assert(rollback_info_trimmed_to_riter == log.rbegin());

      unindex();
      pg_log_t::clear();
      rollback_info_trimmed_to_riter = log.rbegin();
      reset_recovery_pointers();
    }
    void clear() {
      rollback_info_trimmed_to = head;
      rollback_info_trimmed_to_riter = log.rbegin();
      zero();
    }
    void reset_recovery_pointers() {
      complete_to = log.end();
      last_requested = 0;
    }

    bool logged_object(const hobject_t& oid) const {
      return objects.count(oid);
    }
    bool logged_req(const osd_reqid_t &r) const {
      return caller_ops.count(r);
    }
    const pg_log_entry_t *get_request(const osd_reqid_t &r) const {
      ceph::unordered_map<osd_reqid_t,pg_log_entry_t*>::const_iterator p = caller_ops.find(r);
      if (p == caller_ops.end())
	return NULL;
      return p->second;
    }

    void index() {
      objects.clear();
      caller_ops.clear();
      for (list<pg_log_entry_t>::iterator i = log.begin();
           i != log.end();
           ++i) {
        objects[i->soid] = &(*i);
	if (i->reqid_is_indexed()) {
	  //assert(caller_ops.count(i->reqid) == 0);  // divergent merge_log indexes new before unindexing old
	  caller_ops[i->reqid] = &(*i);
	}
      }

      rollback_info_trimmed_to_riter = log.rbegin();
      while (rollback_info_trimmed_to_riter != log.rend() &&
	     rollback_info_trimmed_to_riter->version > rollback_info_trimmed_to)
	++rollback_info_trimmed_to_riter;
    }

    void index(pg_log_entry_t& e) {
      if (objects.count(e.soid) == 0 || 
          objects[e.soid]->version < e.version)
        objects[e.soid] = &e;
      if (e.reqid_is_indexed()) {
	//assert(caller_ops.count(i->reqid) == 0);  // divergent merge_log indexes new before unindexing old
	caller_ops[e.reqid] = &e;
      }
    }
    void unindex() {
      objects.clear();
      caller_ops.clear();
    }
    void unindex(pg_log_entry_t& e) {
      // NOTE: this only works if we remove from the _tail_ of the log!
      if (objects.count(e.soid) && objects[e.soid]->version == e.version)
        objects.erase(e.soid);
      if (e.reqid_is_indexed() &&
	  caller_ops.count(e.reqid) &&  // divergent merge_log indexes new before unindexing old
	  caller_ops[e.reqid] == &e)
	caller_ops.erase(e.reqid);
    }

    // actors
    void add(pg_log_entry_t& e) {
      // add to log
      log.push_back(e);

      // riter previously pointed to the previous entry
      if (rollback_info_trimmed_to_riter == log.rbegin())
	++rollback_info_trimmed_to_riter;

      assert(e.version > head);
      assert(head.version == 0 || e.version.version > head.version);
      head = e.version;

      // to our index
      objects[e.soid] = &(log.back());
      if (e.reqid_is_indexed())
	caller_ops[e.reqid] = &(log.back());
    }

    void trim(
      LogEntryHandler *handler,
      eversion_t s,
      set<eversion_t> *trimmed);

    ostream& print(ostream& out) const;
  };


protected:
  //////////////////// data members ////////////////////
  bool pg_log_debug;

  map<eversion_t, hobject_t> divergent_priors;
  pg_missing_t     missing;
  IndexedLog  log;

  /// Log is clean on [dirty_to, dirty_from)
  bool touched_log;
  eversion_t dirty_to;         ///< must clear/writeout all keys up to dirty_to
  eversion_t dirty_from;       ///< must clear/writeout all keys past dirty_from
  eversion_t writeout_from;    ///< must writout keys past writeout_from
  set<eversion_t> trimmed;     ///< must clear keys in trimmed
  bool dirty_divergent_priors;
  CephContext *cct;

  bool is_dirty() const {
    return !touched_log ||
      (dirty_to != eversion_t()) ||
      (dirty_from != eversion_t::max()) ||
      dirty_divergent_priors ||
      (writeout_from != eversion_t::max()) ||
      !(trimmed.empty());
  }
  void mark_dirty_to(eversion_t to) {
    if (to > dirty_to)
      dirty_to = to;
  }
  void mark_dirty_from(eversion_t from) {
    if (from < dirty_from)
      dirty_from = from;
  }
  void mark_writeout_from(eversion_t from) {
    if (from < writeout_from)
      writeout_from = from;
  }
  void add_divergent_prior(eversion_t version, hobject_t obj) {
    divergent_priors.insert(make_pair(version, obj));
    dirty_divergent_priors = true;
  }
public:
  void mark_log_for_rewrite() {
    mark_dirty_to(eversion_t::max());
    mark_dirty_from(eversion_t());
    touched_log = false;
  }
protected:

  /// DEBUG
  set<string> log_keys_debug;
  static void clear_after(set<string> *log_keys_debug, const string &lb) {
    if (!log_keys_debug)
      return;
    for (set<string>::iterator i = log_keys_debug->lower_bound(lb);
	 i != log_keys_debug->end();
	 log_keys_debug->erase(i++));
  }
  static void clear_up_to(set<string> *log_keys_debug, const string &ub) {
    if (!log_keys_debug)
      return;
    for (set<string>::iterator i = log_keys_debug->begin();
	 i != log_keys_debug->end() && *i < ub;
	 log_keys_debug->erase(i++));
  }
  void check() {
    if (!pg_log_debug)
      return;
    assert(log.log.size() == log_keys_debug.size());
    for (list<pg_log_entry_t>::iterator i = log.log.begin();
	 i != log.log.end();
	 ++i) {
      assert(log_keys_debug.count(i->get_key_name()));
    }
  }

  void undirty() {
    dirty_to = eversion_t();
    dirty_from = eversion_t::max();
    dirty_divergent_priors = false;
    touched_log = true;
    trimmed.clear();
    writeout_from = eversion_t::max();
    check();
  }
public:
  PGLog(CephContext *cct = 0) :
    pg_log_debug(!(cct && !(cct->_conf->osd_debug_pg_log_writeout))),
    touched_log(false), dirty_from(eversion_t::max()),
    writeout_from(eversion_t::max()),
    dirty_divergent_priors(false), cct(cct) {}


  void reset_backfill();

  void clear();

  //////////////////// get or set missing ////////////////////

  const pg_missing_t& get_missing() const { return missing; }

  void missing_got(map<hobject_t, pg_missing_t::item>::const_iterator m) {
    map<hobject_t, pg_missing_t::item>::iterator p = missing.missing.find(m->first);
    missing.got(p);
  }

  void revise_have(hobject_t oid, eversion_t have) {
    missing.revise_have(oid, have);
  }

  void revise_need(hobject_t oid, eversion_t need) {
    missing.revise_need(oid, need);
  }

  void missing_add(const hobject_t& oid, eversion_t need, eversion_t have) {
    missing.add(oid, need, have);
  }

  void missing_rm(map<hobject_t, pg_missing_t::item>::const_iterator m) {
    map<hobject_t, pg_missing_t::item>::iterator p = missing.missing.find(m->first);
    missing.rm(p);
  }

  void missing_add_event(const pg_log_entry_t &e) {
    missing.add_next_event(e);
  }

  //////////////////// get or set log ////////////////////

  const IndexedLog &get_log() const { return log; }

  const eversion_t &get_tail() const { return log.tail; }

  void set_tail(eversion_t tail) { log.tail = tail; }

  const eversion_t &get_head() const { return log.head; }

  void set_head(eversion_t head) { log.head = head; }

  void set_last_requested(version_t last_requested) {
    log.last_requested = last_requested;
  }

  void index() { log.index(); }

  void unindex() { log.unindex(); }

  void add(pg_log_entry_t& e) {
    mark_writeout_from(e.version);
    log.add(e);
  }

  void reset_recovery_pointers() { log.reset_recovery_pointers(); }

  static void clear_info_log(
    spg_t pgid,
    ObjectStore::Transaction *t);

  void trim(
    LogEntryHandler *handler,
    eversion_t trim_to,
    pg_info_t &info);

  void trim_rollback_info(
    eversion_t trim_rollback_to,
    LogEntryHandler *h) {
    if (trim_rollback_to > log.can_rollback_to)
      log.can_rollback_to = trim_rollback_to;
    log.advance_rollback_info_trimmed_to(
      trim_rollback_to,
      h);
  }

  eversion_t get_rollback_trimmed_to() const {
    return log.rollback_info_trimmed_to;
  }

  void clear_can_rollback_to(LogEntryHandler *h) {
    log.can_rollback_to = log.head;
    log.advance_rollback_info_trimmed_to(
      log.head,
      h);
  }

  //////////////////// get or set log & missing ////////////////////

  void claim_log_and_clear_rollback_info(const pg_log_t &o, LogEntryHandler *h) {
    log.can_rollback_to = log.head;
    log.advance_rollback_info_trimmed_to(log.head, h);
    log.claim_log_and_clear_rollback_info(o);
    missing.clear();
    mark_dirty_to(eversion_t::max());
  }

  void split_into(
      pg_t child_pgid,
      unsigned split_bits,
      PGLog *opg_log) { 
    log.split_into(child_pgid, split_bits, &(opg_log->log));
    missing.split_into(child_pgid, split_bits, &(opg_log->missing));
    opg_log->mark_dirty_to(eversion_t::max());
    mark_dirty_to(eversion_t::max());
  }

  void recover_got(hobject_t oid, eversion_t v, pg_info_t &info) {
    if (missing.is_missing(oid, v)) {
      missing.got(oid, v);
      
      // raise last_complete?
      if (missing.missing.empty()) {
	log.complete_to = log.log.end();
	info.last_complete = info.last_update;
      }
      while (log.complete_to != log.log.end()) {
	if (missing.missing[missing.rmissing.begin()->second].need <=
	    log.complete_to->version)
	  break;
	if (info.last_complete < log.complete_to->version)
	  info.last_complete = log.complete_to->version;
	++log.complete_to;
      }
    }

    if (log.can_rollback_to < v)
      log.can_rollback_to = v;
  }

  void activate_not_complete(pg_info_t &info) {
    log.complete_to = log.log.begin();
    while (log.complete_to->version <
	   missing.missing[missing.rmissing.begin()->second].need)
      ++log.complete_to;
    assert(log.complete_to != log.log.end());
    if (log.complete_to == log.log.begin()) {
      info.last_complete = eversion_t();
    } else {
      --log.complete_to;
      info.last_complete = log.complete_to->version;
      ++log.complete_to;
    }
    log.last_requested = 0;
  }

  void proc_replica_log(ObjectStore::Transaction& t, pg_info_t &oinfo, const pg_log_t &olog,
			pg_missing_t& omissing, pg_shard_t from) const;

protected:
  static void split_by_object(
    list<pg_log_entry_t> &entries,
    map<hobject_t, list<pg_log_entry_t> > *out_entries) {
    while (!entries.empty()) {
      list<pg_log_entry_t> &out_list = (*out_entries)[entries.front().soid];
      out_list.splice(out_list.end(), entries, entries.begin());
    }
  }

  /**
   * Merge complete list of divergent entries for an object
   *
   * @param new_divergent_prior [out] filled out for a new divergent prior
   */
  static void _merge_object_divergent_entries(
    const IndexedLog &log,               ///< [in] log to merge against
    const hobject_t &hoid,               ///< [in] object we are merging
    const list<pg_log_entry_t> &entries, ///< [in] entries for hoid to merge
    const pg_info_t &oinfo,              ///< [in] info for merging entries
    eversion_t olog_can_rollback_to,     ///< [in] rollback boundary
    pg_missing_t &omissing,              ///< [in,out] missing to adjust, use
    boost::optional<pair<eversion_t, hobject_t> > *new_divergent_prior,
    LogEntryHandler *rollbacker          ///< [in] optional rollbacker object
    );

  /// Merge all entries using above
  static void _merge_divergent_entries(
    const IndexedLog &log,               ///< [in] log to merge against
    list<pg_log_entry_t> &entries,       ///< [in] entries to merge
    const pg_info_t &oinfo,              ///< [in] info for merging entries
    eversion_t olog_can_rollback_to,     ///< [in] rollback boundary
    pg_missing_t &omissing,              ///< [in,out] missing to adjust, use
    map<eversion_t, hobject_t> *priors,  ///< [out] target for new priors
    LogEntryHandler *rollbacker          ///< [in] optional rollbacker object
    ) {
    map<hobject_t, list<pg_log_entry_t> > split;
    split_by_object(entries, &split);
    for (map<hobject_t, list<pg_log_entry_t> >::iterator i = split.begin();
	 i != split.end();
	 ++i) {
      boost::optional<pair<eversion_t, hobject_t> > new_divergent_prior;
      _merge_object_divergent_entries(
	log,
	i->first,
	i->second,
	oinfo,
	olog_can_rollback_to,
	omissing,
	&new_divergent_prior,
	rollbacker);
      if (priors && new_divergent_prior) {
	(*priors)[new_divergent_prior->first] = new_divergent_prior->second;
      }
    }
  }

  /**
   * Exists for use in TestPGLog for simply testing single divergent log
   * cases
   */
  void merge_old_entry(
    ObjectStore::Transaction& t,
    const pg_log_entry_t& oe,
    const pg_info_t& info,
    LogEntryHandler *rollbacker) {
    boost::optional<pair<eversion_t, hobject_t> > new_divergent_prior;
    list<pg_log_entry_t> entries;
    entries.push_back(oe);
    _merge_object_divergent_entries(
      log,
      oe.soid,
      entries,
      info,
      log.can_rollback_to,
      missing,
      &new_divergent_prior,
      rollbacker);
    if (new_divergent_prior)
      add_divergent_prior(
	(*new_divergent_prior).first,
	(*new_divergent_prior).second);
  }
public:
  void rewind_divergent_log(ObjectStore::Transaction& t, eversion_t newhead,
                            pg_info_t &info, LogEntryHandler *rollbacker,
                            bool &dirty_info, bool &dirty_big_info);

  void merge_log(ObjectStore::Transaction& t, pg_info_t &oinfo, pg_log_t &olog,
		 pg_shard_t from,
		 pg_info_t &info, LogEntryHandler *rollbacker,
		 bool &dirty_info, bool &dirty_big_info);

  void write_log(ObjectStore::Transaction& t, const coll_t& coll,
		 const ghobject_t &log_oid);

  static void write_log(ObjectStore::Transaction& t, pg_log_t &log,
    const coll_t& coll,
    const ghobject_t &log_oid, map<eversion_t, hobject_t> &divergent_priors);

  static void _write_log(
    ObjectStore::Transaction& t, pg_log_t &log,
    const coll_t& coll, const ghobject_t &log_oid,
    map<eversion_t, hobject_t> &divergent_priors,
    eversion_t dirty_to,
    eversion_t dirty_from,
    eversion_t writeout_from,
    const set<eversion_t> &trimmed,
    bool dirty_divergent_priors,
    bool touch_log,
    set<string> *log_keys_debug
    );

  void read_log(ObjectStore *store, coll_t pg_coll,
		coll_t log_coll, ghobject_t log_oid,
		const pg_info_t &info, ostringstream &oss) {
    return read_log(
      store, pg_coll, log_coll, log_oid, info, divergent_priors,
      log, missing, oss,
      (pg_log_debug ? &log_keys_debug : 0));
  }

  static void read_log(ObjectStore *store, coll_t pg_coll,
    coll_t log_coll, ghobject_t log_oid,
    const pg_info_t &info, map<eversion_t, hobject_t> &divergent_priors,
    IndexedLog &log,
    pg_missing_t &missing, ostringstream &oss,
    set<string> *log_keys_debug = 0
    );
};
  
#endif // CEPH_PG_LOG_H
