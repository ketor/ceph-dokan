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
#include "acconfig.h"

#include <fstream>
#include <iostream>
#include <errno.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <signal.h>
#include <ctype.h>
#include <boost/scoped_ptr.hpp>

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif

#include "osd/PG.h"

#include "include/types.h"
#include "include/compat.h"

#include "OSD.h"
#include "OSDMap.h"
#include "Watch.h"
#include "osdc/Objecter.h"

#include "common/ceph_argparse.h"
#include "common/version.h"
#include "common/io_priority.h"

#include "os/ObjectStore.h"

#include "ReplicatedPG.h"

#include "Ager.h"


#include "msg/Messenger.h"
#include "msg/Message.h"

#include "mon/MonClient.h"

#include "messages/MLog.h"

#include "messages/MGenericMessage.h"
#include "messages/MPing.h"
#include "messages/MOSDPing.h"
#include "messages/MOSDFailure.h"
#include "messages/MOSDMarkMeDown.h"
#include "messages/MOSDOp.h"
#include "messages/MOSDOpReply.h"
#include "messages/MOSDRepOp.h"
#include "messages/MOSDRepOpReply.h"
#include "messages/MOSDSubOp.h"
#include "messages/MOSDSubOpReply.h"
#include "messages/MOSDBoot.h"
#include "messages/MOSDPGTemp.h"

#include "messages/MOSDMap.h"
#include "messages/MMonGetOSDMap.h"
#include "messages/MOSDPGNotify.h"
#include "messages/MOSDPGQuery.h"
#include "messages/MOSDPGLog.h"
#include "messages/MOSDPGRemove.h"
#include "messages/MOSDPGInfo.h"
#include "messages/MOSDPGCreate.h"
#include "messages/MOSDPGTrim.h"
#include "messages/MOSDPGScan.h"
#include "messages/MOSDPGBackfill.h"
#include "messages/MOSDPGMissing.h"
#include "messages/MBackfillReserve.h"
#include "messages/MRecoveryReserve.h"
#include "messages/MOSDECSubOpWrite.h"
#include "messages/MOSDECSubOpWriteReply.h"
#include "messages/MOSDECSubOpRead.h"
#include "messages/MOSDECSubOpReadReply.h"

#include "messages/MOSDAlive.h"

#include "messages/MOSDScrub.h"
#include "messages/MOSDRepScrub.h"

#include "messages/MMonCommand.h"
#include "messages/MCommand.h"
#include "messages/MCommandReply.h"

#include "messages/MPGStats.h"
#include "messages/MPGStatsAck.h"

#include "messages/MWatchNotify.h"
#include "messages/MOSDPGPush.h"
#include "messages/MOSDPGPushReply.h"
#include "messages/MOSDPGPull.h"

#include "common/perf_counters.h"
#include "common/Timer.h"
#include "common/LogClient.h"
#include "common/HeartbeatMap.h"
#include "common/admin_socket.h"

#include "global/signal_handler.h"
#include "global/pidfile.h"

#include "include/color.h"
#include "perfglue/cpu_profiler.h"
#include "perfglue/heap_profiler.h"

#include "osd/ClassHandler.h"
#include "osd/OpRequest.h"

#include "auth/AuthAuthorizeHandler.h"

#include "common/errno.h"

#include "objclass/objclass.h"

#include "common/cmdparse.h"
#include "include/str_list.h"

#include "include/assert.h"
#include "common/config.h"

#ifdef WITH_LTTNG
#include "tracing/osd.h"
#else
#define tracepoint(...)
#endif

static coll_t META_COLL("meta");

#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix _prefix(_dout, whoami, get_osdmap_epoch())

static ostream& _prefix(std::ostream* _dout, int whoami, epoch_t epoch) {
  return *_dout << "osd." << whoami << " " << epoch << " ";
}

//Initial features in new superblock.
//Features here are also automatically upgraded
CompatSet OSD::get_osd_initial_compat_set() {
  CompatSet::FeatureSet ceph_osd_feature_compat;
  CompatSet::FeatureSet ceph_osd_feature_ro_compat;
  CompatSet::FeatureSet ceph_osd_feature_incompat;
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_BASE);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_PGINFO);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_OLOC);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_LEC);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_CATEGORIES);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_HOBJECTPOOL);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_BIGINFO);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_LEVELDBINFO);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_LEVELDBLOG);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_SNAPMAPPER);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_HINTS);
  ceph_osd_feature_incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_PGMETA);
  return CompatSet(ceph_osd_feature_compat, ceph_osd_feature_ro_compat,
		   ceph_osd_feature_incompat);
}

//Features are added here that this OSD supports.
CompatSet OSD::get_osd_compat_set() {
  CompatSet compat =  get_osd_initial_compat_set();
  //Any features here can be set in code, but not in initial superblock
  compat.incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_SHARDS);
  return compat;
}

OSDService::OSDService(OSD *osd) :
  osd(osd),
  cct(osd->cct),
  whoami(osd->whoami), store(osd->store),
  log_client(osd->log_client), clog(osd->clog),
  pg_recovery_stats(osd->pg_recovery_stats),
  cluster_messenger(osd->cluster_messenger),
  client_messenger(osd->client_messenger),
  logger(osd->logger),
  recoverystate_perf(osd->recoverystate_perf),
  monc(osd->monc),
  op_wq(osd->op_shardedwq),
  peering_wq(osd->peering_wq),
  recovery_wq(osd->recovery_wq),
  snap_trim_wq(osd->snap_trim_wq),
  scrub_wq(osd->scrub_wq),
  rep_scrub_wq(osd->rep_scrub_wq),
  recovery_gen_wq("recovery_gen_wq", cct->_conf->osd_recovery_thread_timeout,
		  &osd->recovery_tp),
  op_gen_wq("op_gen_wq", cct->_conf->osd_recovery_thread_timeout, &osd->osd_tp),
  class_handler(osd->class_handler),
  pg_epoch_lock("OSDService::pg_epoch_lock"),
  publish_lock("OSDService::publish_lock"),
  pre_publish_lock("OSDService::pre_publish_lock"),
  peer_map_epoch_lock("OSDService::peer_map_epoch_lock"),
  sched_scrub_lock("OSDService::sched_scrub_lock"), scrubs_pending(0),
  scrubs_active(0),
  agent_lock("OSD::agent_lock"),
  agent_valid_iterator(false),
  agent_ops(0),
  agent_active(true),
  agent_thread(this),
  agent_stop_flag(false),
  agent_timer_lock("OSD::agent_timer_lock"),
  agent_timer(osd->client_messenger->cct, agent_timer_lock),
  objecter(new Objecter(osd->client_messenger->cct, osd->objecter_messenger, osd->monc, NULL, 0, 0)),
  objecter_finisher(osd->client_messenger->cct),
  watch_lock("OSD::watch_lock"),
  watch_timer(osd->client_messenger->cct, watch_lock),
  next_notif_id(0),
  backfill_request_lock("OSD::backfill_request_lock"),
  backfill_request_timer(cct, backfill_request_lock, false),
  last_tid(0),
  tid_lock("OSDService::tid_lock"),
  reserver_finisher(cct),
  local_reserver(&reserver_finisher, cct->_conf->osd_max_backfills,
		 cct->_conf->osd_min_recovery_priority),
  remote_reserver(&reserver_finisher, cct->_conf->osd_max_backfills,
		  cct->_conf->osd_min_recovery_priority),
  pg_temp_lock("OSDService::pg_temp_lock"),
  map_cache_lock("OSDService::map_lock"),
  map_cache(cct, cct->_conf->osd_map_cache_size),
  map_bl_cache(cct->_conf->osd_map_cache_size),
  map_bl_inc_cache(cct->_conf->osd_map_cache_size),
  in_progress_split_lock("OSDService::in_progress_split_lock"),
  stat_lock("OSD::stat_lock"),
  full_status_lock("OSDService::full_status_lock"),
  cur_state(NONE),
  last_msg(0),
  cur_ratio(0),
  epoch_lock("OSDService::epoch_lock"),
  boot_epoch(0), up_epoch(0), bind_epoch(0),
  is_stopping_lock("OSDService::is_stopping_lock"),
  state(NOT_STOPPING)
#ifdef PG_DEBUG_REFS
  , pgid_lock("OSDService::pgid_lock")
#endif
{
  objecter->init();
}

OSDService::~OSDService()
{
  delete objecter;
}

void OSDService::_start_split(spg_t parent, const set<spg_t> &children)
{
  for (set<spg_t>::const_iterator i = children.begin();
       i != children.end();
       ++i) {
    dout(10) << __func__ << ": Starting split on pg " << *i
	     << ", parent=" << parent << dendl;
    assert(!pending_splits.count(*i));
    assert(!in_progress_splits.count(*i));
    pending_splits.insert(make_pair(*i, parent));

    assert(!rev_pending_splits[parent].count(*i));
    rev_pending_splits[parent].insert(*i);
  }
}

void OSDService::mark_split_in_progress(spg_t parent, const set<spg_t> &children)
{
  Mutex::Locker l(in_progress_split_lock);
  map<spg_t, set<spg_t> >::iterator piter = rev_pending_splits.find(parent);
  assert(piter != rev_pending_splits.end());
  for (set<spg_t>::const_iterator i = children.begin();
       i != children.end();
       ++i) {
    assert(piter->second.count(*i));
    assert(pending_splits.count(*i));
    assert(!in_progress_splits.count(*i));
    assert(pending_splits[*i] == parent);

    pending_splits.erase(*i);
    piter->second.erase(*i);
    in_progress_splits.insert(*i);
  }
  if (piter->second.empty())
    rev_pending_splits.erase(piter);
}

void OSDService::cancel_pending_splits_for_parent(spg_t parent)
{
  Mutex::Locker l(in_progress_split_lock);
  return _cancel_pending_splits_for_parent(parent);
}

void OSDService::_cancel_pending_splits_for_parent(spg_t parent)
{
  map<spg_t, set<spg_t> >::iterator piter = rev_pending_splits.find(parent);
  if (piter == rev_pending_splits.end())
    return;

  for (set<spg_t>::iterator i = piter->second.begin();
       i != piter->second.end();
       ++i) {
    assert(pending_splits.count(*i));
    assert(!in_progress_splits.count(*i));
    pending_splits.erase(*i);
    dout(10) << __func__ << ": Completing split on pg " << *i
	     << " for parent: " << parent << dendl;
    _cancel_pending_splits_for_parent(*i);
  }
  rev_pending_splits.erase(piter);
}

void OSDService::_maybe_split_pgid(OSDMapRef old_map,
				  OSDMapRef new_map,
				  spg_t pgid)
{
  assert(old_map->have_pg_pool(pgid.pool()));
  if (pgid.ps() < static_cast<unsigned>(old_map->get_pg_num(pgid.pool()))) {
    set<spg_t> children;
    pgid.is_split(old_map->get_pg_num(pgid.pool()),
		  new_map->get_pg_num(pgid.pool()), &children);
    _start_split(pgid, children);
  } else {
    assert(pgid.ps() < static_cast<unsigned>(new_map->get_pg_num(pgid.pool())));
  }
}

void OSDService::init_splits_between(spg_t pgid,
				     OSDMapRef frommap,
				     OSDMapRef tomap)
{
  // First, check whether we can avoid this potentially expensive check
  if (tomap->have_pg_pool(pgid.pool()) &&
      pgid.is_split(
	frommap->get_pg_num(pgid.pool()),
	tomap->get_pg_num(pgid.pool()),
	NULL)) {
    // Ok, a split happened, so we need to walk the osdmaps
    set<spg_t> new_pgs; // pgs to scan on each map
    new_pgs.insert(pgid);
    OSDMapRef curmap(get_map(frommap->get_epoch()));
    for (epoch_t e = frommap->get_epoch() + 1;
	 e <= tomap->get_epoch();
	 ++e) {
      OSDMapRef nextmap(try_get_map(e));
      if (!nextmap)
	continue;
      set<spg_t> even_newer_pgs; // pgs added in this loop
      for (set<spg_t>::iterator i = new_pgs.begin(); i != new_pgs.end(); ++i) {
	set<spg_t> split_pgs;
	if (i->is_split(curmap->get_pg_num(i->pool()),
			nextmap->get_pg_num(i->pool()),
			&split_pgs)) {
	  start_split(*i, split_pgs);
	  even_newer_pgs.insert(split_pgs.begin(), split_pgs.end());
	}
      }
      new_pgs.insert(even_newer_pgs.begin(), even_newer_pgs.end());
      curmap = nextmap;
    }
    assert(curmap == tomap); // we must have had both frommap and tomap
  }
}

void OSDService::expand_pg_num(OSDMapRef old_map,
			       OSDMapRef new_map)
{
  Mutex::Locker l(in_progress_split_lock);
  for (set<spg_t>::iterator i = in_progress_splits.begin();
       i != in_progress_splits.end();
    ) {
    if (!new_map->have_pg_pool(i->pool())) {
      in_progress_splits.erase(i++);
    } else {
      _maybe_split_pgid(old_map, new_map, *i);
      ++i;
    }
  }
  for (map<spg_t, spg_t>::iterator i = pending_splits.begin();
       i != pending_splits.end();
    ) {
    if (!new_map->have_pg_pool(i->first.pool())) {
      rev_pending_splits.erase(i->second);
      pending_splits.erase(i++);
    } else {
      _maybe_split_pgid(old_map, new_map, i->first);
      ++i;
    }
  }
}

bool OSDService::splitting(spg_t pgid)
{
  Mutex::Locker l(in_progress_split_lock);
  return in_progress_splits.count(pgid) ||
    pending_splits.count(pgid);
}

void OSDService::complete_split(const set<spg_t> &pgs)
{
  Mutex::Locker l(in_progress_split_lock);
  for (set<spg_t>::const_iterator i = pgs.begin();
       i != pgs.end();
       ++i) {
    dout(10) << __func__ << ": Completing split on pg " << *i << dendl;
    assert(!pending_splits.count(*i));
    assert(in_progress_splits.count(*i));
    in_progress_splits.erase(*i);
  }
}

void OSDService::need_heartbeat_peer_update()
{
  osd->need_heartbeat_peer_update();
}

void OSDService::pg_stat_queue_enqueue(PG *pg)
{
  osd->pg_stat_queue_enqueue(pg);
}

void OSDService::pg_stat_queue_dequeue(PG *pg)
{
  osd->pg_stat_queue_dequeue(pg);
}

void OSDService::start_shutdown()
{
  {
    Mutex::Locker l(agent_timer_lock);
    agent_timer.cancel_all_events();
    agent_timer.shutdown();
  }
}

void OSDService::shutdown()
{
  reserver_finisher.stop();
  {
    Mutex::Locker l(watch_lock);
    watch_timer.shutdown();
  }

  objecter->shutdown();
  objecter_finisher.stop();

  {
    Mutex::Locker l(backfill_request_lock);
    backfill_request_timer.shutdown();
  }
  osdmap = OSDMapRef();
  next_osdmap = OSDMapRef();
}

void OSDService::init()
{
  reserver_finisher.start();
  objecter_finisher.start();
  objecter->set_client_incarnation(0);
  objecter->start();
  watch_timer.init();
  agent_timer.init();

  agent_thread.create();
}

void OSDService::activate_map()
{
  // wake/unwake the tiering agent
  agent_lock.Lock();
  agent_active =
    !osdmap->test_flag(CEPH_OSDMAP_NOTIERAGENT) &&
    osd->is_active();
  agent_cond.Signal();
  agent_lock.Unlock();
}

class AgentTimeoutCB : public Context {
  PGRef pg;
public:
  AgentTimeoutCB(PGRef _pg) : pg(_pg) {}
  void finish(int) {
    pg->agent_choose_mode_restart();
  }
};

void OSDService::agent_entry()
{
  dout(10) << __func__ << " start" << dendl;
  agent_lock.Lock();

  while (!agent_stop_flag) {
    if (agent_queue.empty()) {
      dout(20) << __func__ << " empty queue" << dendl;
      agent_cond.Wait(agent_lock);
      continue;
    }
    uint64_t level = agent_queue.rbegin()->first;
    set<PGRef>& top = agent_queue.rbegin()->second;
    dout(10) << __func__
	     << " tiers " << agent_queue.size()
	     << ", top is " << level
	     << " with pgs " << top.size()
	     << ", ops " << agent_ops << "/"
	     << g_conf->osd_agent_max_ops
	     << (agent_active ? " active" : " NOT ACTIVE")
	     << dendl;
    dout(20) << __func__ << " oids " << agent_oids << dendl;
    if (agent_ops >= g_conf->osd_agent_max_ops || top.empty() ||
	!agent_active) {
      agent_cond.Wait(agent_lock);
      continue;
    }

    if (!agent_valid_iterator || agent_queue_pos == top.end()) {
      agent_queue_pos = top.begin();
      agent_valid_iterator = true;
    }
    PGRef pg = *agent_queue_pos;
    int max = g_conf->osd_agent_max_ops - agent_ops;
    agent_lock.Unlock();
    if (!pg->agent_work(max)) {
      dout(10) << __func__ << " " << *pg
	<< " no agent_work, delay for " << g_conf->osd_agent_delay_time
	<< " seconds" << dendl;

      osd->logger->inc(l_osd_tier_delay);
      // Queue a timer to call agent_choose_mode for this pg in 5 seconds
      agent_timer_lock.Lock();
      Context *cb = new AgentTimeoutCB(pg);
      agent_timer.add_event_after(g_conf->osd_agent_delay_time, cb);
      agent_timer_lock.Unlock();
    }
    agent_lock.Lock();
  }
  agent_lock.Unlock();
  dout(10) << __func__ << " finish" << dendl;
}

void OSDService::agent_stop()
{
  {
    Mutex::Locker l(agent_lock);

    // By this time all ops should be cancelled
    assert(agent_ops == 0);
    // By this time all PGs are shutdown and dequeued
    if (!agent_queue.empty()) {
      set<PGRef>& top = agent_queue.rbegin()->second;
      derr << "agent queue not empty, for example " << (*top.begin())->info.pgid << dendl;
      assert(0 == "agent queue not empty");
    }

    agent_stop_flag = true;
    agent_cond.Signal();
  }
  agent_thread.join();
}

// -------------------------------------

float OSDService::get_full_ratio()
{
  float full_ratio = cct->_conf->osd_failsafe_full_ratio;
  if (full_ratio > 1.0) full_ratio /= 100.0;
  return full_ratio;
}

float OSDService::get_nearfull_ratio()
{
  float nearfull_ratio = cct->_conf->osd_failsafe_nearfull_ratio;
  if (nearfull_ratio > 1.0) nearfull_ratio /= 100.0;
  return nearfull_ratio;
}

void OSDService::check_nearfull_warning(const osd_stat_t &osd_stat)
{
  Mutex::Locker l(full_status_lock);
  enum s_names new_state;

  time_t now = ceph_clock_gettime(NULL);

  // We base ratio on kb_avail rather than kb_used because they can
  // differ significantly e.g. on btrfs volumes with a large number of
  // chunks reserved for metadata, and for our purposes (avoiding
  // completely filling the disk) it's far more important to know how
  // much space is available to use than how much we've already used.
  float ratio = ((float)(osd_stat.kb - osd_stat.kb_avail)) / ((float)osd_stat.kb);
  float nearfull_ratio = get_nearfull_ratio();
  float full_ratio = get_full_ratio();
  cur_ratio = ratio;

  if (full_ratio > 0 && ratio > full_ratio) {
    new_state = FULL;
  } else if (nearfull_ratio > 0 && ratio > nearfull_ratio) {
    new_state = NEAR;
  } else {
    cur_state = NONE;
    return;
  }

  if (cur_state != new_state) {
    cur_state = new_state;
  } else if (now - last_msg < cct->_conf->osd_op_complaint_time) {
    return;
  }
  last_msg = now;
  if (cur_state == FULL)
    clog->error() << "OSD full dropping all updates " << (int)(ratio * 100) << "% full";
  else
    clog->warn() << "OSD near full (" << (int)(ratio * 100) << "%)";
}

bool OSDService::check_failsafe_full()
{
  Mutex::Locker l(full_status_lock);
  if (cur_state == FULL)
    return true;
  return false;
}

bool OSDService::too_full_for_backfill(double *_ratio, double *_max_ratio)
{
  Mutex::Locker l(full_status_lock);
  double max_ratio;
  max_ratio = cct->_conf->osd_backfill_full_ratio;
  if (_ratio)
    *_ratio = cur_ratio;
  if (_max_ratio)
    *_max_ratio = max_ratio;
  return cur_ratio >= max_ratio;
}

void OSDService::update_osd_stat(vector<int>& hb_peers)
{
  Mutex::Locker lock(stat_lock);

  // fill in osd stats too
  struct statfs stbuf;
  osd->store->statfs(&stbuf);

  uint64_t bytes = stbuf.f_blocks * stbuf.f_bsize;
  uint64_t used = (stbuf.f_blocks - stbuf.f_bfree) * stbuf.f_bsize;
  uint64_t avail = stbuf.f_bavail * stbuf.f_bsize;

  osd_stat.kb = bytes >> 10;
  osd_stat.kb_used = used >> 10;
  osd_stat.kb_avail = avail >> 10;

  osd->logger->set(l_osd_stat_bytes, bytes);
  osd->logger->set(l_osd_stat_bytes_used, used);
  osd->logger->set(l_osd_stat_bytes_avail, avail);

  osd_stat.hb_in.swap(hb_peers);
  osd_stat.hb_out.clear();

  check_nearfull_warning(osd_stat);

  osd->op_tracker.get_age_ms_histogram(&osd_stat.op_queue_age_hist);

  dout(20) << "update_osd_stat " << osd_stat << dendl;
}

void OSDService::send_message_osd_cluster(int peer, Message *m, epoch_t from_epoch)
{
  OSDMapRef next_map = get_nextmap_reserved();
  // service map is always newer/newest
  assert(from_epoch <= next_map->get_epoch());

  if (next_map->is_down(peer) ||
      next_map->get_info(peer).up_from > from_epoch) {
    m->put();
    release_map(next_map);
    return;
  }
  const entity_inst_t& peer_inst = next_map->get_cluster_inst(peer);
  Connection *peer_con = osd->cluster_messenger->get_connection(peer_inst).get();
  share_map_peer(peer, peer_con, next_map);
  peer_con->send_message(m);
  release_map(next_map);
}

ConnectionRef OSDService::get_con_osd_cluster(int peer, epoch_t from_epoch)
{
  OSDMapRef next_map = get_nextmap_reserved();
  // service map is always newer/newest
  assert(from_epoch <= next_map->get_epoch());

  if (next_map->is_down(peer) ||
      next_map->get_info(peer).up_from > from_epoch) {
    release_map(next_map);
    return NULL;
  }
  ConnectionRef con = osd->cluster_messenger->get_connection(next_map->get_cluster_inst(peer));
  release_map(next_map);
  return con;
}

pair<ConnectionRef,ConnectionRef> OSDService::get_con_osd_hb(int peer, epoch_t from_epoch)
{
  OSDMapRef next_map = get_nextmap_reserved();
  // service map is always newer/newest
  assert(from_epoch <= next_map->get_epoch());

  pair<ConnectionRef,ConnectionRef> ret;
  if (next_map->is_down(peer) ||
      next_map->get_info(peer).up_from > from_epoch) {
    release_map(next_map);
    return ret;
  }
  ret.first = osd->hbclient_messenger->get_connection(next_map->get_hb_back_inst(peer));
  if (next_map->get_hb_front_addr(peer) != entity_addr_t())
    ret.second = osd->hbclient_messenger->get_connection(next_map->get_hb_front_inst(peer));
  release_map(next_map);
  return ret;
}


void OSDService::queue_want_pg_temp(pg_t pgid, vector<int>& want)
{
  Mutex::Locker l(pg_temp_lock);
  pg_temp_wanted[pgid] = want;
}

void OSDService::send_pg_temp()
{
  Mutex::Locker l(pg_temp_lock);
  if (pg_temp_wanted.empty())
    return;
  dout(10) << "send_pg_temp " << pg_temp_wanted << dendl;
  MOSDPGTemp *m = new MOSDPGTemp(osdmap->get_epoch());
  m->pg_temp = pg_temp_wanted;
  monc->send_mon_message(m);
}


// --------------------------------------
// dispatch

epoch_t OSDService::get_peer_epoch(int peer)
{
  Mutex::Locker l(peer_map_epoch_lock);
  map<int,epoch_t>::iterator p = peer_map_epoch.find(peer);
  if (p == peer_map_epoch.end())
    return 0;
  return p->second;
}

epoch_t OSDService::note_peer_epoch(int peer, epoch_t e)
{
  Mutex::Locker l(peer_map_epoch_lock);
  map<int,epoch_t>::iterator p = peer_map_epoch.find(peer);
  if (p != peer_map_epoch.end()) {
    if (p->second < e) {
      dout(10) << "note_peer_epoch osd." << peer << " has " << e << dendl;
      p->second = e;
    } else {
      dout(30) << "note_peer_epoch osd." << peer << " has " << p->second << " >= " << e << dendl;
    }
    return p->second;
  } else {
    dout(10) << "note_peer_epoch osd." << peer << " now has " << e << dendl;
    peer_map_epoch[peer] = e;
    return e;
  }
}
 
void OSDService::forget_peer_epoch(int peer, epoch_t as_of)
{
  Mutex::Locker l(peer_map_epoch_lock);
  map<int,epoch_t>::iterator p = peer_map_epoch.find(peer);
  if (p != peer_map_epoch.end()) {
    if (p->second <= as_of) {
      dout(10) << "forget_peer_epoch osd." << peer << " as_of " << as_of
	       << " had " << p->second << dendl;
      peer_map_epoch.erase(p);
    } else {
      dout(10) << "forget_peer_epoch osd." << peer << " as_of " << as_of
	       << " has " << p->second << " - not forgetting" << dendl;
    }
  }
}

bool OSDService::should_share_map(entity_name_t name, Connection *con,
                                  epoch_t epoch, OSDMapRef& osdmap,
                                  const epoch_t *sent_epoch_p)
{
  bool should_send = false;
  dout(20) << "should_share_map "
           << name << " " << con->get_peer_addr()
           << " " << epoch << dendl;

  // does client have old map?
  if (name.is_client()) {
    bool message_sendmap = epoch < osdmap->get_epoch();
    if (message_sendmap && sent_epoch_p) {
      dout(20) << "client session last_sent_epoch: "
               << *sent_epoch_p
               << " versus osdmap epoch " << osdmap->get_epoch() << dendl;
      if (*sent_epoch_p < osdmap->get_epoch()) {
        should_send = true;
      } // else we don't need to send it out again
    }
  }

  if (con->get_messenger() == osd->cluster_messenger &&
      con != osd->cluster_messenger->get_loopback_connection() &&
      osdmap->is_up(name.num()) &&
      (osdmap->get_cluster_addr(name.num()) == con->get_peer_addr() ||
       osdmap->get_hb_back_addr(name.num()) == con->get_peer_addr())) {
    // remember
    epoch_t has = MAX(get_peer_epoch(name.num()), epoch);

    // share?
    if (has < osdmap->get_epoch()) {
      dout(10) << name << " " << con->get_peer_addr()
               << " has old map " << epoch << " < "
               << osdmap->get_epoch() << dendl;
      should_send = true;
    }
  }

  return should_send;
}

void OSDService::share_map(
    entity_name_t name,
    Connection *con,
    epoch_t epoch,
    OSDMapRef& osdmap,
    epoch_t *sent_epoch_p)
{
  dout(20) << "share_map "
	   << name << " " << con->get_peer_addr()
	   << " " << epoch << dendl;

  if ((!osd->is_active()) && (!osd->is_stopping())) {
    /*It is safe not to proceed as OSD is not in healthy state*/
    return;
  }

  bool want_shared = should_share_map(name, con, epoch,
                                      osdmap, sent_epoch_p);

  if (want_shared){
    if (name.is_client()) {
      dout(10) << name << " has old map " << epoch
          << " < " << osdmap->get_epoch() << dendl;
      // we know the Session is valid or we wouldn't be sending
      if (sent_epoch_p) {
	*sent_epoch_p = osdmap->get_epoch();
      }
      send_incremental_map(epoch, con, osdmap);
    } else if (con->get_messenger() == osd->cluster_messenger &&
        osdmap->is_up(name.num()) &&
        (osdmap->get_cluster_addr(name.num()) == con->get_peer_addr() ||
            osdmap->get_hb_back_addr(name.num()) == con->get_peer_addr())) {
      dout(10) << name << " " << con->get_peer_addr()
	               << " has old map " << epoch << " < "
	               << osdmap->get_epoch() << dendl;
      note_peer_epoch(name.num(), osdmap->get_epoch());
      send_incremental_map(epoch, con, osdmap);
    }
  }
}


void OSDService::share_map_peer(int peer, Connection *con, OSDMapRef map)
{
  if (!map)
    map = get_osdmap();

  // send map?
  epoch_t pe = get_peer_epoch(peer);
  if (pe) {
    if (pe < map->get_epoch()) {
      send_incremental_map(pe, con, map);
      note_peer_epoch(peer, map->get_epoch());
    } else
      dout(20) << "share_map_peer " << con << " already has epoch " << pe << dendl;
  } else {
    dout(20) << "share_map_peer " << con << " don't know epoch, doing nothing" << dendl;
    // no idea about peer's epoch.
    // ??? send recent ???
    // do nothing.
  }
}


bool OSDService::inc_scrubs_pending()
{
  bool result = false;

  sched_scrub_lock.Lock();
  if (scrubs_pending + scrubs_active < cct->_conf->osd_max_scrubs) {
    dout(20) << "inc_scrubs_pending " << scrubs_pending << " -> " << (scrubs_pending+1)
	     << " (max " << cct->_conf->osd_max_scrubs << ", active " << scrubs_active << ")" << dendl;
    result = true;
    ++scrubs_pending;
  } else {
    dout(20) << "inc_scrubs_pending " << scrubs_pending << " + " << scrubs_active << " active >= max " << cct->_conf->osd_max_scrubs << dendl;
  }
  sched_scrub_lock.Unlock();

  return result;
}

void OSDService::dec_scrubs_pending()
{
  sched_scrub_lock.Lock();
  dout(20) << "dec_scrubs_pending " << scrubs_pending << " -> " << (scrubs_pending-1)
	   << " (max " << cct->_conf->osd_max_scrubs << ", active " << scrubs_active << ")" << dendl;
  --scrubs_pending;
  assert(scrubs_pending >= 0);
  sched_scrub_lock.Unlock();
}

void OSDService::inc_scrubs_active(bool reserved)
{
  sched_scrub_lock.Lock();
  ++(scrubs_active);
  if (reserved) {
    --(scrubs_pending);
    dout(20) << "inc_scrubs_active " << (scrubs_active-1) << " -> " << scrubs_active
	     << " (max " << cct->_conf->osd_max_scrubs
	     << ", pending " << (scrubs_pending+1) << " -> " << scrubs_pending << ")" << dendl;
    assert(scrubs_pending >= 0);
  } else {
    dout(20) << "inc_scrubs_active " << (scrubs_active-1) << " -> " << scrubs_active
	     << " (max " << cct->_conf->osd_max_scrubs
	     << ", pending " << scrubs_pending << ")" << dendl;
  }
  sched_scrub_lock.Unlock();
}

void OSDService::dec_scrubs_active()
{
  sched_scrub_lock.Lock();
  dout(20) << "dec_scrubs_active " << scrubs_active << " -> " << (scrubs_active-1)
	   << " (max " << cct->_conf->osd_max_scrubs << ", pending " << scrubs_pending << ")" << dendl;
  --scrubs_active;
  sched_scrub_lock.Unlock();
}

void OSDService::retrieve_epochs(epoch_t *_boot_epoch, epoch_t *_up_epoch,
                                 epoch_t *_bind_epoch) const
{
  Mutex::Locker l(epoch_lock);
  if (_boot_epoch)
    *_boot_epoch = boot_epoch;
  if (_up_epoch)
    *_up_epoch = up_epoch;
  if (_bind_epoch)
    *_bind_epoch = bind_epoch;
}

void OSDService::set_epochs(const epoch_t *_boot_epoch, const epoch_t *_up_epoch,
                            const epoch_t *_bind_epoch)
{
  Mutex::Locker l(epoch_lock);
  if (_boot_epoch) {
    assert(*_boot_epoch == 0 || *_boot_epoch >= boot_epoch);
    boot_epoch = *_boot_epoch;
  }
  if (_up_epoch) {
    assert(*_up_epoch == 0 || *_up_epoch >= up_epoch);
    up_epoch = *_up_epoch;
  }
  if (_bind_epoch) {
    assert(*_bind_epoch == 0 || *_bind_epoch >= bind_epoch);
    bind_epoch = *_bind_epoch;
  }
}

bool OSDService::prepare_to_stop()
{
  Mutex::Locker l(is_stopping_lock);
  if (state != NOT_STOPPING)
    return false;

  OSDMapRef osdmap = get_osdmap();
  if (osdmap && osdmap->is_up(whoami)) {
    dout(0) << __func__ << " telling mon we are shutting down" << dendl;
    state = PREPARING_TO_STOP;
    monc->send_mon_message(new MOSDMarkMeDown(monc->get_fsid(),
					      osdmap->get_inst(whoami),
					      osdmap->get_epoch(),
					      true  // request ack
					      ));
    utime_t now = ceph_clock_now(cct);
    utime_t timeout;
    timeout.set_from_double(now + cct->_conf->osd_mon_shutdown_timeout);
    while ((ceph_clock_now(cct) < timeout) &&
	   (state != STOPPING)) {
      is_stopping_cond.WaitUntil(is_stopping_lock, timeout);
    }
  }
  dout(0) << __func__ << " starting shutdown" << dendl;
  state = STOPPING;
  return true;
}

void OSDService::got_stop_ack()
{
  Mutex::Locker l(is_stopping_lock);
  if (state == PREPARING_TO_STOP) {
    dout(0) << __func__ << " starting shutdown" << dendl;
    state = STOPPING;
    is_stopping_cond.Signal();
  } else {
    dout(10) << __func__ << " ignoring msg" << dendl;
  }
}


MOSDMap *OSDService::build_incremental_map_msg(epoch_t since, epoch_t to,
                                               OSDSuperblock& sblock)
{
  MOSDMap *m = new MOSDMap(monc->get_fsid());
  m->oldest_map = sblock.oldest_map;
  m->newest_map = sblock.newest_map;
  
  for (epoch_t e = to; e > since; e--) {
    bufferlist bl;
    if (e > m->oldest_map && get_inc_map_bl(e, bl)) {
      m->incremental_maps[e].claim(bl);
    } else if (get_map_bl(e, bl)) {
      m->maps[e].claim(bl);
      break;
    } else {
      derr << "since " << since << " to " << to
	   << " oldest " << m->oldest_map << " newest " << m->newest_map
	   << dendl;
      m->put();
      m = NULL;
      break;
    }
  }
  return m;
}

void OSDService::send_map(MOSDMap *m, Connection *con)
{
  con->send_message(m);
}

void OSDService::send_incremental_map(epoch_t since, Connection *con,
                                      OSDMapRef& osdmap)
{
  epoch_t to = osdmap->get_epoch();
  dout(10) << "send_incremental_map " << since << " -> " << to
           << " to " << con << " " << con->get_peer_addr() << dendl;

  MOSDMap *m = NULL;
  while (!m) {
    OSDSuperblock sblock(get_superblock());
    if (since < sblock.oldest_map) {
      // just send latest full map
      MOSDMap *m = new MOSDMap(monc->get_fsid());
      m->oldest_map = sblock.oldest_map;
      m->newest_map = sblock.newest_map;
      get_map_bl(to, m->maps[to]);
      send_map(m, con);
      return;
    }
    
    if (to > since && (int64_t)(to - since) > cct->_conf->osd_map_share_max_epochs) {
      dout(10) << "  " << (to - since) << " > max " << cct->_conf->osd_map_share_max_epochs
	       << ", only sending most recent" << dendl;
      since = to - cct->_conf->osd_map_share_max_epochs;
    }
    
    if (to - since > (epoch_t)cct->_conf->osd_map_message_max)
      to = since + cct->_conf->osd_map_message_max;
    m = build_incremental_map_msg(since, to, sblock);
  }
  send_map(m, con);
}

bool OSDService::_get_map_bl(epoch_t e, bufferlist& bl)
{
  bool found = map_bl_cache.lookup(e, &bl);
  if (found)
    return true;
  found = store->read(
    META_COLL, OSD::get_osdmap_pobject_name(e), 0, 0, bl) >= 0;
  if (found)
    _add_map_bl(e, bl);
  return found;
}

bool OSDService::get_inc_map_bl(epoch_t e, bufferlist& bl)
{
  Mutex::Locker l(map_cache_lock);
  bool found = map_bl_inc_cache.lookup(e, &bl);
  if (found)
    return true;
  found = store->read(
    META_COLL, OSD::get_inc_osdmap_pobject_name(e), 0, 0, bl) >= 0;
  if (found)
    _add_map_inc_bl(e, bl);
  return found;
}

void OSDService::_add_map_bl(epoch_t e, bufferlist& bl)
{
  dout(10) << "add_map_bl " << e << " " << bl.length() << " bytes" << dendl;
  map_bl_cache.add(e, bl);
}

void OSDService::_add_map_inc_bl(epoch_t e, bufferlist& bl)
{
  dout(10) << "add_map_inc_bl " << e << " " << bl.length() << " bytes" << dendl;
  map_bl_inc_cache.add(e, bl);
}

void OSDService::pin_map_inc_bl(epoch_t e, bufferlist &bl)
{
  Mutex::Locker l(map_cache_lock);
  map_bl_inc_cache.pin(e, bl);
}

void OSDService::pin_map_bl(epoch_t e, bufferlist &bl)
{
  Mutex::Locker l(map_cache_lock);
  map_bl_cache.pin(e, bl);
}

void OSDService::clear_map_bl_cache_pins(epoch_t e)
{
  Mutex::Locker l(map_cache_lock);
  map_bl_inc_cache.clear_pinned(e);
  map_bl_cache.clear_pinned(e);
}

OSDMapRef OSDService::_add_map(OSDMap *o)
{
  epoch_t e = o->get_epoch();

  if (cct->_conf->osd_map_dedup) {
    // Dedup against an existing map at a nearby epoch
    OSDMapRef for_dedup = map_cache.lower_bound(e);
    if (for_dedup) {
      OSDMap::dedup(for_dedup.get(), o);
    }
  }
  bool existed;
  OSDMapRef l = map_cache.add(e, o, &existed);
  if (existed) {
    delete o;
  }
  return l;
}

OSDMapRef OSDService::try_get_map(epoch_t epoch)
{
  Mutex::Locker l(map_cache_lock);
  OSDMapRef retval = map_cache.lookup(epoch);
  if (retval) {
    dout(30) << "get_map " << epoch << " -cached" << dendl;
    return retval;
  }

  OSDMap *map = new OSDMap;
  if (epoch > 0) {
    dout(20) << "get_map " << epoch << " - loading and decoding " << map << dendl;
    bufferlist bl;
    if (!_get_map_bl(epoch, bl)) {
      delete map;
      return OSDMapRef();
    }
    map->decode(bl);
  } else {
    dout(20) << "get_map " << epoch << " - return initial " << map << dendl;
  }
  return _add_map(map);
}

bool OSDService::queue_for_recovery(PG *pg)
{
  bool b = recovery_wq.queue(pg);
  if (b)
    dout(10) << "queue_for_recovery queued " << *pg << dendl;
  else
    dout(10) << "queue_for_recovery already queued " << *pg << dendl;
  return b;
}


// ops


void OSDService::reply_op_error(OpRequestRef op, int err)
{
  reply_op_error(op, err, eversion_t(), 0);
}

void OSDService::reply_op_error(OpRequestRef op, int err, eversion_t v,
                                version_t uv)
{
  MOSDOp *m = static_cast<MOSDOp*>(op->get_req());
  assert(m->get_type() == CEPH_MSG_OSD_OP);
  int flags;
  flags = m->get_flags() & (CEPH_OSD_FLAG_ACK|CEPH_OSD_FLAG_ONDISK);

  MOSDOpReply *reply = new MOSDOpReply(m, err, osdmap->get_epoch(), flags,
				       true);
  reply->set_reply_versions(v, uv);
  m->get_connection()->send_message(reply);
}

void OSDService::handle_misdirected_op(PG *pg, OpRequestRef op)
{
  MOSDOp *m = static_cast<MOSDOp*>(op->get_req());
  assert(m->get_type() == CEPH_MSG_OSD_OP);

  assert(m->get_map_epoch() >= pg->info.history.same_primary_since);

  if (pg->is_ec_pg()) {
    /**
       * OSD recomputes op target based on current OSDMap. With an EC pg, we
       * can get this result:
       * 1) client at map 512 sends an op to osd 3, pg_t 3.9 based on mapping
       *    [CRUSH_ITEM_NONE, 2, 3]/3
       * 2) OSD 3 at map 513 remaps op to osd 3, spg_t 3.9s0 based on mapping
       *    [3, 2, 3]/3
       * 3) PG 3.9s0 dequeues the op at epoch 512 and notices that it isn't primary
       *    -- misdirected op
       * 4) client resends and this time PG 3.9s0 having caught up to 513 gets
       *    it and fulfils it
       *
       * We can't compute the op target based on the sending map epoch due to
       * splitting.  The simplest thing is to detect such cases here and drop
       * them without an error (the client will resend anyway).
       */
    OSDMapRef opmap = try_get_map(m->get_map_epoch());
    if (!opmap) {
      dout(7) << __func__ << ": " << *pg << " no longer have map for "
	      << m->get_map_epoch() << ", dropping" << dendl;
      return;
    }
    pg_t _pgid = m->get_pg();
    spg_t pgid;
    if ((m->get_flags() & CEPH_OSD_FLAG_PGOP) == 0)
      _pgid = opmap->raw_pg_to_pg(_pgid);
    if (opmap->get_primary_shard(_pgid, &pgid) &&
	pgid.shard != pg->info.pgid.shard) {
      dout(7) << __func__ << ": " << *pg << " primary changed since "
	      << m->get_map_epoch() << ", dropping" << dendl;
      return;
    }
  }

  dout(7) << *pg << " misdirected op in " << m->get_map_epoch() << dendl;
  clog->warn() << m->get_source_inst() << " misdirected " << m->get_reqid()
	      << " pg " << m->get_pg()
	      << " to osd." << whoami
	      << " not " << pg->acting
	      << " in e" << m->get_map_epoch() << "/" << osdmap->get_epoch() << "\n";
  reply_op_error(op, -ENXIO);
}


void OSDService::dequeue_pg(PG *pg, list<OpRequestRef> *dequeued)
{
  osd->op_shardedwq.dequeue(pg, dequeued);
}

void OSDService::queue_for_peering(PG *pg)
{
  peering_wq.queue(pg);
}


// ====================================================================
// OSD

#undef dout_prefix
#define dout_prefix *_dout

int OSD::mkfs(CephContext *cct, ObjectStore *store, const string &dev,
	      uuid_d fsid, int whoami)
{
  int ret;

  try {
    // if we are fed a uuid for this osd, use it.
    store->set_fsid(cct->_conf->osd_uuid);

    ret = store->mkfs();
    if (ret) {
      derr << "OSD::mkfs: ObjectStore::mkfs failed with error " << ret << dendl;
      goto free_store;
    }

    ret = store->mount();
    if (ret) {
      derr << "OSD::mkfs: couldn't mount ObjectStore: error " << ret << dendl;
      goto free_store;
    }

    // age?
    if (cct->_conf->osd_age_time != 0) {
      if (cct->_conf->osd_age_time >= 0) {
        dout(0) << "aging..." << dendl;
        Ager ager(cct, store);
        ager.age(cct->_conf->osd_age_time,
          cct->_conf->osd_age,
          cct->_conf->osd_age - .05,
          50000,
          cct->_conf->osd_age - .05);
      }
    }

    OSDSuperblock sb;
    bufferlist sbbl;
    ret = store->read(META_COLL, OSD_SUPERBLOCK_POBJECT, 0, 0, sbbl);
    if (ret >= 0) {
      dout(0) << " have superblock" << dendl;
      if (whoami != sb.whoami) {
	derr << "provided osd id " << whoami << " != superblock's " << sb.whoami << dendl;
	ret = -EINVAL;
	goto umount_store;
      }
      if (fsid != sb.cluster_fsid) {
	derr << "provided cluster fsid " << fsid << " != superblock's " << sb.cluster_fsid << dendl;
	ret = -EINVAL;
	goto umount_store;
      }
    } else {
      // create superblock
      if (fsid.is_zero()) {
	derr << "must specify cluster fsid" << dendl;
	ret = -EINVAL;
	goto umount_store;
      }

      sb.cluster_fsid = fsid;
      sb.osd_fsid = store->get_fsid();
      sb.whoami = whoami;
      sb.compat_features = get_osd_initial_compat_set();

      // benchmark?
      if (cct->_conf->osd_auto_weight) {
	bufferlist bl;
	bufferptr bp(1048576);
	bp.zero();
	bl.push_back(bp);
	dout(0) << "testing disk bandwidth..." << dendl;
	utime_t start = ceph_clock_now(cct);
	object_t oid("disk_bw_test");
	for (int i=0; i<1000; i++) {
	  ObjectStore::Transaction *t = new ObjectStore::Transaction;
	  t->write(META_COLL, hobject_t(sobject_t(oid, 0)), i*bl.length(), bl.length(), bl);
	  store->queue_transaction_and_cleanup(NULL, t);
	}
	store->sync();
	utime_t end = ceph_clock_now(cct);
	end -= start;
	dout(0) << "measured " << (1000.0 / (double)end) << " mb/sec" << dendl;
	ObjectStore::Transaction tr;
	tr.remove(META_COLL, hobject_t(sobject_t(oid, 0)));
	ret = store->apply_transaction(tr);
	if (ret) {
	  derr << "OSD::mkfs: error while benchmarking: apply_transaction returned "
	       << ret << dendl;
	  goto umount_store;
	}
	
	// set osd weight
	sb.weight = (1000.0 / (double)end);
      }

      bufferlist bl;
      ::encode(sb, bl);

      ObjectStore::Transaction t;
      t.create_collection(META_COLL);
      t.write(META_COLL, OSD_SUPERBLOCK_POBJECT, 0, bl.length(), bl);
      ret = store->apply_transaction(t);
      if (ret) {
	derr << "OSD::mkfs: error while writing OSD_SUPERBLOCK_POBJECT: "
	     << "apply_transaction returned " << ret << dendl;
	goto umount_store;
      }
    }

    store->sync_and_flush();

    ret = write_meta(store, sb.cluster_fsid, sb.osd_fsid, whoami);
    if (ret) {
      derr << "OSD::mkfs: failed to write fsid file: error " << ret << dendl;
      goto umount_store;
    }

  }
  catch (const std::exception &se) {
    derr << "OSD::mkfs: caught exception " << se.what() << dendl;
    ret = 1000;
  }
  catch (...) {
    derr << "OSD::mkfs: caught unknown exception." << dendl;
    ret = 1000;
  }

umount_store:
  store->umount();
free_store:
  delete store;
  return ret;
}

int OSD::write_meta(ObjectStore *store, uuid_d& cluster_fsid, uuid_d& osd_fsid, int whoami)
{
  char val[80];
  int r;
  
  snprintf(val, sizeof(val), "%s", CEPH_OSD_ONDISK_MAGIC);
  r = store->write_meta("magic", val);
  if (r < 0)
    return r;

  snprintf(val, sizeof(val), "%d", whoami);
  r = store->write_meta("whoami", val);
  if (r < 0)
    return r;

  cluster_fsid.print(val);
  r = store->write_meta("ceph_fsid", val);
  if (r < 0)
    return r;

  r = store->write_meta("ready", "ready");
  if (r < 0)
    return r;

  return 0;
}

int OSD::peek_meta(ObjectStore *store, std::string& magic,
		   uuid_d& cluster_fsid, uuid_d& osd_fsid, int& whoami)
{
  string val;

  int r = store->read_meta("magic", &val);
  if (r < 0)
    return r;
  magic = val;

  r = store->read_meta("whoami", &val);
  if (r < 0)
    return r;
  whoami = atoi(val.c_str());

  r = store->read_meta("ceph_fsid", &val);
  if (r < 0)
    return r;
  r = cluster_fsid.parse(val.c_str());
  if (r < 0)
    return r;

  r = store->read_meta("fsid", &val);
  if (r < 0) {
    osd_fsid = uuid_d();
  } else {
    r = osd_fsid.parse(val.c_str());
    if (r < 0)
      return r;
  }

  return 0;
}


#undef dout_prefix
#define dout_prefix _prefix(_dout, whoami, get_osdmap_epoch())

// cons/des

OSD::OSD(CephContext *cct_, ObjectStore *store_,
	 int id,
	 Messenger *internal_messenger,
	 Messenger *external_messenger,
	 Messenger *hb_clientm,
	 Messenger *hb_front_serverm,
	 Messenger *hb_back_serverm,
	 Messenger *osdc_messenger,
	 MonClient *mc,
	 const std::string &dev, const std::string &jdev) :
  Dispatcher(cct_),
  osd_lock("OSD::osd_lock"),
  tick_timer(cct, osd_lock),
  authorize_handler_cluster_registry(new AuthAuthorizeHandlerRegistry(cct,
								      cct->_conf->auth_supported.length() ?
								      cct->_conf->auth_supported :
								      cct->_conf->auth_cluster_required)),
  authorize_handler_service_registry(new AuthAuthorizeHandlerRegistry(cct,
								      cct->_conf->auth_supported.length() ?
								      cct->_conf->auth_supported :
								      cct->_conf->auth_service_required)),
  cluster_messenger(internal_messenger),
  client_messenger(external_messenger),
  objecter_messenger(osdc_messenger),
  monc(mc),
  logger(NULL),
  recoverystate_perf(NULL),
  store(store_),
  log_client(cct, client_messenger, &mc->monmap, LogClient::NO_FLAGS),
  clog(log_client.create_channel()),
  whoami(id),
  dev_path(dev), journal_path(jdev),
  dispatch_running(false),
  asok_hook(NULL),
  osd_compat(get_osd_compat_set()),
  state(STATE_INITIALIZING),
  osd_tp(cct, "OSD::osd_tp", cct->_conf->osd_op_threads, "osd_op_threads"),
  osd_op_tp(cct, "OSD::osd_op_tp", 
    cct->_conf->osd_op_num_threads_per_shard * cct->_conf->osd_op_num_shards),
  recovery_tp(cct, "OSD::recovery_tp", cct->_conf->osd_recovery_threads, "osd_recovery_threads"),
  disk_tp(cct, "OSD::disk_tp", cct->_conf->osd_disk_threads, "osd_disk_threads"),
  command_tp(cct, "OSD::command_tp", 1),
  paused_recovery(false),
  session_waiting_lock("OSD::session_waiting_lock"),
  heartbeat_lock("OSD::heartbeat_lock"),
  heartbeat_stop(false), heartbeat_update_lock("OSD::heartbeat_update_lock"),
  heartbeat_need_update(true), heartbeat_epoch(0),
  hbclient_messenger(hb_clientm),
  hb_front_server_messenger(hb_front_serverm),
  hb_back_server_messenger(hb_back_serverm),
  heartbeat_thread(this),
  heartbeat_dispatcher(this),
  finished_lock("OSD::finished_lock"),
  op_tracker(cct, cct->_conf->osd_enable_op_tracker, 
                  cct->_conf->osd_num_op_tracker_shard),
  test_ops_hook(NULL),
  op_shardedwq(cct->_conf->osd_op_num_shards, this, 
    cct->_conf->osd_op_thread_timeout, &osd_op_tp),
  peering_wq(this, cct->_conf->osd_op_thread_timeout, &osd_tp),
  map_lock("OSD::map_lock"),
  pg_map_lock("OSD::pg_map_lock"),
  debug_drop_pg_create_probability(cct->_conf->osd_debug_drop_pg_create_probability),
  debug_drop_pg_create_duration(cct->_conf->osd_debug_drop_pg_create_duration),
  debug_drop_pg_create_left(-1),
  outstanding_pg_stats(false),
  timeout_mon_on_pg_stats(true),
  up_thru_wanted(0), up_thru_pending(0),
  pg_stat_queue_lock("OSD::pg_stat_queue_lock"),
  osd_stat_updated(false),
  pg_stat_tid(0), pg_stat_tid_flushed(0),
  command_wq(this, cct->_conf->osd_command_thread_timeout, &command_tp),
  recovery_ops_active(0),
  recovery_wq(this, cct->_conf->osd_recovery_thread_timeout, &recovery_tp),
  replay_queue_lock("OSD::replay_queue_lock"),
  snap_trim_wq(this, cct->_conf->osd_snap_trim_thread_timeout, &disk_tp),
  scrub_wq(this, cct->_conf->osd_scrub_thread_timeout, &disk_tp),
  rep_scrub_wq(this, cct->_conf->osd_scrub_thread_timeout, &disk_tp),
  remove_wq(store, cct->_conf->osd_remove_thread_timeout, &disk_tp),
  service(this)
{
  monc->set_messenger(client_messenger);
  op_tracker.set_complaint_and_threshold(cct->_conf->osd_op_complaint_time,
                                         cct->_conf->osd_op_log_threshold);
  op_tracker.set_history_size_and_duration(cct->_conf->osd_op_history_size,
                                           cct->_conf->osd_op_history_duration);
}

OSD::~OSD()
{
  delete authorize_handler_cluster_registry;
  delete authorize_handler_service_registry;
  delete class_handler;
  cct->get_perfcounters_collection()->remove(recoverystate_perf);
  cct->get_perfcounters_collection()->remove(logger);
  delete recoverystate_perf;
  delete logger;
  delete store;
}

void cls_initialize(ClassHandler *ch);

void OSD::handle_signal(int signum)
{
  assert(signum == SIGINT || signum == SIGTERM);
  derr << "*** Got signal " << sys_siglist[signum] << " ***" << dendl;
  //suicide(128 + signum);
  shutdown();
}

int OSD::pre_init()
{
  Mutex::Locker lock(osd_lock);
  if (is_stopping())
    return 0;
  
  if (store->test_mount_in_use()) {
    derr << "OSD::pre_init: object store '" << dev_path << "' is "
         << "currently in use. (Is ceph-osd already running?)" << dendl;
    return -EBUSY;
  }

  cct->_conf->add_observer(this);
  return 0;
}

// asok

class OSDSocketHook : public AdminSocketHook {
  OSD *osd;
public:
  OSDSocketHook(OSD *o) : osd(o) {}
  bool call(std::string command, cmdmap_t& cmdmap, std::string format,
	    bufferlist& out) {
    stringstream ss;
    bool r = osd->asok_command(command, cmdmap, format, ss);
    out.append(ss);
    return r;
  }
};

bool OSD::asok_command(string command, cmdmap_t& cmdmap, string format,
		       ostream& ss)
{
  Formatter *f = Formatter::create(format, "json-pretty", "json-pretty");
  if (command == "status") {
    f->open_object_section("status");
    f->dump_stream("cluster_fsid") << superblock.cluster_fsid;
    f->dump_stream("osd_fsid") << superblock.osd_fsid;
    f->dump_unsigned("whoami", superblock.whoami);
    f->dump_string("state", get_state_name(get_state()));
    f->dump_unsigned("oldest_map", superblock.oldest_map);
    f->dump_unsigned("newest_map", superblock.newest_map);
    {
      RWLock::RLocker l(pg_map_lock);
      f->dump_unsigned("num_pgs", pg_map.size());
    }
    f->close_section();
  } else if (command == "flush_journal") {
    store->sync_and_flush();
  } else if (command == "dump_ops_in_flight" ||
	     command == "ops") {
    op_tracker.dump_ops_in_flight(f);
  } else if (command == "dump_historic_ops") {
    op_tracker.dump_historic_ops(f);
  } else if (command == "dump_op_pq_state") {
    f->open_object_section("pq");
    op_shardedwq.dump(f);
    f->close_section();
  } else if (command == "dump_blacklist") {
    list<pair<entity_addr_t,utime_t> > bl;
    OSDMapRef curmap = service.get_osdmap();

    f->open_array_section("blacklist");
    curmap->get_blacklist(&bl);
    for (list<pair<entity_addr_t,utime_t> >::iterator it = bl.begin();
	it != bl.end(); ++it) {
      f->open_array_section("entry");
      f->open_object_section("entity_addr_t");
      it->first.dump(f);
      f->close_section(); //entity_addr_t
      it->second.localtime(f->dump_stream("expire_time"));
      f->close_section(); //entry
    }
    f->close_section(); //blacklist
  } else if (command == "dump_watchers") {
    list<obj_watch_item_t> watchers;
    // scan pg's
    {
      Mutex::Locker l(osd_lock);
      RWLock::RLocker l2(pg_map_lock);
      for (ceph::unordered_map<spg_t,PG*>::iterator it = pg_map.begin();
          it != pg_map.end();
          ++it) {

        list<obj_watch_item_t> pg_watchers;
        PG *pg = it->second;
        pg->lock();
        pg->get_watchers(pg_watchers);
        pg->unlock();
        watchers.splice(watchers.end(), pg_watchers);
      }
    }

    f->open_array_section("watchers");
    for (list<obj_watch_item_t>::iterator it = watchers.begin();
	it != watchers.end(); ++it) {

      f->open_array_section("watch");

      f->dump_string("namespace", it->obj.nspace);
      f->dump_string("object", it->obj.oid.name);

      f->open_object_section("entity_name");
      it->wi.name.dump(f);
      f->close_section(); //entity_name_t

      f->dump_int("cookie", it->wi.cookie);
      f->dump_int("timeout", it->wi.timeout_seconds);

      f->open_object_section("entity_addr_t");
      it->wi.addr.dump(f);
      f->close_section(); //entity_addr_t

      f->close_section(); //watch
    }

    f->close_section(); //watches
  } else if (command == "dump_reservations") {
    f->open_object_section("reservations");
    f->open_object_section("local_reservations");
    service.local_reserver.dump(f);
    f->close_section();
    f->open_object_section("remote_reservations");
    service.remote_reserver.dump(f);
    f->close_section();
    f->close_section();
  } else if (command == "get_latest_osdmap") {
    get_latest_osdmap();
  } else {
    assert(0 == "broken asok registration");
  }
  f->flush(ss);
  delete f;
  return true;
}

class TestOpsSocketHook : public AdminSocketHook {
  OSDService *service;
  ObjectStore *store;
public:
  TestOpsSocketHook(OSDService *s, ObjectStore *st) : service(s), store(st) {}
  bool call(std::string command, cmdmap_t& cmdmap, std::string format,
	    bufferlist& out) {
    stringstream ss;
    test_ops(service, store, command, cmdmap, ss);
    out.append(ss);
    return true;
  }
  void test_ops(OSDService *service, ObjectStore *store, std::string command,
     cmdmap_t& cmdmap, ostream &ss);

};

int OSD::init()
{
  CompatSet initial, diff;
  Mutex::Locker lock(osd_lock);
  if (is_stopping())
    return 0;

  tick_timer.init();
  service.backfill_request_timer.init();

  // mount.
  dout(2) << "mounting " << dev_path << " "
	  << (journal_path.empty() ? "(no journal)" : journal_path) << dendl;
  assert(store);  // call pre_init() first!

  int r = store->mount();
  if (r < 0) {
    derr << "OSD:init: unable to mount object store" << dendl;
    return r;
  }

  dout(2) << "boot" << dendl;

  // read superblock
  r = read_superblock();
  if (r < 0) {
    derr << "OSD::init() : unable to read osd superblock" << dendl;
    r = -EINVAL;
    goto out;
  }

  if (osd_compat.compare(superblock.compat_features) < 0) {
    derr << "The disk uses features unsupported by the executable." << dendl;
    derr << " ondisk features " << superblock.compat_features << dendl;
    derr << " daemon features " << osd_compat << dendl;

    if (osd_compat.writeable(superblock.compat_features)) {
      CompatSet diff = osd_compat.unsupported(superblock.compat_features);
      derr << "it is still writeable, though. Missing features: " << diff << dendl;
      r = -EOPNOTSUPP;
      goto out;
    }
    else {
      CompatSet diff = osd_compat.unsupported(superblock.compat_features);
      derr << "Cannot write to disk! Missing features: " << diff << dendl;
      r = -EOPNOTSUPP;
      goto out;
    }
  }

  assert_warn(whoami == superblock.whoami);
  if (whoami != superblock.whoami) {
    derr << "OSD::init: superblock says osd"
	 << superblock.whoami << " but i am osd." << whoami << dendl;
    r = -EINVAL;
    goto out;
  }

  initial = get_osd_initial_compat_set();
  diff = superblock.compat_features.unsupported(initial);
  if (superblock.compat_features.merge(initial)) {
    // We need to persist the new compat_set before we
    // do anything else
    dout(5) << "Upgrading superblock adding: " << diff << dendl;
    ObjectStore::Transaction t;
    write_superblock(t);
    r = store->apply_transaction(t);
    if (r < 0)
      goto out;
  }

  // make sure snap mapper object exists
  if (!store->exists(META_COLL, OSD::make_snapmapper_oid())) {
    dout(10) << "init creating/touching snapmapper object" << dendl;
    ObjectStore::Transaction t;
    t.touch(META_COLL, OSD::make_snapmapper_oid());
    r = store->apply_transaction(t);
    if (r < 0)
      goto out;
  }

  class_handler = new ClassHandler(cct);
  cls_initialize(class_handler);

  if (cct->_conf->osd_open_classes_on_start) {
    int r = class_handler->open_all_classes();
    if (r)
      dout(1) << "warning: got an error loading one or more classes: " << cpp_strerror(r) << dendl;
  }

  // load up "current" osdmap
  assert_warn(!osdmap);
  if (osdmap) {
    derr << "OSD::init: unable to read current osdmap" << dendl;
    r = -EINVAL;
    goto out;
  }
  osdmap = get_map(superblock.current_epoch);
  check_osdmap_features(store);

  create_recoverystate_perf();

  {
    epoch_t bind_epoch = osdmap->get_epoch();
    service.set_epochs(NULL, NULL, &bind_epoch);
  }

  // load up pgs (as they previously existed)
  load_pgs();

  dout(2) << "superblock: i am osd." << superblock.whoami << dendl;

  create_logger();

  // i'm ready!
  client_messenger->add_dispatcher_head(this);
  cluster_messenger->add_dispatcher_head(this);

  hbclient_messenger->add_dispatcher_head(&heartbeat_dispatcher);
  hb_front_server_messenger->add_dispatcher_head(&heartbeat_dispatcher);
  hb_back_server_messenger->add_dispatcher_head(&heartbeat_dispatcher);

  objecter_messenger->add_dispatcher_head(service.objecter);

  monc->set_want_keys(CEPH_ENTITY_TYPE_MON | CEPH_ENTITY_TYPE_OSD);
  r = monc->init();
  if (r < 0)
    goto out;

  // tell monc about log_client so it will know about mon session resets
  monc->set_log_client(&log_client);
  update_log_config();

  osd_tp.start();
  osd_op_tp.start();
  recovery_tp.start();
  disk_tp.start();
  command_tp.start();

  set_disk_tp_priority();

  // start the heartbeat
  heartbeat_thread.create();

  // tick
  tick_timer.add_event_after(cct->_conf->osd_heartbeat_interval, new C_Tick(this));

  service.init();
  service.publish_map(osdmap);
  service.publish_superblock(superblock);

  osd_lock.Unlock();

  r = monc->authenticate();
  if (r < 0) {
    osd_lock.Lock(); // locker is going to unlock this on function exit
    if (is_stopping())
      r =  0;
    goto monout;
  }

  while (monc->wait_auth_rotating(30.0) < 0) {
    derr << "unable to obtain rotating service keys; retrying" << dendl;
  }

  osd_lock.Lock();
  if (is_stopping())
    return 0;

  check_config();

  dout(10) << "ensuring pgs have consumed prior maps" << dendl;
  consume_map();
  peering_wq.drain();

  dout(0) << "done with init, starting boot process" << dendl;
  set_state(STATE_BOOTING);
  start_boot();

  return 0;
monout:
  monc->shutdown();

out:
  store->umount();
  delete store;
  return r;
}

void OSD::final_init()
{
  int r;
  AdminSocket *admin_socket = cct->get_admin_socket();
  asok_hook = new OSDSocketHook(this);
  r = admin_socket->register_command("status", "status", asok_hook,
				     "high-level status of OSD");
  assert(r == 0);
  r = admin_socket->register_command("flush_journal", "flush_journal",
                                     asok_hook,
                                     "flush the journal to permanent store");
  assert(r == 0);
  r = admin_socket->register_command("dump_ops_in_flight",
				     "dump_ops_in_flight", asok_hook,
				     "show the ops currently in flight");
  assert(r == 0);
  r = admin_socket->register_command("ops",
				     "ops", asok_hook,
				     "show the ops currently in flight");
  assert(r == 0);
  r = admin_socket->register_command("dump_historic_ops", "dump_historic_ops",
				     asok_hook,
				     "show slowest recent ops");
  assert(r == 0);
  r = admin_socket->register_command("dump_op_pq_state", "dump_op_pq_state",
				     asok_hook,
				     "dump op priority queue state");
  assert(r == 0);
  r = admin_socket->register_command("dump_blacklist", "dump_blacklist",
				     asok_hook,
				     "dump blacklisted clients and times");
  assert(r == 0);
  r = admin_socket->register_command("dump_watchers", "dump_watchers",
				     asok_hook,
				     "show clients which have active watches,"
				     " and on which objects");
  assert(r == 0);
  r = admin_socket->register_command("dump_reservations", "dump_reservations",
				     asok_hook,
				     "show recovery reservations");
  assert(r == 0);
  r = admin_socket->register_command("get_latest_osdmap", "get_latest_osdmap",
				     asok_hook,
				     "force osd to update the latest map from "
				     "the mon");
  assert(r == 0);

  test_ops_hook = new TestOpsSocketHook(&(this->service), this->store);
  // Note: pools are CephString instead of CephPoolname because
  // these commands traditionally support both pool names and numbers
  r = admin_socket->register_command(
   "setomapval",
   "setomapval " \
   "name=pool,type=CephString " \
   "name=objname,type=CephObjectname " \
   "name=key,type=CephString "\
   "name=val,type=CephString",
   test_ops_hook,
   "set omap key");
  assert(r == 0);
  r = admin_socket->register_command(
    "rmomapkey",
    "rmomapkey " \
    "name=pool,type=CephString " \
    "name=objname,type=CephObjectname " \
    "name=key,type=CephString",
    test_ops_hook,
    "remove omap key");
  assert(r == 0);
  r = admin_socket->register_command(
    "setomapheader",
    "setomapheader " \
    "name=pool,type=CephString " \
    "name=objname,type=CephObjectname " \
    "name=header,type=CephString",
    test_ops_hook,
    "set omap header");
  assert(r == 0);

  r = admin_socket->register_command(
    "getomap",
    "getomap " \
    "name=pool,type=CephString " \
    "name=objname,type=CephObjectname",
    test_ops_hook,
    "output entire object map");
  assert(r == 0);

  r = admin_socket->register_command(
    "truncobj",
    "truncobj " \
    "name=pool,type=CephString " \
    "name=objname,type=CephObjectname " \
    "name=len,type=CephInt",
    test_ops_hook,
    "truncate object to length");
  assert(r == 0);

  r = admin_socket->register_command(
    "injectdataerr",
    "injectdataerr " \
    "name=pool,type=CephString " \
    "name=objname,type=CephObjectname",
    test_ops_hook,
    "inject data error into omap");
  assert(r == 0);

  r = admin_socket->register_command(
    "injectmdataerr",
    "injectmdataerr " \
    "name=pool,type=CephString " \
    "name=objname,type=CephObjectname",
    test_ops_hook,
    "inject metadata error");
  assert(r == 0);
}

void OSD::create_logger()
{
  dout(10) << "create_logger" << dendl;

  PerfCountersBuilder osd_plb(cct, "osd", l_osd_first, l_osd_last);

  osd_plb.add_u64(l_osd_opq, "opq");       // op queue length (waiting to be processed yet)
  osd_plb.add_u64(l_osd_op_wip, "op_wip");   // rep ops currently being processed (primary)

  osd_plb.add_u64_counter(l_osd_op,       "op");           // client ops
  osd_plb.add_u64_counter(l_osd_op_inb,   "op_in_bytes");       // client op in bytes (writes)
  osd_plb.add_u64_counter(l_osd_op_outb,  "op_out_bytes");      // client op out bytes (reads)
  osd_plb.add_time_avg(l_osd_op_lat,   "op_latency");       // client op latency
  osd_plb.add_time_avg(l_osd_op_process_lat, "op_process_latency");   // client op process latency

  osd_plb.add_u64_counter(l_osd_op_r,      "op_r");        // client reads
  osd_plb.add_u64_counter(l_osd_op_r_outb, "op_r_out_bytes");   // client read out bytes
  osd_plb.add_time_avg(l_osd_op_r_lat,  "op_r_latency");    // client read latency
  osd_plb.add_time_avg(l_osd_op_r_process_lat, "op_r_process_latency");   // client read process latency
  osd_plb.add_u64_counter(l_osd_op_w,      "op_w");        // client writes
  osd_plb.add_u64_counter(l_osd_op_w_inb,  "op_w_in_bytes");    // client write in bytes
  osd_plb.add_time_avg(l_osd_op_w_rlat, "op_w_rlat");   // client write readable/applied latency
  osd_plb.add_time_avg(l_osd_op_w_lat,  "op_w_latency");    // client write latency
  osd_plb.add_time_avg(l_osd_op_w_process_lat, "op_w_process_latency");   // client write process latency
  osd_plb.add_u64_counter(l_osd_op_rw,     "op_rw");       // client rmw
  osd_plb.add_u64_counter(l_osd_op_rw_inb, "op_rw_in_bytes");   // client rmw in bytes
  osd_plb.add_u64_counter(l_osd_op_rw_outb,"op_rw_out_bytes");  // client rmw out bytes
  osd_plb.add_time_avg(l_osd_op_rw_rlat,"op_rw_rlat");  // client rmw readable/applied latency
  osd_plb.add_time_avg(l_osd_op_rw_lat, "op_rw_latency");   // client rmw latency
  osd_plb.add_time_avg(l_osd_op_rw_process_lat, "op_rw_process_latency");   // client rmw process latency

  osd_plb.add_u64_counter(l_osd_sop,       "subop");         // subops
  osd_plb.add_u64_counter(l_osd_sop_inb,   "subop_in_bytes");     // subop in bytes
  osd_plb.add_time_avg(l_osd_sop_lat,   "subop_latency");     // subop latency

  osd_plb.add_u64_counter(l_osd_sop_w,     "subop_w");          // replicated (client) writes
  osd_plb.add_u64_counter(l_osd_sop_w_inb, "subop_w_in_bytes");      // replicated write in bytes
  osd_plb.add_time_avg(l_osd_sop_w_lat, "subop_w_latency");      // replicated write latency
  osd_plb.add_u64_counter(l_osd_sop_pull,     "subop_pull");       // pull request
  osd_plb.add_time_avg(l_osd_sop_pull_lat, "subop_pull_latency");
  osd_plb.add_u64_counter(l_osd_sop_push,     "subop_push");       // push (write)
  osd_plb.add_u64_counter(l_osd_sop_push_inb, "subop_push_in_bytes");
  osd_plb.add_time_avg(l_osd_sop_push_lat, "subop_push_latency");

  osd_plb.add_u64_counter(l_osd_pull,      "pull");       // pull requests sent
  osd_plb.add_u64_counter(l_osd_push,      "push");       // push messages
  osd_plb.add_u64_counter(l_osd_push_outb, "push_out_bytes");  // pushed bytes

  osd_plb.add_u64_counter(l_osd_push_in,    "push_in");        // inbound push messages
  osd_plb.add_u64_counter(l_osd_push_inb,   "push_in_bytes");  // inbound pushed bytes

  osd_plb.add_u64_counter(l_osd_rop, "recovery_ops");       // recovery ops (started)

  osd_plb.add_u64(l_osd_loadavg, "loadavg");
  osd_plb.add_u64(l_osd_buf, "buffer_bytes");       // total ceph::buffer bytes

  osd_plb.add_u64(l_osd_pg, "numpg");   // num pgs
  osd_plb.add_u64(l_osd_pg_primary, "numpg_primary"); // num primary pgs
  osd_plb.add_u64(l_osd_pg_replica, "numpg_replica"); // num replica pgs
  osd_plb.add_u64(l_osd_pg_stray, "numpg_stray");   // num stray pgs
  osd_plb.add_u64(l_osd_hb_to, "heartbeat_to_peers");     // heartbeat peers we send to
  osd_plb.add_u64(l_osd_hb_from, "heartbeat_from_peers"); // heartbeat peers we recv from
  osd_plb.add_u64_counter(l_osd_map, "map_messages");           // osdmap messages
  osd_plb.add_u64_counter(l_osd_mape, "map_message_epochs");         // osdmap epochs
  osd_plb.add_u64_counter(l_osd_mape_dup, "map_message_epoch_dups"); // dup osdmap epochs
  osd_plb.add_u64_counter(l_osd_waiting_for_map,
			  "messages_delayed_for_map"); // dup osdmap epochs

  osd_plb.add_u64(l_osd_stat_bytes, "stat_bytes");
  osd_plb.add_u64(l_osd_stat_bytes_used, "stat_bytes_used");
  osd_plb.add_u64(l_osd_stat_bytes_avail, "stat_bytes_avail");

  osd_plb.add_u64_counter(l_osd_copyfrom, "copyfrom");

  osd_plb.add_u64_counter(l_osd_tier_promote, "tier_promote");
  osd_plb.add_u64_counter(l_osd_tier_flush, "tier_flush");
  osd_plb.add_u64_counter(l_osd_tier_flush_fail, "tier_flush_fail");
  osd_plb.add_u64_counter(l_osd_tier_try_flush, "tier_try_flush");
  osd_plb.add_u64_counter(l_osd_tier_try_flush_fail, "tier_try_flush_fail");
  osd_plb.add_u64_counter(l_osd_tier_evict, "tier_evict");
  osd_plb.add_u64_counter(l_osd_tier_whiteout, "tier_whiteout");
  osd_plb.add_u64_counter(l_osd_tier_dirty, "tier_dirty");
  osd_plb.add_u64_counter(l_osd_tier_clean, "tier_clean");
  osd_plb.add_u64_counter(l_osd_tier_delay, "tier_delay");

  osd_plb.add_u64_counter(l_osd_agent_wake, "agent_wake");
  osd_plb.add_u64_counter(l_osd_agent_skip, "agent_skip");
  osd_plb.add_u64_counter(l_osd_agent_flush, "agent_flush");
  osd_plb.add_u64_counter(l_osd_agent_evict, "agent_evict");

  logger = osd_plb.create_perf_counters();
  cct->get_perfcounters_collection()->add(logger);
}

void OSD::create_recoverystate_perf()
{
  dout(10) << "create_recoverystate_perf" << dendl;

  PerfCountersBuilder rs_perf(cct, "recoverystate_perf", rs_first, rs_last);

  rs_perf.add_time_avg(rs_initial_latency, "initial_latency");
  rs_perf.add_time_avg(rs_started_latency, "started_latency");
  rs_perf.add_time_avg(rs_reset_latency, "reset_latency");
  rs_perf.add_time_avg(rs_start_latency, "start_latency");
  rs_perf.add_time_avg(rs_primary_latency, "primary_latency");
  rs_perf.add_time_avg(rs_peering_latency, "peering_latency");
  rs_perf.add_time_avg(rs_backfilling_latency, "backfilling_latency");
  rs_perf.add_time_avg(rs_waitremotebackfillreserved_latency, "waitremotebackfillreserved_latency");
  rs_perf.add_time_avg(rs_waitlocalbackfillreserved_latency, "waitlocalbackfillreserved_latency");
  rs_perf.add_time_avg(rs_notbackfilling_latency, "notbackfilling_latency");
  rs_perf.add_time_avg(rs_repnotrecovering_latency, "repnotrecovering_latency");
  rs_perf.add_time_avg(rs_repwaitrecoveryreserved_latency, "repwaitrecoveryreserved_latency");
  rs_perf.add_time_avg(rs_repwaitbackfillreserved_latency, "repwaitbackfillreserved_latency");
  rs_perf.add_time_avg(rs_RepRecovering_latency, "RepRecovering_latency");
  rs_perf.add_time_avg(rs_activating_latency, "activating_latency");
  rs_perf.add_time_avg(rs_waitlocalrecoveryreserved_latency, "waitlocalrecoveryreserved_latency");
  rs_perf.add_time_avg(rs_waitremoterecoveryreserved_latency, "waitremoterecoveryreserved_latency");
  rs_perf.add_time_avg(rs_recovering_latency, "recovering_latency");
  rs_perf.add_time_avg(rs_recovered_latency, "recovered_latency");
  rs_perf.add_time_avg(rs_clean_latency, "clean_latency");
  rs_perf.add_time_avg(rs_active_latency, "active_latency");
  rs_perf.add_time_avg(rs_replicaactive_latency, "replicaactive_latency");
  rs_perf.add_time_avg(rs_stray_latency, "stray_latency");
  rs_perf.add_time_avg(rs_getinfo_latency, "getinfo_latency");
  rs_perf.add_time_avg(rs_getlog_latency, "getlog_latency");
  rs_perf.add_time_avg(rs_waitactingchange_latency, "waitactingchange_latency");
  rs_perf.add_time_avg(rs_incomplete_latency, "incomplete_latency");
  rs_perf.add_time_avg(rs_getmissing_latency, "getmissing_latency");
  rs_perf.add_time_avg(rs_waitupthru_latency, "waitupthru_latency");

  recoverystate_perf = rs_perf.create_perf_counters();
  cct->get_perfcounters_collection()->add(recoverystate_perf);
}

void OSD::suicide(int exitcode)
{
  if (cct->_conf->filestore_blackhole) {
    derr << " filestore_blackhole=true, doing abbreviated shutdown" << dendl;
    _exit(exitcode);
  }

  // turn off lockdep; the surviving threads tend to fight with exit() below
  g_lockdep = 0;

  derr << " pausing thread pools" << dendl;
  osd_tp.pause();
  osd_op_tp.pause();
  disk_tp.pause();
  recovery_tp.pause();
  command_tp.pause();

  derr << " flushing io" << dendl;
  store->sync_and_flush();

  derr << " removing pid file" << dendl;
  pidfile_remove();

  derr << " exit" << dendl;
  exit(exitcode);
}

int OSD::shutdown()
{
  if (!service.prepare_to_stop())
    return 0; // already shutting down
  osd_lock.Lock();
  if (is_stopping()) {
    osd_lock.Unlock();
    return 0;
  }
  derr << "shutdown" << dendl;

  set_state(STATE_STOPPING);

  // Debugging
  cct->_conf->set_val("debug_osd", "100");
  cct->_conf->set_val("debug_journal", "100");
  cct->_conf->set_val("debug_filestore", "100");
  cct->_conf->set_val("debug_ms", "100");
  cct->_conf->apply_changes(NULL);

  service.start_shutdown();

  clear_waiting_sessions();

  // Shutdown PGs
  {
    RWLock::RLocker l(pg_map_lock);
    for (ceph::unordered_map<spg_t, PG*>::iterator p = pg_map.begin();
        p != pg_map.end();
        ++p) {
      dout(20) << " kicking pg " << p->first << dendl;
      p->second->lock();
      p->second->on_shutdown();
      p->second->unlock();
      p->second->osr->flush();
    }
  }
  
  // finish ops
  op_shardedwq.drain(); // should already be empty except for lagard PGs
  {
    Mutex::Locker l(finished_lock);
    finished.clear(); // zap waiters (bleh, this is messy)
  }

  // unregister commands
  cct->get_admin_socket()->unregister_command("status");
  cct->get_admin_socket()->unregister_command("flush_journal");
  cct->get_admin_socket()->unregister_command("dump_ops_in_flight");
  cct->get_admin_socket()->unregister_command("ops");
  cct->get_admin_socket()->unregister_command("dump_historic_ops");
  cct->get_admin_socket()->unregister_command("dump_op_pq_state");
  cct->get_admin_socket()->unregister_command("dump_blacklist");
  cct->get_admin_socket()->unregister_command("dump_watchers");
  cct->get_admin_socket()->unregister_command("dump_reservations");
  cct->get_admin_socket()->unregister_command("get_latest_osdmap");
  delete asok_hook;
  asok_hook = NULL;

  cct->get_admin_socket()->unregister_command("setomapval");
  cct->get_admin_socket()->unregister_command("rmomapkey");
  cct->get_admin_socket()->unregister_command("setomapheader");
  cct->get_admin_socket()->unregister_command("getomap");
  cct->get_admin_socket()->unregister_command("truncobj");
  cct->get_admin_socket()->unregister_command("injectdataerr");
  cct->get_admin_socket()->unregister_command("injectmdataerr");
  delete test_ops_hook;
  test_ops_hook = NULL;

  osd_lock.Unlock();

  heartbeat_lock.Lock();
  heartbeat_stop = true;
  heartbeat_cond.Signal();
  heartbeat_lock.Unlock();
  heartbeat_thread.join();

  recovery_tp.drain();
  recovery_tp.stop();
  dout(10) << "recovery tp stopped" << dendl;

  osd_tp.drain();
  peering_wq.clear();
  osd_tp.stop();
  dout(10) << "osd tp stopped" << dendl;

  osd_op_tp.drain();
  osd_op_tp.stop();
  dout(10) << "op sharded tp stopped" << dendl;

  command_tp.drain();
  command_tp.stop();
  dout(10) << "command tp stopped" << dendl;

  disk_tp.drain();
  disk_tp.stop();
  dout(10) << "disk tp paused (new)" << dendl;

  dout(10) << "stopping agent" << dendl;
  service.agent_stop();

  osd_lock.Lock();

  reset_heartbeat_peers();

  tick_timer.shutdown();

  // note unmount epoch
  dout(10) << "noting clean unmount in epoch " << osdmap->get_epoch() << dendl;
  superblock.mounted = service.get_boot_epoch();
  superblock.clean_thru = osdmap->get_epoch();
  ObjectStore::Transaction t;
  write_superblock(t);
  int r = store->apply_transaction(t);
  if (r) {
    derr << "OSD::shutdown: error writing superblock: "
	 << cpp_strerror(r) << dendl;
  }

  dout(10) << "syncing store" << dendl;
  store->flush();
  store->sync();
  store->umount();
  delete store;
  store = 0;
  dout(10) << "Store synced" << dendl;

  {
    Mutex::Locker l(pg_stat_queue_lock);
    assert(pg_stat_queue.empty());
  }

  // Remove PGs
#ifdef PG_DEBUG_REFS
  service.dump_live_pgids();
#endif
  {
    RWLock::RLocker l(pg_map_lock);
    for (ceph::unordered_map<spg_t, PG*>::iterator p = pg_map.begin();
        p != pg_map.end();
        ++p) {
      dout(20) << " kicking pg " << p->first << dendl;
      p->second->lock();
      if (p->second->ref.read() != 1) {
        derr << "pgid " << p->first << " has ref count of "
            << p->second->ref.read() << dendl;
        assert(0);
      }
      p->second->unlock();
      p->second->put("PGMap");
    }
    pg_map.clear();
  }
#ifdef PG_DEBUG_REFS
  service.dump_live_pgids();
#endif
  cct->_conf->remove_observer(this);

  monc->shutdown();
  osd_lock.Unlock();

  osdmap = OSDMapRef();
  service.shutdown();
  op_tracker.on_shutdown();

  class_handler->shutdown();
  client_messenger->shutdown();
  cluster_messenger->shutdown();
  hbclient_messenger->shutdown();
  objecter_messenger->shutdown();
  hb_front_server_messenger->shutdown();
  hb_back_server_messenger->shutdown();

  peering_wq.clear();

  return r;
}

void OSD::write_superblock(ObjectStore::Transaction& t)
{
  dout(10) << "write_superblock " << superblock << dendl;

  //hack: at minimum it's using the baseline feature set
  if (!superblock.compat_features.incompat.mask |
      CEPH_OSD_FEATURE_INCOMPAT_BASE.id)
    superblock.compat_features.incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_BASE);

  bufferlist bl;
  ::encode(superblock, bl);
  t.write(META_COLL, OSD_SUPERBLOCK_POBJECT, 0, bl.length(), bl);
}

int OSD::read_superblock()
{
  bufferlist bl;
  int r = store->read(META_COLL, OSD_SUPERBLOCK_POBJECT, 0, 0, bl);
  if (r < 0)
    return r;

  bufferlist::iterator p = bl.begin();
  ::decode(superblock, p);

  dout(10) << "read_superblock " << superblock << dendl;
  
  return 0;
}



void OSD::recursive_remove_collection(ObjectStore *store, coll_t tmp)
{
  OSDriver driver(
    store,
    coll_t(),
    make_snapmapper_oid());

  spg_t pg;
  tmp.is_pg_prefix(pg);

  ObjectStore::Transaction t;
  SnapMapper mapper(&driver, 0, 0, 0, pg.shard);

  vector<ghobject_t> objects;
  store->collection_list(tmp, objects);

  // delete them.
  unsigned removed = 0;
  for (vector<ghobject_t>::iterator p = objects.begin();
       p != objects.end();
       ++p, removed++) {
    OSDriver::OSTransaction _t(driver.get_transaction(&t));
    int r = mapper.remove_oid(p->hobj, &_t);
    if (r != 0 && r != -ENOENT)
      assert(0);
    t.remove(tmp, *p);
    if (removed > 300) {
      int r = store->apply_transaction(t);
      assert(r == 0);
      t = ObjectStore::Transaction();
      removed = 0;
    }
  }
  t.remove_collection(tmp);
  int r = store->apply_transaction(t);
  assert(r == 0);
  store->sync_and_flush();
}


// ======================================================
// PG's

PGPool OSD::_get_pool(int id, OSDMapRef createmap)
{
  if (!createmap->have_pg_pool(id)) {
    dout(5) << __func__ << ": the OSDmap does not contain a PG pool with id = "
	    << id << dendl;
    assert(0);
  }

  PGPool p = PGPool(id, createmap->get_pool_name(id),
		    createmap->get_pg_pool(id)->auid);
    
  const pg_pool_t *pi = createmap->get_pg_pool(id);
  p.info = *pi;
  p.snapc = pi->get_snap_context();

  pi->build_removed_snaps(p.cached_removed_snaps);
  dout(10) << "_get_pool " << p.id << dendl;
  return p;
}

PG *OSD::_open_lock_pg(
  OSDMapRef createmap,
  spg_t pgid, bool no_lockdep_check)
{
  assert(osd_lock.is_locked());

  PG* pg = _make_pg(createmap, pgid);
  {
    RWLock::WLocker l(pg_map_lock);
    pg->lock(no_lockdep_check);
    pg_map[pgid] = pg;
    pg->get("PGMap");  // because it's in pg_map
    service.pg_add_epoch(pg->info.pgid, createmap->get_epoch());
  }
  return pg;
}

PG* OSD::_make_pg(
  OSDMapRef createmap,
  spg_t pgid)
{
  dout(10) << "_open_lock_pg " << pgid << dendl;
  PGPool pool = _get_pool(pgid.pool(), createmap);

  // create
  PG *pg;
  if (createmap->get_pg_type(pgid.pgid) == pg_pool_t::TYPE_REPLICATED ||
      createmap->get_pg_type(pgid.pgid) == pg_pool_t::TYPE_ERASURE)
    pg = new ReplicatedPG(&service, createmap, pool, pgid);
  else 
    assert(0);

  return pg;
}


void OSD::add_newly_split_pg(PG *pg, PG::RecoveryCtx *rctx)
{
  epoch_t e(service.get_osdmap()->get_epoch());
  pg->get("PGMap");  // For pg_map
  pg_map[pg->info.pgid] = pg;
  service.pg_add_epoch(pg->info.pgid, pg->get_osdmap()->get_epoch());

  dout(10) << "Adding newly split pg " << *pg << dendl;
  vector<int> up, acting;
  pg->get_osdmap()->pg_to_up_acting_osds(pg->info.pgid.pgid, up, acting);
  int role = OSDMap::calc_pg_role(service.whoami, acting);
  pg->set_role(role);
  pg->reg_next_scrub();
  pg->handle_loaded(rctx);
  pg->write_if_dirty(*(rctx->transaction));
  pg->queue_null(e, e);
  map<spg_t, list<PG::CephPeeringEvtRef> >::iterator to_wake =
    peering_wait_for_split.find(pg->info.pgid);
  if (to_wake != peering_wait_for_split.end()) {
    for (list<PG::CephPeeringEvtRef>::iterator i =
	   to_wake->second.begin();
	 i != to_wake->second.end();
	 ++i) {
      pg->queue_peering_event(*i);
    }
    peering_wait_for_split.erase(to_wake);
  }
  if (!service.get_osdmap()->have_pg_pool(pg->info.pgid.pool()))
    _remove_pg(pg);
}

OSD::res_result OSD::_try_resurrect_pg(
  OSDMapRef curmap, spg_t pgid, spg_t *resurrected, PGRef *old_pg_state)
{
  assert(resurrected);
  assert(old_pg_state);
  // find nearest ancestor
  DeletingStateRef df;
  spg_t cur(pgid);
  while (true) {
    df = service.deleting_pgs.lookup(cur);
    if (df)
      break;
    if (!cur.ps())
      break;
    cur = cur.get_parent();
  }
  if (!df)
    return RES_NONE; // good to go

  df->old_pg_state->lock();
  OSDMapRef create_map = df->old_pg_state->get_osdmap();
  df->old_pg_state->unlock();

  set<spg_t> children;
  if (cur == pgid) {
    if (df->try_stop_deletion()) {
      dout(10) << __func__ << ": halted deletion on pg " << pgid << dendl;
      *resurrected = cur;
      *old_pg_state = df->old_pg_state;
      service.deleting_pgs.remove(pgid); // PG is no longer being removed!
      return RES_SELF;
    } else {
      // raced, ensure we don't see DeletingStateRef when we try to
      // delete this pg
      service.deleting_pgs.remove(pgid);
      return RES_NONE;
    }
  } else if (cur.is_split(create_map->get_pg_num(cur.pool()),
			  curmap->get_pg_num(cur.pool()),
			  &children) &&
	     children.count(pgid)) {
    if (df->try_stop_deletion()) {
      dout(10) << __func__ << ": halted deletion on ancestor pg " << pgid
	       << dendl;
      *resurrected = cur;
      *old_pg_state = df->old_pg_state;
      service.deleting_pgs.remove(cur); // PG is no longer being removed!
      return RES_PARENT;
    } else {
      /* this is not a problem, failing to cancel proves that all objects
       * have been removed, so no hobject_t overlap is possible
       */
      return RES_NONE;
    }
  }
  return RES_NONE;
}

PG *OSD::_create_lock_pg(
  OSDMapRef createmap,
  spg_t pgid,
  bool newly_created,
  bool hold_map_lock,
  bool backfill,
  int role,
  vector<int>& up, int up_primary,
  vector<int>& acting, int acting_primary,
  pg_history_t history,
  pg_interval_map_t& pi,
  ObjectStore::Transaction& t)
{
  assert(osd_lock.is_locked());
  dout(20) << "_create_lock_pg pgid " << pgid << dendl;

  PG *pg = _open_lock_pg(createmap, pgid, true);

  service.init_splits_between(pgid, pg->get_osdmap(), service.get_osdmap());

  pg->init(
    role,
    up,
    up_primary,
    acting,
    acting_primary,
    history,
    pi,
    backfill,
    &t);

  dout(7) << "_create_lock_pg " << *pg << dendl;
  return pg;
}

PG *OSD::get_pg_or_queue_for_pg(const spg_t& pgid, OpRequestRef& op)
{
  Session *session = static_cast<Session*>(
    op->get_req()->get_connection()->get_priv());
  if (!session)
    return NULL;
  // get_pg_or_queue_for_pg is only called from the fast_dispatch path where
  // the session_dispatch_lock must already be held.
  assert(session->session_dispatch_lock.is_locked());
  RWLock::RLocker l(pg_map_lock);

  ceph::unordered_map<spg_t, PG*>::iterator i = pg_map.find(pgid);
  if (i == pg_map.end())
    session->waiting_for_pg[pgid];

  map<spg_t, list<OpRequestRef> >::iterator wlistiter =
    session->waiting_for_pg.find(pgid);

  PG *out = NULL;
  if (wlistiter == session->waiting_for_pg.end()) {
    out = i->second;
  } else {
    wlistiter->second.push_back(op);
    register_session_waiting_on_pg(session, pgid);
  }
  session->put();
  return out;
}

bool OSD::_have_pg(spg_t pgid)
{
  assert(osd_lock.is_locked());
  RWLock::RLocker l(pg_map_lock);
  return pg_map.count(pgid);
}

PG *OSD::_lookup_lock_pg(spg_t pgid)
{
  assert(osd_lock.is_locked());
  RWLock::RLocker l(pg_map_lock);
  if (!pg_map.count(pgid))
    return NULL;
  PG *pg = pg_map[pgid];
  pg->lock();
  return pg;
}


PG *OSD::_lookup_pg(spg_t pgid)
{
  assert(osd_lock.is_locked());
  RWLock::RLocker l(pg_map_lock);
  if (!pg_map.count(pgid))
    return NULL;
  PG *pg = pg_map[pgid];
  return pg;
}

PG *OSD::_lookup_lock_pg_with_map_lock_held(spg_t pgid)
{
  assert(osd_lock.is_locked());
  assert(pg_map.count(pgid));
  PG *pg = pg_map[pgid];
  pg->lock();
  return pg;
}

void OSD::load_pgs()
{
  assert(osd_lock.is_locked());
  dout(0) << "load_pgs" << dendl;
  {
    RWLock::RLocker l(pg_map_lock);
    assert(pg_map.empty());
  }

  vector<coll_t> ls;
  int r = store->list_collections(ls);
  if (r < 0) {
    derr << "failed to list pgs: " << cpp_strerror(-r) << dendl;
  }

  set<spg_t> head_pgs;
  map<spg_t, interval_set<snapid_t> > pgs;
  for (vector<coll_t>::iterator it = ls.begin();
       it != ls.end();
       ++it) {
    spg_t pgid;
    snapid_t snap;
    uint64_t seq;

    if (it->is_temp(pgid) ||
	it->is_removal(&seq, &pgid) ||
	(it->is_pg(pgid, snap) &&
	 PG::_has_removal_flag(store, pgid))) {
      dout(10) << "load_pgs " << *it << " clearing temp" << dendl;
      recursive_remove_collection(store, *it);
      continue;
    }

    if (it->is_pg(pgid, snap)) {
      if (snap != CEPH_NOSNAP) {
	dout(10) << "load_pgs skipping snapped dir " << *it
		 << " (pg " << pgid << " snap " << snap << ")" << dendl;
	pgs[pgid].insert(snap);
      } else {
	pgs[pgid];
	head_pgs.insert(pgid);
      }
      continue;
    }

    dout(10) << "load_pgs ignoring unrecognized " << *it << dendl;
  }

  bool has_upgraded = false;
  for (map<spg_t, interval_set<snapid_t> >::iterator i = pgs.begin();
       i != pgs.end();
       ++i) {
    spg_t pgid(i->first);

    if (!head_pgs.count(pgid)) {
      dout(10) << __func__ << ": " << pgid << " has orphan snap collections " << i->second
	       << " with no head" << dendl;
      continue;
    }

    if (!osdmap->have_pg_pool(pgid.pool())) {
      dout(10) << __func__ << ": skipping PG " << pgid << " because we don't have pool "
	       << pgid.pool() << dendl;
      continue;
    }

    if (pgid.preferred() >= 0) {
      dout(10) << __func__ << ": skipping localized PG " << pgid << dendl;
      // FIXME: delete it too, eventually
      continue;
    }

    dout(10) << "pgid " << pgid << " coll " << coll_t(pgid) << dendl;
    bufferlist bl;
    epoch_t map_epoch = PG::peek_map_epoch(store, pgid, &bl);

    PG *pg = _open_lock_pg(map_epoch == 0 ? osdmap : service.get_map(map_epoch), pgid);
    // there can be no waiters here, so we don't call wake_pg_waiters

    // read pg state, log
    pg->read_state(store, bl);

    if (pg->must_upgrade()) {
      if (!pg->can_upgrade()) {
	derr << "PG needs upgrade, but on-disk data is too old; upgrade to"
	     << " an older version first." << dendl;
	assert(0 == "PG too old to upgrade");
      }
      if (!has_upgraded) {
	derr << "PGs are upgrading" << dendl;
	has_upgraded = true;
      }
      dout(10) << "PG " << pg->info.pgid
	       << " must upgrade..." << dendl;
      pg->upgrade(store, i->second);
    } else if (!i->second.empty()) {
      // handle upgrade bug
      for (interval_set<snapid_t>::iterator j = i->second.begin();
	   j != i->second.end();
	   ++j) {
	for (snapid_t k = j.get_start();
	     k != j.get_start() + j.get_len();
	     ++k) {
	  assert(store->collection_empty(coll_t(pgid, k)));
	  ObjectStore::Transaction t;
	  t.remove_collection(coll_t(pgid, k));
	  store->apply_transaction(t);
	}
      }
    }

    service.init_splits_between(pg->info.pgid, pg->get_osdmap(), osdmap);

    // generate state for PG's current mapping
    int primary, up_primary;
    vector<int> acting, up;
    pg->get_osdmap()->pg_to_up_acting_osds(
      pgid.pgid, &up, &up_primary, &acting, &primary);
    pg->init_primary_up_acting(
      up,
      acting,
      up_primary,
      primary);
    int role = OSDMap::calc_pg_role(whoami, pg->acting);
    pg->set_role(role);

    pg->reg_next_scrub();

    PG::RecoveryCtx rctx(0, 0, 0, 0, 0, 0);
    pg->handle_loaded(&rctx);

    dout(10) << "load_pgs loaded " << *pg << " " << pg->pg_log.get_log() << dendl;
    pg->unlock();
  }
  {
    RWLock::RLocker l(pg_map_lock);
    dout(0) << "load_pgs opened " << pg_map.size() << " pgs" << dendl;
  }

  // clean up old infos object?
  if (has_upgraded && store->exists(META_COLL, OSD::make_infos_oid())) {
    dout(1) << __func__ << " removing legacy infos object" << dendl;
    ObjectStore::Transaction t;
    t.remove(META_COLL, OSD::make_infos_oid());
    int r = store->apply_transaction(t);
    if (r != 0) {
      derr << __func__ << ": apply_transaction returned "
	   << cpp_strerror(r) << dendl;
      assert(0);
    }
  }
  
  build_past_intervals_parallel();
}


/*
 * build past_intervals efficiently on old, degraded, and buried
 * clusters.  this is important for efficiently catching up osds that
 * are way behind on maps to the current cluster state.
 *
 * this is a parallel version of PG::generate_past_intervals().
 * follow the same logic, but do all pgs at the same time so that we
 * can make a single pass across the osdmap history.
 */
struct pistate {
  epoch_t start, end;
  vector<int> old_acting, old_up;
  epoch_t same_interval_since;
  int primary;
  int up_primary;
};

void OSD::build_past_intervals_parallel()
{
  map<PG*,pistate> pis;

  // calculate untion of map range
  epoch_t end_epoch = superblock.oldest_map;
  epoch_t cur_epoch = superblock.newest_map;
  {
    RWLock::RLocker l(pg_map_lock);
    for (ceph::unordered_map<spg_t, PG*>::iterator i = pg_map.begin();
        i != pg_map.end();
        ++i) {
      PG *pg = i->second;

      epoch_t start, end;
      if (!pg->_calc_past_interval_range(&start, &end))
        continue;

      dout(10) << pg->info.pgid << " needs " << start << "-" << end << dendl;
      pistate& p = pis[pg];
      p.start = start;
      p.end = end;
      p.same_interval_since = 0;

      if (start < cur_epoch)
        cur_epoch = start;
      if (end > end_epoch)
        end_epoch = end;
    }
  }
  if (pis.empty()) {
    dout(10) << __func__ << " nothing to build" << dendl;
    return;
  }

  dout(1) << __func__ << " over " << cur_epoch << "-" << end_epoch << dendl;
  assert(cur_epoch <= end_epoch);

  OSDMapRef cur_map, last_map;
  for ( ; cur_epoch <= end_epoch; cur_epoch++) {
    dout(10) << __func__ << " epoch " << cur_epoch << dendl;
    last_map = cur_map;
    cur_map = get_map(cur_epoch);

    for (map<PG*,pistate>::iterator i = pis.begin(); i != pis.end(); ++i) {
      PG *pg = i->first;
      pistate& p = i->second;

      if (cur_epoch < p.start || cur_epoch > p.end)
	continue;

      vector<int> acting, up;
      int up_primary;
      int primary;
      cur_map->pg_to_up_acting_osds(
	pg->info.pgid.pgid, &up, &up_primary, &acting, &primary);

      if (p.same_interval_since == 0) {
	dout(10) << __func__ << " epoch " << cur_epoch << " pg " << pg->info.pgid
		 << " first map, acting " << acting
		 << " up " << up << ", same_interval_since = " << cur_epoch << dendl;
	p.same_interval_since = cur_epoch;
	p.old_up = up;
	p.old_acting = acting;
	p.primary = primary;
	p.up_primary = up_primary;
	continue;
      }
      assert(last_map);

      std::stringstream debug;
      bool new_interval = pg_interval_t::check_new_interval(
	p.primary,
	primary,
	p.old_acting, acting,
	p.up_primary,
	up_primary,
	p.old_up, up,
	p.same_interval_since,
	pg->info.history.last_epoch_clean,
	cur_map, last_map,
	pg->info.pgid.pgid,
	&pg->past_intervals,
	&debug);
      if (new_interval) {
	dout(10) << __func__ << " epoch " << cur_epoch << " pg " << pg->info.pgid
		 << " " << debug.str() << dendl;
	p.old_up = up;
	p.old_acting = acting;
	p.same_interval_since = cur_epoch;
      }
    }
  }

  // write info only at the end.  this is necessary because we check
  // whether the past_intervals go far enough back or forward in time,
  // but we don't check for holes.  we could avoid it by discarding
  // the previous past_intervals and rebuilding from scratch, or we
  // can just do this and commit all our work at the end.
  ObjectStore::Transaction t;
  int num = 0;
  for (map<PG*,pistate>::iterator i = pis.begin(); i != pis.end(); ++i) {
    PG *pg = i->first;
    pg->lock();
    pg->dirty_big_info = true;
    pg->dirty_info = true;
    pg->write_if_dirty(t);
    pg->unlock();

    // don't let the transaction get too big
    if (++num >= cct->_conf->osd_target_transaction_size) {
      store->apply_transaction(t);
      t = ObjectStore::Transaction();
      num = 0;
    }
  }
  if (!t.empty())
    store->apply_transaction(t);
}

/*
 * look up a pg.  if we have it, great.  if not, consider creating it IF the pg mapping
 * hasn't changed since the given epoch and we are the primary.
 */
void OSD::handle_pg_peering_evt(
  spg_t pgid,
  const pg_info_t& info,
  pg_interval_map_t& pi,
  epoch_t epoch,
  pg_shard_t from,
  bool primary,
  PG::CephPeeringEvtRef evt)
{
  if (service.splitting(pgid)) {
    peering_wait_for_split[pgid].push_back(evt);
    return;
  }

  if (!_have_pg(pgid)) {
    // same primary?
    if (!osdmap->have_pg_pool(pgid.pool()))
      return;
    int up_primary, acting_primary;
    vector<int> up, acting;
    osdmap->pg_to_up_acting_osds(
      pgid.pgid, &up, &up_primary, &acting, &acting_primary);
    int role = osdmap->calc_pg_role(whoami, acting, acting.size());

    pg_history_t history = info.history;
    bool valid_history = project_pg_history(
      pgid, history, epoch, up, up_primary, acting, acting_primary);

    if (!valid_history || epoch < history.same_interval_since) {
      dout(10) << "get_or_create_pg " << pgid << " acting changed in "
	       << history.same_interval_since << " (msg from " << epoch << ")" << dendl;
      return;
    }

    if (service.splitting(pgid)) {
      assert(0);
    }

    bool create = false;
    if (primary) {
      // DNE on source?
      if (info.dne()) {
	// is there a creation pending on this pg?
	if (creating_pgs.count(pgid)) {
	  creating_pgs[pgid].prior.erase(from);
	  if (!can_create_pg(pgid))
	    return;
	  history = creating_pgs[pgid].history;
	  create = true;
	} else {
	  dout(10) << "get_or_create_pg " << pgid
		   << " DNE on source, but creation probe, ignoring" << dendl;
	  return;
	}
      }
      creating_pgs.erase(pgid);
    } else {
      assert(!info.dne());  // pg exists if we are hearing about it
    }

    // do we need to resurrect a deleting pg?
    spg_t resurrected;
    PGRef old_pg_state;
    res_result result = _try_resurrect_pg(
      service.get_osdmap(),
      pgid,
      &resurrected,
      &old_pg_state);

    PG::RecoveryCtx rctx = create_context();
    switch (result) {
    case RES_NONE: {
      const pg_pool_t* pp = osdmap->get_pg_pool(pgid.pool());
      PG::_create(*rctx.transaction, pgid);
      PG::_init(*rctx.transaction, pgid, pp);

      PG *pg = _create_lock_pg(
	get_map(epoch),
	pgid, create, false, result == RES_SELF,
	role,
	up, up_primary,
	acting, acting_primary,
	history, pi,
	*rctx.transaction);
      pg->handle_create(&rctx);
      pg->write_if_dirty(*rctx.transaction);
      dispatch_context(rctx, pg, osdmap);

      dout(10) << *pg << " is new" << dendl;

      pg->queue_peering_event(evt);
      pg->unlock();
      wake_pg_waiters(pg, pgid);
      return;
    }
    case RES_SELF: {
      old_pg_state->lock();
      OSDMapRef old_osd_map = old_pg_state->get_osdmap();
      int old_role = old_pg_state->role;
      vector<int> old_up = old_pg_state->up;
      int old_up_primary = old_pg_state->up_primary.osd;
      vector<int> old_acting = old_pg_state->acting;
      int old_primary = old_pg_state->primary.osd;
      pg_history_t old_history = old_pg_state->info.history;
      pg_interval_map_t old_past_intervals = old_pg_state->past_intervals;
      old_pg_state->unlock();
      PG *pg = _create_lock_pg(
	old_osd_map,
	resurrected,
	false,
	false,
	true,
	old_role,
	old_up,
	old_up_primary,
	old_acting,
	old_primary,
	old_history,
	old_past_intervals,
	*rctx.transaction);
      pg->handle_create(&rctx);
      pg->write_if_dirty(*rctx.transaction);
      dispatch_context(rctx, pg, osdmap);

      dout(10) << *pg << " is new (resurrected)" << dendl;

      pg->queue_peering_event(evt);
      pg->unlock();
      wake_pg_waiters(pg, resurrected);
      return;
    }
    case RES_PARENT: {
      assert(old_pg_state);
      old_pg_state->lock();
      OSDMapRef old_osd_map = old_pg_state->get_osdmap();
      int old_role = old_pg_state->role;
      vector<int> old_up = old_pg_state->up;
      int old_up_primary = old_pg_state->up_primary.osd;
      vector<int> old_acting = old_pg_state->acting;
      int old_primary = old_pg_state->primary.osd;
      pg_history_t old_history = old_pg_state->info.history;
      pg_interval_map_t old_past_intervals = old_pg_state->past_intervals;
      old_pg_state->unlock();
      PG *parent = _create_lock_pg(
	old_osd_map,
	resurrected,
	false,
	false,
	true,
	old_role,
	old_up,
	old_up_primary,
	old_acting,
	old_primary,
	old_history,
	old_past_intervals,
	*rctx.transaction
	);
      parent->handle_create(&rctx);
      parent->write_if_dirty(*rctx.transaction);
      dispatch_context(rctx, parent, osdmap);

      dout(10) << *parent << " is new" << dendl;

      assert(service.splitting(pgid));
      peering_wait_for_split[pgid].push_back(evt);

      //parent->queue_peering_event(evt);
      parent->queue_null(osdmap->get_epoch(), osdmap->get_epoch());
      parent->unlock();
      wake_pg_waiters(parent, resurrected);
      return;
    }
    }
  } else {
    // already had it.  did the mapping change?
    PG *pg = _lookup_lock_pg(pgid);
    if (epoch < pg->info.history.same_interval_since) {
      dout(10) << *pg << " get_or_create_pg acting changed in "
	       << pg->info.history.same_interval_since
	       << " (msg from " << epoch << ")" << dendl;
      pg->unlock();
      return;
    }
    pg->queue_peering_event(evt);
    pg->unlock();
    return;
  }
}


/*
 * calculate prior pg members during an epoch interval [start,end)
 *  - from each epoch, include all osds up then AND now
 *  - if no osds from then are up now, include them all, even tho they're not reachable now
 */
void OSD::calc_priors_during(
  spg_t pgid, epoch_t start, epoch_t end, set<pg_shard_t>& pset)
{
  dout(15) << "calc_priors_during " << pgid << " [" << start
	   << "," << end << ")" << dendl;
  
  for (epoch_t e = start; e < end; e++) {
    OSDMapRef oldmap = get_map(e);
    vector<int> acting;
    oldmap->pg_to_acting_osds(pgid.pgid, acting);
    dout(20) << "  " << pgid << " in epoch " << e << " was " << acting << dendl;
    int up = 0;
    int actual_osds = 0;
    for (unsigned i=0; i<acting.size(); i++) {
      if (acting[i] != CRUSH_ITEM_NONE) {
	if (osdmap->is_up(acting[i])) {
	  if (acting[i] != whoami) {
	    pset.insert(
	      pg_shard_t(
		acting[i],
		osdmap->pg_is_ec(pgid.pgid) ? shard_id_t(i) : shard_id_t::NO_SHARD));
	  }
	  up++;
	}
	actual_osds++;
      }
    }
    if (!up && actual_osds) {
      // sucky.  add down osds, even tho we can't reach them right now.
      for (unsigned i=0; i<acting.size(); i++) {
	if (acting[i] != whoami && acting[i] != CRUSH_ITEM_NONE) {
	  pset.insert(
	    pg_shard_t(
	      acting[i],
	      osdmap->pg_is_ec(pgid.pgid) ? shard_id_t(i) : shard_id_t::NO_SHARD));
	}
      }
    }
  }
  dout(10) << "calc_priors_during " << pgid
	   << " [" << start << "," << end 
	   << ") = " << pset << dendl;
}


/**
 * Fill in the passed history so you know same_interval_since, same_up_since,
 * and same_primary_since.
 */
bool OSD::project_pg_history(spg_t pgid, pg_history_t& h, epoch_t from,
			     const vector<int>& currentup,
			     int currentupprimary,
			     const vector<int>& currentacting,
			     int currentactingprimary)
{
  dout(15) << "project_pg_history " << pgid
           << " from " << from << " to " << osdmap->get_epoch()
           << ", start " << h
           << dendl;

  epoch_t e;
  for (e = osdmap->get_epoch();
       e > from;
       e--) {
    // verify during intermediate epoch (e-1)
    OSDMapRef oldmap = service.try_get_map(e-1);
    if (!oldmap) {
      dout(15) << __func__ << ": found map gap, returning false" << dendl;
      return false;
    }
    assert(oldmap->have_pg_pool(pgid.pool()));

    int upprimary, actingprimary;
    vector<int> up, acting;
    oldmap->pg_to_up_acting_osds(
      pgid.pgid,
      &up,
      &upprimary,
      &acting,
      &actingprimary);

    // acting set change?
    if ((actingprimary != currentactingprimary ||
	 upprimary != currentupprimary ||
	 acting != currentacting ||
	 up != currentup) && e > h.same_interval_since) {
      dout(15) << "project_pg_history " << pgid << " acting|up changed in " << e 
	       << " from " << acting << "/" << up
	       << " " << actingprimary << "/" << upprimary
	       << " -> " << currentacting << "/" << currentup
	       << " " << currentactingprimary << "/" << currentupprimary 
	       << dendl;
      h.same_interval_since = e;
    }
    // split?
    if (pgid.is_split(oldmap->get_pg_num(pgid.pool()),
		      osdmap->get_pg_num(pgid.pool()),
		      0)) {
      h.same_interval_since = e;
    }
    // up set change?
    if ((up != currentup || upprimary != currentupprimary)
	&& e > h.same_up_since) {
      dout(15) << "project_pg_history " << pgid << " up changed in " << e 
	       << " from " << up << " " << upprimary
	       << " -> " << currentup << " " << currentupprimary << dendl;
      h.same_up_since = e;
    }

    // primary change?
    if (OSDMap::primary_changed(
	  actingprimary,
	  acting,
	  currentactingprimary,
	  currentacting) &&
        e > h.same_primary_since) {
      dout(15) << "project_pg_history " << pgid << " primary changed in " << e << dendl;
      h.same_primary_since = e;
    }

    if (h.same_interval_since >= e && h.same_up_since >= e && h.same_primary_since >= e)
      break;
  }

  // base case: these floors should be the creation epoch if we didn't
  // find any changes.
  if (e == h.epoch_created) {
    if (!h.same_interval_since)
      h.same_interval_since = e;
    if (!h.same_up_since)
      h.same_up_since = e;
    if (!h.same_primary_since)
      h.same_primary_since = e;
  }

  dout(15) << "project_pg_history end " << h << dendl;
  return true;
}



void OSD::_add_heartbeat_peer(int p)
{
  if (p == whoami)
    return;
  HeartbeatInfo *hi;

  map<int,HeartbeatInfo>::iterator i = heartbeat_peers.find(p);
  if (i == heartbeat_peers.end()) {
    pair<ConnectionRef,ConnectionRef> cons = service.get_con_osd_hb(p, osdmap->get_epoch());
    if (!cons.first)
      return;
    hi = &heartbeat_peers[p];
    hi->peer = p;
    HeartbeatSession *s = new HeartbeatSession(p);
    hi->con_back = cons.first.get();
    hi->con_back->set_priv(s->get());
    if (cons.second) {
      hi->con_front = cons.second.get();
      hi->con_front->set_priv(s->get());
      dout(10) << "_add_heartbeat_peer: new peer osd." << p
	       << " " << hi->con_back->get_peer_addr()
	       << " " << hi->con_front->get_peer_addr()
	       << dendl;
    } else {
      hi->con_front.reset(NULL);
      dout(10) << "_add_heartbeat_peer: new peer osd." << p
	       << " " << hi->con_back->get_peer_addr()
	       << dendl;
    }
    s->put();
  } else {
    hi = &i->second;
  }
  hi->epoch = osdmap->get_epoch();
}

void OSD::_remove_heartbeat_peer(int n)
{
  map<int,HeartbeatInfo>::iterator q = heartbeat_peers.find(n);
  assert(q != heartbeat_peers.end());
  dout(20) << " removing heartbeat peer osd." << n
	   << " " << q->second.con_back->get_peer_addr()
	   << " " << (q->second.con_front ? q->second.con_front->get_peer_addr() : entity_addr_t())
	   << dendl;
  q->second.con_back->mark_down();
  if (q->second.con_front) {
    q->second.con_front->mark_down();
  }
  heartbeat_peers.erase(q);
}

void OSD::need_heartbeat_peer_update()
{
  if (is_stopping())
    return;
  dout(20) << "need_heartbeat_peer_update" << dendl;
  heartbeat_set_peers_need_update();
}

void OSD::maybe_update_heartbeat_peers()
{
  assert(osd_lock.is_locked());

  if (is_waiting_for_healthy()) {
    utime_t now = ceph_clock_now(cct);
    if (last_heartbeat_resample == utime_t()) {
      last_heartbeat_resample = now;
      heartbeat_set_peers_need_update();
    } else if (!heartbeat_peers_need_update()) {
      utime_t dur = now - last_heartbeat_resample;
      if (dur > cct->_conf->osd_heartbeat_grace) {
	dout(10) << "maybe_update_heartbeat_peers forcing update after " << dur << " seconds" << dendl;
	heartbeat_set_peers_need_update();
	last_heartbeat_resample = now;
	reset_heartbeat_peers();   // we want *new* peers!
      }
    }
  }

  Mutex::Locker l(heartbeat_lock);
  if (!heartbeat_peers_need_update())
    return;
  heartbeat_need_update = false;

  dout(10) << "maybe_update_heartbeat_peers updating" << dendl;

  heartbeat_epoch = osdmap->get_epoch();

  // build heartbeat from set
  if (is_active()) {
    RWLock::RLocker l(pg_map_lock);
    for (ceph::unordered_map<spg_t, PG*>::iterator i = pg_map.begin();
	 i != pg_map.end();
	 ++i) {
      PG *pg = i->second;
      pg->heartbeat_peer_lock.Lock();
      dout(20) << i->first << " heartbeat_peers " << pg->heartbeat_peers << dendl;
      for (set<int>::iterator p = pg->heartbeat_peers.begin();
	   p != pg->heartbeat_peers.end();
	   ++p)
	if (osdmap->is_up(*p))
	  _add_heartbeat_peer(*p);
      for (set<int>::iterator p = pg->probe_targets.begin();
	   p != pg->probe_targets.end();
	   ++p)
	if (osdmap->is_up(*p))
	  _add_heartbeat_peer(*p);
      pg->heartbeat_peer_lock.Unlock();
    }
  }

  // include next and previous up osds to ensure we have a fully-connected set
  set<int> want, extras;
  int next = osdmap->get_next_up_osd_after(whoami);
  if (next >= 0)
    want.insert(next);
  int prev = osdmap->get_previous_up_osd_before(whoami);
  if (prev >= 0)
    want.insert(prev);

  for (set<int>::iterator p = want.begin(); p != want.end(); ++p) {
    dout(10) << " adding neighbor peer osd." << *p << dendl;
    extras.insert(*p);
    _add_heartbeat_peer(*p);
  }

  // remove down peers; enumerate extras
  map<int,HeartbeatInfo>::iterator p = heartbeat_peers.begin();
  while (p != heartbeat_peers.end()) {
    if (!osdmap->is_up(p->first)) {
      int o = p->first;
      ++p;
      _remove_heartbeat_peer(o);
      continue;
    }
    if (p->second.epoch < osdmap->get_epoch()) {
      extras.insert(p->first);
    }
    ++p;
  }

  // too few?
  int start = osdmap->get_next_up_osd_after(whoami);
  for (int n = start; n >= 0; ) {
    if ((int)heartbeat_peers.size() >= cct->_conf->osd_heartbeat_min_peers)
      break;
    if (!extras.count(n) && !want.count(n) && n != whoami) {
      dout(10) << " adding random peer osd." << n << dendl;
      extras.insert(n);
      _add_heartbeat_peer(n);
    }
    n = osdmap->get_next_up_osd_after(n);
    if (n == start)
      break;  // came full circle; stop
  }

  // too many?
  for (set<int>::iterator p = extras.begin();
       (int)heartbeat_peers.size() > cct->_conf->osd_heartbeat_min_peers && p != extras.end();
       ++p) {
    if (want.count(*p))
      continue;
    _remove_heartbeat_peer(*p);
  }

  dout(10) << "maybe_update_heartbeat_peers " << heartbeat_peers.size() << " peers, extras " << extras << dendl;
}

void OSD::reset_heartbeat_peers()
{
  assert(osd_lock.is_locked());
  dout(10) << "reset_heartbeat_peers" << dendl;
  Mutex::Locker l(heartbeat_lock);
  while (!heartbeat_peers.empty()) {
    HeartbeatInfo& hi = heartbeat_peers.begin()->second;
    hi.con_back->mark_down();
    if (hi.con_front) {
      hi.con_front->mark_down();
    }
    heartbeat_peers.erase(heartbeat_peers.begin());
  }
  failure_queue.clear();
}

void OSD::handle_osd_ping(MOSDPing *m)
{
  if (superblock.cluster_fsid != m->fsid) {
    dout(20) << "handle_osd_ping from " << m->get_source_inst()
	     << " bad fsid " << m->fsid << " != " << superblock.cluster_fsid << dendl;
    m->put();
    return;
  }

  int from = m->get_source().num();

  heartbeat_lock.Lock();
  if (is_stopping()) {
    heartbeat_lock.Unlock();
    m->put();
    return;
  }

  OSDMapRef curmap = service.get_osdmap();
  
  switch (m->op) {

  case MOSDPing::PING:
    {
      if (cct->_conf->osd_debug_drop_ping_probability > 0) {
	if (debug_heartbeat_drops_remaining.count(from)) {
	  if (debug_heartbeat_drops_remaining[from] == 0) {
	    debug_heartbeat_drops_remaining.erase(from);
	  } else {
	    debug_heartbeat_drops_remaining[from]--;
	    dout(5) << "Dropping heartbeat from " << from
		    << ", " << debug_heartbeat_drops_remaining[from]
		    << " remaining to drop" << dendl;
	    break;
	  }
	} else if (cct->_conf->osd_debug_drop_ping_probability >
	           ((((double)(rand()%100))/100.0))) {
	  debug_heartbeat_drops_remaining[from] =
	    cct->_conf->osd_debug_drop_ping_duration;
	  dout(5) << "Dropping heartbeat from " << from
		  << ", " << debug_heartbeat_drops_remaining[from]
		  << " remaining to drop" << dendl;
	  break;
	}
      }

      if (!cct->get_heartbeat_map()->is_healthy()) {
	dout(10) << "internal heartbeat not healthy, dropping ping request" << dendl;
	break;
      }

      Message *r = new MOSDPing(monc->get_fsid(),
				curmap->get_epoch(),
				MOSDPing::PING_REPLY,
				m->stamp);
      m->get_connection()->send_message(r);

      if (curmap->is_up(from)) {
	service.note_peer_epoch(from, m->map_epoch);
	if (is_active()) {
	  ConnectionRef con = service.get_con_osd_cluster(from, curmap->get_epoch());
	  if (con) {
	    service.share_map_peer(from, con.get());
	  }
	}
      } else if (!curmap->exists(from) ||
		 curmap->get_down_at(from) > m->map_epoch) {
	// tell them they have died
	Message *r = new MOSDPing(monc->get_fsid(),
				  curmap->get_epoch(),
				  MOSDPing::YOU_DIED,
				  m->stamp);
	m->get_connection()->send_message(r);
      }
    }
    break;

  case MOSDPing::PING_REPLY:
    {
      map<int,HeartbeatInfo>::iterator i = heartbeat_peers.find(from);
      if (i != heartbeat_peers.end()) {
	if (m->get_connection() == i->second.con_back) {
	  dout(25) << "handle_osd_ping got reply from osd." << from
		   << " first_rx " << i->second.first_tx
		   << " last_tx " << i->second.last_tx
		   << " last_rx_back " << i->second.last_rx_back << " -> " << m->stamp
		   << " last_rx_front " << i->second.last_rx_front
		   << dendl;
	  i->second.last_rx_back = m->stamp;
	  // if there is no front con, set both stamps.
	  if (i->second.con_front == NULL)
	    i->second.last_rx_front = m->stamp;
	} else if (m->get_connection() == i->second.con_front) {
	  dout(25) << "handle_osd_ping got reply from osd." << from
		   << " first_rx " << i->second.first_tx
		   << " last_tx " << i->second.last_tx
		   << " last_rx_back " << i->second.last_rx_back
		   << " last_rx_front " << i->second.last_rx_front << " -> " << m->stamp
		   << dendl;
	  i->second.last_rx_front = m->stamp;
	}
      }

      if (m->map_epoch &&
	  curmap->is_up(from)) {
	service.note_peer_epoch(from, m->map_epoch);
	if (is_active()) {
	  ConnectionRef con = service.get_con_osd_cluster(from, curmap->get_epoch());
	  if (con) {
	    service.share_map_peer(from, con.get());
	  }
	}
      }

      utime_t cutoff = ceph_clock_now(cct);
      cutoff -= cct->_conf->osd_heartbeat_grace;
      if (i->second.is_healthy(cutoff)) {
	// Cancel false reports
	if (failure_queue.count(from)) {
	  dout(10) << "handle_osd_ping canceling queued failure report for osd." << from<< dendl;
	  failure_queue.erase(from);
	}
	if (failure_pending.count(from)) {
	  dout(10) << "handle_osd_ping canceling in-flight failure report for osd." << from<< dendl;
	  send_still_alive(curmap->get_epoch(), failure_pending[from]);
	  failure_pending.erase(from);
	}
      }
    }
    break;

  case MOSDPing::YOU_DIED:
    dout(10) << "handle_osd_ping " << m->get_source_inst()
	     << " says i am down in " << m->map_epoch << dendl;
    osdmap_subscribe(curmap->get_epoch()+1, false);
    break;
  }

  heartbeat_lock.Unlock();
  m->put();
}

void OSD::heartbeat_entry()
{
  Mutex::Locker l(heartbeat_lock);
  if (is_stopping())
    return;
  while (!heartbeat_stop) {
    heartbeat();

    double wait = .5 + ((float)(rand() % 10)/10.0) * (float)cct->_conf->osd_heartbeat_interval;
    utime_t w;
    w.set_from_double(wait);
    dout(30) << "heartbeat_entry sleeping for " << wait << dendl;
    heartbeat_cond.WaitInterval(cct, heartbeat_lock, w);
    if (is_stopping())
      return;
    dout(30) << "heartbeat_entry woke up" << dendl;
  }
}

void OSD::heartbeat_check()
{
  assert(heartbeat_lock.is_locked());
  utime_t now = ceph_clock_now(cct);
  double age = hbclient_messenger->get_dispatch_queue_max_age(now);
  if (age > (cct->_conf->osd_heartbeat_grace / 2)) {
    derr << "skipping heartbeat_check, hbqueue max age: " << age << dendl;
    return; // hb dispatch is too backed up for our hb status to be meaningful
  }

  // check for incoming heartbeats (move me elsewhere?)
  utime_t cutoff = now;
  cutoff -= cct->_conf->osd_heartbeat_grace;
  for (map<int,HeartbeatInfo>::iterator p = heartbeat_peers.begin();
       p != heartbeat_peers.end();
       ++p) {
    dout(25) << "heartbeat_check osd." << p->first
	     << " first_tx " << p->second.first_tx
	     << " last_tx " << p->second.last_tx
	     << " last_rx_back " << p->second.last_rx_back
	     << " last_rx_front " << p->second.last_rx_front
	     << dendl;
    if (p->second.is_unhealthy(cutoff)) {
      if (p->second.last_rx_back == utime_t() ||
	  p->second.last_rx_front == utime_t()) {
	derr << "heartbeat_check: no reply from osd." << p->first
	     << " ever on either front or back, first ping sent " << p->second.first_tx
	     << " (cutoff " << cutoff << ")" << dendl;
	// fail
	failure_queue[p->first] = p->second.last_tx;
      } else {
	derr << "heartbeat_check: no reply from osd." << p->first
	     << " since back " << p->second.last_rx_back
	     << " front " << p->second.last_rx_front
	     << " (cutoff " << cutoff << ")" << dendl;
	// fail
	failure_queue[p->first] = MIN(p->second.last_rx_back, p->second.last_rx_front);
      }
    }
  }
}

void OSD::heartbeat()
{
  dout(30) << "heartbeat" << dendl;

  // get CPU load avg
  double loadavgs[1];
  if (getloadavg(loadavgs, 1) == 1)
    logger->set(l_osd_loadavg, 100 * loadavgs[0]);

  dout(30) << "heartbeat checking stats" << dendl;

  // refresh stats?
  vector<int> hb_peers;
  for (map<int,HeartbeatInfo>::iterator p = heartbeat_peers.begin();
       p != heartbeat_peers.end();
       ++p)
    hb_peers.push_back(p->first);
  service.update_osd_stat(hb_peers);

  dout(5) << "heartbeat: " << service.get_osd_stat() << dendl;

  utime_t now = ceph_clock_now(cct);

  // send heartbeats
  for (map<int,HeartbeatInfo>::iterator i = heartbeat_peers.begin();
       i != heartbeat_peers.end();
       ++i) {
    int peer = i->first;
    i->second.last_tx = now;
    if (i->second.first_tx == utime_t())
      i->second.first_tx = now;
    dout(30) << "heartbeat sending ping to osd." << peer << dendl;
    i->second.con_back->send_message(new MOSDPing(monc->get_fsid(),
					  service.get_osdmap()->get_epoch(),
					  MOSDPing::PING,
					  now));

    if (i->second.con_front)
      i->second.con_front->send_message(new MOSDPing(monc->get_fsid(),
					     service.get_osdmap()->get_epoch(),
						     MOSDPing::PING,
						     now));
  }

  dout(30) << "heartbeat check" << dendl;
  heartbeat_check();

  logger->set(l_osd_hb_to, heartbeat_peers.size());
  logger->set(l_osd_hb_from, 0);
  
  // hmm.. am i all alone?
  dout(30) << "heartbeat lonely?" << dendl;
  if (heartbeat_peers.empty()) {
    if (now - last_mon_heartbeat > cct->_conf->osd_mon_heartbeat_interval && is_active()) {
      last_mon_heartbeat = now;
      dout(10) << "i have no heartbeat peers; checking mon for new map" << dendl;
      osdmap_subscribe(osdmap->get_epoch() + 1, true);
    }
  }

  dout(30) << "heartbeat done" << dendl;
}

bool OSD::heartbeat_reset(Connection *con)
{
  HeartbeatSession *s = static_cast<HeartbeatSession*>(con->get_priv());
  if (s) {
    heartbeat_lock.Lock();
    if (is_stopping()) {
      heartbeat_lock.Unlock();
      s->put();
      return true;
    }
    map<int,HeartbeatInfo>::iterator p = heartbeat_peers.find(s->peer);
    if (p != heartbeat_peers.end() &&
	(p->second.con_back == con ||
	 p->second.con_front == con)) {
      dout(10) << "heartbeat_reset failed hb con " << con << " for osd." << p->second.peer
	       << ", reopening" << dendl;
      if (con != p->second.con_back) {
	p->second.con_back->mark_down();
      }
      p->second.con_back.reset(NULL);
      if (p->second.con_front && con != p->second.con_front) {
	p->second.con_front->mark_down();
      }
      p->second.con_front.reset(NULL);
      pair<ConnectionRef,ConnectionRef> newcon = service.get_con_osd_hb(p->second.peer, p->second.epoch);
      if (newcon.first) {
	p->second.con_back = newcon.first.get();
	p->second.con_back->set_priv(s->get());
	if (newcon.second) {
	  p->second.con_front = newcon.second.get();
	  p->second.con_front->set_priv(s->get());
	}
      } else {
	dout(10) << "heartbeat_reset failed hb con " << con << " for osd." << p->second.peer
		 << ", raced with osdmap update, closing out peer" << dendl;
	heartbeat_peers.erase(p);
      }
    } else {
      dout(10) << "heartbeat_reset closing (old) failed hb con " << con << dendl;
    }
    heartbeat_lock.Unlock();
    s->put();
  }
  return true;
}



// =========================================

void OSD::tick()
{
  assert(osd_lock.is_locked());
  dout(5) << "tick" << dendl;

  logger->set(l_osd_buf, buffer::get_total_alloc());

  if (is_active() || is_waiting_for_healthy()) {
    map_lock.get_read();

    maybe_update_heartbeat_peers();

    heartbeat_lock.Lock();
    heartbeat_check();
    heartbeat_lock.Unlock();

    // mon report?
    utime_t now = ceph_clock_now(cct);
    if (outstanding_pg_stats && timeout_mon_on_pg_stats &&
	(now - cct->_conf->osd_mon_ack_timeout) > last_pg_stats_ack) {
      dout(1) << "mon hasn't acked PGStats in " << now - last_pg_stats_ack
	      << " seconds, reconnecting elsewhere" << dendl;
      monc->reopen_session(new C_MonStatsAckTimer(this));
      timeout_mon_on_pg_stats = false;
      last_pg_stats_ack = ceph_clock_now(cct);  // reset clock
      last_pg_stats_sent = utime_t();
    }
    if (now - last_pg_stats_sent > cct->_conf->osd_mon_report_interval_max) {
      osd_stat_updated = true;
      do_mon_report();
    } else if (now - last_mon_report > cct->_conf->osd_mon_report_interval_min) {
      do_mon_report();
    }

    map_lock.put_read();
  }

  if (is_waiting_for_healthy()) {
    if (_is_healthy()) {
      dout(1) << "healthy again, booting" << dendl;
      set_state(STATE_BOOTING);
      start_boot();
    }
  }

  if (is_active()) {
    // periodically kick recovery work queue
    recovery_tp.wake();

    if (!scrub_random_backoff()) {
      sched_scrub();
    }

    check_replay_queue();
  }

  // only do waiters if dispatch() isn't currently running.  (if it is,
  // it'll do the waiters, and doing them here may screw up ordering
  // of op_queue vs handle_osd_map.)
  if (!dispatch_running) {
    dispatch_running = true;
    do_waiters();
    dispatch_running = false;
    dispatch_cond.Signal();
  }

  check_ops_in_flight();

  tick_timer.add_event_after(1.0, new C_Tick(this));
}

void OSD::check_ops_in_flight()
{
  vector<string> warnings;
  if (op_tracker.check_ops_in_flight(warnings)) {
    for (vector<string>::iterator i = warnings.begin();
        i != warnings.end();
        ++i) {
      clog->warn() << *i;
    }
  }
  return;
}

// Usage:
//   setomapval <pool-id> [namespace/]<obj-name> <key> <val>
//   rmomapkey <pool-id> [namespace/]<obj-name> <key>
//   setomapheader <pool-id> [namespace/]<obj-name> <header>
//   getomap <pool> [namespace/]<obj-name>
//   truncobj <pool-id> [namespace/]<obj-name> <newlen>
//   injectmdataerr [namespace/]<obj-name>
//   injectdataerr [namespace/]<obj-name>
void TestOpsSocketHook::test_ops(OSDService *service, ObjectStore *store,
     std::string command, cmdmap_t& cmdmap, ostream &ss)
{
  //Test support
  //Support changing the omap on a single osd by using the Admin Socket to
  //directly request the osd make a change.
  if (command == "setomapval" || command == "rmomapkey" ||
      command == "setomapheader" || command == "getomap" ||
      command == "truncobj" || command == "injectmdataerr" ||
      command == "injectdataerr"
    ) {
    pg_t rawpg;
    int64_t pool;
    OSDMapRef curmap = service->get_osdmap();
    int r;

    string poolstr;

    cmd_getval(service->cct, cmdmap, "pool", poolstr);
    pool = curmap->lookup_pg_pool_name(poolstr);
    //If we can't find it by name then maybe id specified
    if (pool < 0 && isdigit(poolstr[0]))
      pool = atoll(poolstr.c_str());
    if (pool < 0) {
      ss << "Invalid pool" << poolstr;
      return;
    }
    r = -1;
    string objname, nspace;
    cmd_getval(service->cct, cmdmap, "objname", objname);
    std::size_t found = objname.find_first_of('/');
    if (found != string::npos) {
      nspace = objname.substr(0, found);
      objname = objname.substr(found+1);
    }
    object_locator_t oloc(pool, nspace);
    r = curmap->object_locator_to_pg(object_t(objname), oloc,  rawpg);

    if (r < 0) {
      ss << "Invalid namespace/objname";
      return;
    }
    if (curmap->pg_is_ec(rawpg)) {
      ss << "Must not call on ec pool";
      return;
    }
    spg_t pgid = spg_t(curmap->raw_pg_to_pg(rawpg), shard_id_t::NO_SHARD);

    hobject_t obj(object_t(objname), string(""), CEPH_NOSNAP, rawpg.ps(), pool, nspace);
    ObjectStore::Transaction t;

    if (command == "setomapval") {
      map<string, bufferlist> newattrs;
      bufferlist val;
      string key, valstr;
      cmd_getval(service->cct, cmdmap, "key", key);
      cmd_getval(service->cct, cmdmap, "val", valstr);

      val.append(valstr);
      newattrs[key] = val;
      t.omap_setkeys(coll_t(pgid), obj, newattrs);
      r = store->apply_transaction(t);
      if (r < 0)
        ss << "error=" << r;
      else
        ss << "ok";
    } else if (command == "rmomapkey") {
      string key;
      set<string> keys;
      cmd_getval(service->cct, cmdmap, "key", key);

      keys.insert(key);
      t.omap_rmkeys(coll_t(pgid), obj, keys);
      r = store->apply_transaction(t);
      if (r < 0)
        ss << "error=" << r;
      else
        ss << "ok";
    } else if (command == "setomapheader") {
      bufferlist newheader;
      string headerstr;

      cmd_getval(service->cct, cmdmap, "header", headerstr);
      newheader.append(headerstr);
      t.omap_setheader(coll_t(pgid), obj, newheader);
      r = store->apply_transaction(t);
      if (r < 0)
        ss << "error=" << r;
      else
        ss << "ok";
    } else if (command == "getomap") {
      //Debug: Output entire omap
      bufferlist hdrbl;
      map<string, bufferlist> keyvals;
      r = store->omap_get(coll_t(pgid), obj, &hdrbl, &keyvals);
      if (r >= 0) {
          ss << "header=" << string(hdrbl.c_str(), hdrbl.length());
          for (map<string, bufferlist>::iterator it = keyvals.begin();
              it != keyvals.end(); ++it)
            ss << " key=" << (*it).first << " val="
               << string((*it).second.c_str(), (*it).second.length());
      } else {
          ss << "error=" << r;
      }
    } else if (command == "truncobj") {
      int64_t trunclen;
      cmd_getval(service->cct, cmdmap, "len", trunclen);
      t.truncate(coll_t(pgid), obj, trunclen);
      r = store->apply_transaction(t);
      if (r < 0)
	ss << "error=" << r;
      else
	ss << "ok";
    } else if (command == "injectdataerr") {
      store->inject_data_error(obj);
      ss << "ok";
    } else if (command == "injectmdataerr") {
      store->inject_mdata_error(obj);
      ss << "ok";
    }
    return;
  }
  ss << "Internal error - command=" << command;
  return;
}

// =========================================
bool remove_dir(
  CephContext *cct,
  ObjectStore *store, SnapMapper *mapper,
  OSDriver *osdriver,
  ObjectStore::Sequencer *osr,
  coll_t coll, DeletingStateRef dstate,
  ThreadPool::TPHandle &handle)
{
  vector<ghobject_t> olist;
  int64_t num = 0;
  ObjectStore::Transaction *t = new ObjectStore::Transaction;
  ghobject_t next;
  while (!next.is_max()) {
    handle.reset_tp_timeout();
    store->collection_list_partial(
      coll,
      next,
      store->get_ideal_list_min(),
      store->get_ideal_list_max(),
      0,
      &olist,
      &next);
    for (vector<ghobject_t>::iterator i = olist.begin();
	 i != olist.end();
	 ++i, ++num) {
      if (i->is_pgmeta())
	continue;
      OSDriver::OSTransaction _t(osdriver->get_transaction(t));
      int r = mapper->remove_oid(i->hobj, &_t);
      if (r != 0 && r != -ENOENT) {
	assert(0);
      }
      t->remove(coll, *i);
      if (num >= cct->_conf->osd_target_transaction_size) {
	C_SaferCond waiter;
	store->queue_transaction(osr, t, &waiter);
	bool cont = dstate->pause_clearing();
	handle.suspend_tp_timeout();
	waiter.wait();
	handle.reset_tp_timeout();
	if (cont)
	  cont = dstate->resume_clearing();
	delete t;
	if (!cont)
	  return false;
	t = new ObjectStore::Transaction;
	num = 0;
      }
    }
    olist.clear();
  }

  C_SaferCond waiter;
  store->queue_transaction(osr, t, &waiter);
  bool cont = dstate->pause_clearing();
  handle.suspend_tp_timeout();
  waiter.wait();
  handle.reset_tp_timeout();
  if (cont)
    cont = dstate->resume_clearing();
  delete t;
  return cont;
}

void OSD::RemoveWQ::_process(
  pair<PGRef, DeletingStateRef> item,
  ThreadPool::TPHandle &handle)
{
  PGRef pg(item.first);
  SnapMapper &mapper = pg->snap_mapper;
  OSDriver &driver = pg->osdriver;
  coll_t coll = coll_t(pg->info.pgid);
  pg->osr->flush();

  if (!item.second->start_clearing())
    return;

  list<coll_t> colls_to_remove;
  pg->get_colls(&colls_to_remove);
  for (list<coll_t>::iterator i = colls_to_remove.begin();
       i != colls_to_remove.end();
       ++i) {
    bool cont = remove_dir(
      pg->cct, store, &mapper, &driver, pg->osr.get(), *i, item.second,
      handle);
    if (!cont)
      return;
  }

  if (!item.second->start_deleting())
    return;

  ObjectStore::Transaction *t = new ObjectStore::Transaction;
  PGLog::clear_info_log(pg->info.pgid, t);

  for (list<coll_t>::iterator i = colls_to_remove.begin();
       i != colls_to_remove.end();
       ++i) {
    t->remove_collection(*i);
  }

  // We need the sequencer to stick around until the op is complete
  store->queue_transaction(
    pg->osr.get(),
    t,
    0, // onapplied
    0, // oncommit
    0, // onreadable sync
    new ObjectStore::C_DeleteTransactionHolder<PGRef>(
      t, pg), // oncomplete
    TrackedOpRef());

  item.second->finish_deleting();
}
// =========================================

void OSD::do_mon_report()
{
  dout(7) << "do_mon_report" << dendl;

  utime_t now(ceph_clock_now(cct));
  last_mon_report = now;

  // do any pending reports
  send_alive();
  service.send_pg_temp();
  send_failures();
  send_pg_stats(now);
}

void OSD::ms_handle_connect(Connection *con)
{
  if (con->get_peer_type() == CEPH_ENTITY_TYPE_MON) {
    Mutex::Locker l(osd_lock);
    if (is_stopping())
      return;
    dout(10) << "ms_handle_connect on mon" << dendl;
    if (is_booting()) {
      start_boot();
    } else {
      send_alive();
      service.send_pg_temp();
      send_failures();
      send_pg_stats(ceph_clock_now(cct));

      monc->sub_want("osd_pg_creates", 0, CEPH_SUBSCRIBE_ONETIME);
      monc->sub_want("osdmap", osdmap->get_epoch(), CEPH_SUBSCRIBE_ONETIME);
      monc->renew_subs();
    }
  }
}

void OSD::ms_handle_fast_connect(Connection *con)
{
  if (con->get_peer_type() != CEPH_ENTITY_TYPE_MON) {
    Session *s = static_cast<Session*>(con->get_priv());
    if (!s) {
      s = new Session(cct);
      con->set_priv(s->get());
      s->con = con;
      dout(10) << " new session (outgoing) " << s << " con=" << s->con
          << " addr=" << s->con->get_peer_addr() << dendl;
      // we don't connect to clients
      assert(con->get_peer_type() == CEPH_ENTITY_TYPE_OSD);
      s->entity_name.set_type(CEPH_ENTITY_TYPE_OSD);
    }
    s->put();
  }
}

void OSD::ms_handle_fast_accept(Connection *con)
{
  if (con->get_peer_type() != CEPH_ENTITY_TYPE_MON) {
    Session *s = static_cast<Session*>(con->get_priv());
    if (!s) {
      s = new Session(cct);
      con->set_priv(s->get());
      s->con = con;
      dout(10) << "new session (incoming)" << s << " con=" << con
          << " addr=" << con->get_peer_addr()
          << " must have raced with connect" << dendl;
      assert(con->get_peer_type() == CEPH_ENTITY_TYPE_OSD);
      s->entity_name.set_type(CEPH_ENTITY_TYPE_OSD);
    }
    s->put();
  }
}

bool OSD::ms_handle_reset(Connection *con)
{
  OSD::Session *session = (OSD::Session *)con->get_priv();
  dout(1) << "ms_handle_reset con " << con << " session " << session << dendl;
  if (!session)
    return false;
  session->wstate.reset();
  session->con.reset(NULL);  // break con <-> session ref cycle
  session_handle_reset(session);
  session->put();
  return true;
}

struct C_OSD_GetVersion : public Context {
  OSD *osd;
  uint64_t oldest, newest;
  C_OSD_GetVersion(OSD *o) : osd(o), oldest(0), newest(0) {}
  void finish(int r) {
    if (r >= 0)
      osd->_maybe_boot(oldest, newest);
  }
};

void OSD::start_boot()
{
  dout(10) << "start_boot - have maps " << superblock.oldest_map
	   << ".." << superblock.newest_map << dendl;
  C_OSD_GetVersion *c = new C_OSD_GetVersion(this);
  monc->get_version("osdmap", &c->newest, &c->oldest, c);
}

void OSD::_maybe_boot(epoch_t oldest, epoch_t newest)
{
  Mutex::Locker l(osd_lock);
  if (is_stopping())
    return;
  dout(10) << "_maybe_boot mon has osdmaps " << oldest << ".." << newest << dendl;

  if (is_initializing()) {
    dout(10) << "still initializing" << dendl;
    return;
  }

  // if our map within recent history, try to add ourselves to the osdmap.
  if (osdmap->test_flag(CEPH_OSDMAP_NOUP)) {
    dout(5) << "osdmap NOUP flag is set, waiting for it to clear" << dendl;
  } else if (is_waiting_for_healthy() || !_is_healthy()) {
    // if we are not healthy, do not mark ourselves up (yet)
    dout(1) << "not healthy; waiting to boot" << dendl;
    if (!is_waiting_for_healthy())
      start_waiting_for_healthy();
    // send pings sooner rather than later
    heartbeat_kick();
  } else if (osdmap->get_epoch() >= oldest - 1 &&
	     osdmap->get_epoch() + cct->_conf->osd_map_message_max > newest) {
    _send_boot();
    return;
  }
  
  // get all the latest maps
  if (osdmap->get_epoch() + 1 >= oldest)
    osdmap_subscribe(osdmap->get_epoch() + 1, true);
  else
    osdmap_subscribe(oldest - 1, true);
}

void OSD::start_waiting_for_healthy()
{
  dout(1) << "start_waiting_for_healthy" << dendl;
  set_state(STATE_WAITING_FOR_HEALTHY);
  last_heartbeat_resample = utime_t();
}

bool OSD::_is_healthy()
{
  if (!cct->get_heartbeat_map()->is_healthy()) {
    dout(1) << "is_healthy false -- internal heartbeat failed" << dendl;
    return false;
  }

  if (is_waiting_for_healthy()) {
    Mutex::Locker l(heartbeat_lock);
    utime_t cutoff = ceph_clock_now(cct);
    cutoff -= cct->_conf->osd_heartbeat_grace;
    int num = 0, up = 0;
    for (map<int,HeartbeatInfo>::iterator p = heartbeat_peers.begin();
	 p != heartbeat_peers.end();
	 ++p) {
      if (p->second.is_healthy(cutoff))
	++up;
      ++num;
    }
    if ((float)up < (float)num * cct->_conf->osd_heartbeat_min_healthy_ratio) {
      dout(1) << "is_healthy false -- only " << up << "/" << num << " up peers (less than 1/3)" << dendl;
      return false;
    }
  }

  return true;
}

void OSD::_send_boot()
{
  dout(10) << "_send_boot" << dendl;
  entity_addr_t cluster_addr = cluster_messenger->get_myaddr();
  Connection *local_connection = cluster_messenger->get_loopback_connection().get();
  if (cluster_addr.is_blank_ip()) {
    int port = cluster_addr.get_port();
    cluster_addr = client_messenger->get_myaddr();
    cluster_addr.set_port(port);
    cluster_messenger->set_addr_unknowns(cluster_addr);
    dout(10) << " assuming cluster_addr ip matches client_addr" << dendl;
  } else {
    Session *s = static_cast<Session*>(local_connection->get_priv());
    if (s)
      s->put();
    else
      cluster_messenger->ms_deliver_handle_fast_connect(local_connection);
  }

  entity_addr_t hb_back_addr = hb_back_server_messenger->get_myaddr();
  local_connection = hb_back_server_messenger->get_loopback_connection().get();
  if (hb_back_addr.is_blank_ip()) {
    int port = hb_back_addr.get_port();
    hb_back_addr = cluster_addr;
    hb_back_addr.set_port(port);
    hb_back_server_messenger->set_addr_unknowns(hb_back_addr);
    dout(10) << " assuming hb_back_addr ip matches cluster_addr" << dendl;
  } else {
    Session *s = static_cast<Session*>(local_connection->get_priv());
    if (s)
      s->put();
    else
      hb_back_server_messenger->ms_deliver_handle_fast_connect(local_connection);
  }

  entity_addr_t hb_front_addr = hb_front_server_messenger->get_myaddr();
  local_connection = hb_front_server_messenger->get_loopback_connection().get();
  if (hb_front_addr.is_blank_ip()) {
    int port = hb_front_addr.get_port();
    hb_front_addr = client_messenger->get_myaddr();
    hb_front_addr.set_port(port);
    hb_front_server_messenger->set_addr_unknowns(hb_front_addr);
    dout(10) << " assuming hb_front_addr ip matches client_addr" << dendl;
  } else {
    Session *s = static_cast<Session*>(local_connection->get_priv());
    if (s)
      s->put();
    else
      hb_front_server_messenger->ms_deliver_handle_fast_connect(local_connection);
  }

  MOSDBoot *mboot = new MOSDBoot(superblock, service.get_boot_epoch(),
                                 hb_back_addr, hb_front_addr, cluster_addr);
  dout(10) << " client_addr " << client_messenger->get_myaddr()
	   << ", cluster_addr " << cluster_addr
	   << ", hb_back_addr " << hb_back_addr
	   << ", hb_front_addr " << hb_front_addr
	   << dendl;
  _collect_metadata(&mboot->metadata);
  monc->send_mon_message(mboot);
}

bool OSD::_lsb_release_set (char *buf, const char *str, map<string,string> *pm, const char *key)
{
  if (strncmp (buf, str, strlen (str)) == 0) {
    char *value;

    if (buf[strlen(buf)-1] == '\n')
      buf[strlen(buf)-1] = '\0';

    value = buf + strlen (str) + 1;
    (*pm)[key] = value;

    return true;
  }
  return false;
}

void OSD::_lsb_release_parse (map<string,string> *pm)
{
  FILE *fp = NULL;
  char buf[512];

  fp = popen("lsb_release -idrc", "r");
  if (!fp) {
    int ret = -errno;
    derr << "lsb_release_parse - failed to call lsb_release binary with error: " << cpp_strerror(ret) << dendl;
    return;
  }

  while (fgets(buf, sizeof(buf) - 1, fp) != NULL) {
    if (_lsb_release_set(buf, "Distributor ID:", pm, "distro")) 
      continue;
    if (_lsb_release_set(buf, "Description:", pm, "distro_description"))
      continue;
    if (_lsb_release_set(buf, "Release:", pm, "distro_version"))
      continue;
    if (_lsb_release_set(buf, "Codename:", pm, "distro_codename"))
      continue;
    
    derr << "unhandled output: " << buf << dendl;
  }

  if (pclose(fp)) {
    int ret = -errno;
    derr << "lsb_release_parse - pclose failed: " << cpp_strerror(ret) << dendl;
  }
}

void OSD::_collect_metadata(map<string,string> *pm)
{
  (*pm)["ceph_version"] = pretty_version_to_str();

  // config info
  (*pm)["osd_data"] = dev_path;
  (*pm)["osd_journal"] = journal_path;
  (*pm)["front_addr"] = stringify(client_messenger->get_myaddr());
  (*pm)["back_addr"] = stringify(cluster_messenger->get_myaddr());
  (*pm)["hb_front_addr"] = stringify(hb_front_server_messenger->get_myaddr());
  (*pm)["hb_back_addr"] = stringify(hb_back_server_messenger->get_myaddr());

  // backend
  (*pm)["osd_objectstore"] = g_conf->osd_objectstore;
  store->collect_metadata(pm);

  // kernel info
  struct utsname u;
  int r = uname(&u);
  if (r >= 0) {
    (*pm)["os"] = u.sysname;
    (*pm)["kernel_version"] = u.release;
    (*pm)["kernel_description"] = u.version;
    (*pm)["hostname"] = u.nodename;
    (*pm)["arch"] = u.machine;
  }

  // memory
  FILE *f = fopen("/proc/meminfo", "r");
  if (f) {
    char buf[100];
    while (!feof(f)) {
      char *line = fgets(buf, sizeof(buf), f);
      if (!line)
	break;
      char key[40];
      long long value;
      int r = sscanf(line, "%s %lld", key, &value);
      if (r == 2) {
	if (strcmp(key, "MemTotal:") == 0)
	  (*pm)["mem_total_kb"] = stringify(value);
	else if (strcmp(key, "SwapTotal:") == 0)
	  (*pm)["mem_swap_kb"] = stringify(value);
      }
    }
    fclose(f);
  }

  // processor
  f = fopen("/proc/cpuinfo", "r");
  if (f) {
    char buf[100];
    while (!feof(f)) {
      char *line = fgets(buf, sizeof(buf), f);
      if (!line)
	break;
      if (strncmp(line, "model name", 10) == 0) {
	char *c = strchr(buf, ':');
	c++;
	while (*c == ' ')
	  ++c;
	char *nl = c;
	while (*nl != '\n')
	  ++nl;
	*nl = '\0';
	(*pm)["cpu"] = c;
	break;
      }
    }
    fclose(f);
  }

  // distro info
  _lsb_release_parse(pm); 

  dout(10) << __func__ << " " << *pm << dendl;
}

void OSD::queue_want_up_thru(epoch_t want)
{
  map_lock.get_read();
  epoch_t cur = osdmap->get_up_thru(whoami);
  if (want > up_thru_wanted) {
    dout(10) << "queue_want_up_thru now " << want << " (was " << up_thru_wanted << ")" 
	     << ", currently " << cur
	     << dendl;
    up_thru_wanted = want;

    // expedite, a bit.  WARNING this will somewhat delay other mon queries.
    last_mon_report = ceph_clock_now(cct);
    send_alive();
  } else {
    dout(10) << "queue_want_up_thru want " << want << " <= queued " << up_thru_wanted 
	     << ", currently " << cur
	     << dendl;
  }
  map_lock.put_read();
}

void OSD::send_alive()
{
  if (!osdmap->exists(whoami))
    return;
  epoch_t up_thru = osdmap->get_up_thru(whoami);
  dout(10) << "send_alive up_thru currently " << up_thru << " want " << up_thru_wanted << dendl;
  if (up_thru_wanted > up_thru) {
    up_thru_pending = up_thru_wanted;
    dout(10) << "send_alive want " << up_thru_wanted << dendl;
    monc->send_mon_message(new MOSDAlive(osdmap->get_epoch(), up_thru_wanted));
  }
}

void OSD::send_failures()
{
  assert(osd_lock.is_locked());
  bool locked = false;
  if (!failure_queue.empty()) {
    heartbeat_lock.Lock();
    locked = true;
  }
  utime_t now = ceph_clock_now(cct);
  while (!failure_queue.empty()) {
    int osd = failure_queue.begin()->first;
    int failed_for = (int)(double)(now - failure_queue.begin()->second);
    entity_inst_t i = osdmap->get_inst(osd);
    monc->send_mon_message(new MOSDFailure(monc->get_fsid(), i, failed_for, osdmap->get_epoch()));
    failure_pending[osd] = i;
    failure_queue.erase(osd);
  }
  if (locked) heartbeat_lock.Unlock();
}

void OSD::send_still_alive(epoch_t epoch, const entity_inst_t &i)
{
  MOSDFailure *m = new MOSDFailure(monc->get_fsid(), i, 0, epoch);
  m->is_failed = false;
  monc->send_mon_message(m);
}

void OSD::send_pg_stats(const utime_t &now)
{
  assert(osd_lock.is_locked());

  dout(20) << "send_pg_stats" << dendl;

  osd_stat_t cur_stat = service.get_osd_stat();

  cur_stat.fs_perf_stat = store->get_cur_stats();
   
  pg_stat_queue_lock.Lock();

  if (osd_stat_updated || !pg_stat_queue.empty()) {
    last_pg_stats_sent = now;
    osd_stat_updated = false;

    dout(10) << "send_pg_stats - " << pg_stat_queue.size() << " pgs updated" << dendl;

    utime_t had_for(now);
    had_for -= had_map_since;

    MPGStats *m = new MPGStats(monc->get_fsid(), osdmap->get_epoch(), had_for);
    m->set_tid(++pg_stat_tid);
    m->osd_stat = cur_stat;

    xlist<PG*>::iterator p = pg_stat_queue.begin();
    while (!p.end()) {
      PG *pg = *p;
      ++p;
      if (!pg->is_primary()) {  // we hold map_lock; role is stable.
	pg->stat_queue_item.remove_myself();
	pg->put("pg_stat_queue");
	continue;
      }
      pg->pg_stats_publish_lock.Lock();
      if (pg->pg_stats_publish_valid) {
	m->pg_stat[pg->info.pgid.pgid] = pg->pg_stats_publish;
	dout(25) << " sending " << pg->info.pgid << " " << pg->pg_stats_publish.reported_epoch << ":"
		 << pg->pg_stats_publish.reported_seq << dendl;
      } else {
	dout(25) << " NOT sending " << pg->info.pgid << " " << pg->pg_stats_publish.reported_epoch << ":"
		 << pg->pg_stats_publish.reported_seq << ", not valid" << dendl;
      }
      pg->pg_stats_publish_lock.Unlock();
    }

    if (!outstanding_pg_stats) {
      outstanding_pg_stats = true;
      last_pg_stats_ack = ceph_clock_now(cct);
    }
    monc->send_mon_message(m);
  }

  pg_stat_queue_lock.Unlock();
}

void OSD::handle_pg_stats_ack(MPGStatsAck *ack)
{
  dout(10) << "handle_pg_stats_ack " << dendl;

  if (!require_mon_peer(ack)) {
    ack->put();
    return;
  }

  last_pg_stats_ack = ceph_clock_now(cct);

  pg_stat_queue_lock.Lock();

  if (ack->get_tid() > pg_stat_tid_flushed) {
    pg_stat_tid_flushed = ack->get_tid();
    pg_stat_queue_cond.Signal();
  }

  xlist<PG*>::iterator p = pg_stat_queue.begin();
  while (!p.end()) {
    PG *pg = *p;
    PGRef _pg(pg);
    ++p;

    if (ack->pg_stat.count(pg->info.pgid.pgid)) {
      pair<version_t,epoch_t> acked = ack->pg_stat[pg->info.pgid.pgid];
      pg->pg_stats_publish_lock.Lock();
      if (acked.first == pg->pg_stats_publish.reported_seq &&
	  acked.second == pg->pg_stats_publish.reported_epoch) {
	dout(25) << " ack on " << pg->info.pgid << " " << pg->pg_stats_publish.reported_epoch
		 << ":" << pg->pg_stats_publish.reported_seq << dendl;
	pg->stat_queue_item.remove_myself();
	pg->put("pg_stat_queue");
      } else {
	dout(25) << " still pending " << pg->info.pgid << " " << pg->pg_stats_publish.reported_epoch
		 << ":" << pg->pg_stats_publish.reported_seq << " > acked " << acked << dendl;
      }
      pg->pg_stats_publish_lock.Unlock();
    } else {
      dout(30) << " still pending " << pg->info.pgid << " " << pg->pg_stats_publish.reported_epoch
	       << ":" << pg->pg_stats_publish.reported_seq << dendl;
    }
  }
  
  if (!pg_stat_queue.size()) {
    outstanding_pg_stats = false;
  }

  pg_stat_queue_lock.Unlock();

  ack->put();
}

void OSD::flush_pg_stats()
{
  dout(10) << "flush_pg_stats" << dendl;
  utime_t now = ceph_clock_now(cct);
  send_pg_stats(now);

  osd_lock.Unlock();

  pg_stat_queue_lock.Lock();
  uint64_t tid = pg_stat_tid;
  dout(10) << "flush_pg_stats waiting for stats tid " << tid << " to flush" << dendl;
  while (tid > pg_stat_tid_flushed)
    pg_stat_queue_cond.Wait(pg_stat_queue_lock);
  dout(10) << "flush_pg_stats finished waiting for stats tid " << tid << " to flush" << dendl;
  pg_stat_queue_lock.Unlock();

  osd_lock.Lock();
}


void OSD::handle_command(MMonCommand *m)
{
  if (!require_mon_peer(m))
    return;

  Command *c = new Command(m->cmd, m->get_tid(), m->get_data(), NULL);
  command_wq.queue(c);
  m->put();
}

void OSD::handle_command(MCommand *m)
{
  ConnectionRef con = m->get_connection();
  Session *session = static_cast<Session *>(con->get_priv());
  if (!session) {
    con->send_message(new MCommandReply(m, -EPERM));
    m->put();
    return;
  }

  OSDCap& caps = session->caps;
  session->put();

  if (!caps.allow_all() || m->get_source().is_mon()) {
    con->send_message(new MCommandReply(m, -EPERM));
    m->put();
    return;
  }

  Command *c = new Command(m->cmd, m->get_tid(), m->get_data(), con.get());
  command_wq.queue(c);

  m->put();
}

struct OSDCommand {
  string cmdstring;
  string helpstring;
  string module;
  string perm;
  string availability;
} osd_commands[] = {

#define COMMAND(parsesig, helptext, module, perm, availability) \
  {parsesig, helptext, module, perm, availability},

// yes, these are really pg commands, but there's a limit to how
// much work it's worth.  The OSD returns all of them.  Make this
// form (pg <pgid> <cmd>) valid only for the cli. 
// Rest uses "tell <pgid> <cmd>"

COMMAND("pg " \
	"name=pgid,type=CephPgid " \
	"name=cmd,type=CephChoices,strings=query", \
	"show details of a specific pg", "osd", "r", "cli")
COMMAND("pg " \
	"name=pgid,type=CephPgid " \
	"name=cmd,type=CephChoices,strings=mark_unfound_lost " \
	"name=mulcmd,type=CephChoices,strings=revert|delete", \
	"mark all unfound objects in this pg as lost, either removing or reverting to a prior version if one is available",
	"osd", "rw", "cli")
COMMAND("pg " \
	"name=pgid,type=CephPgid " \
	"name=cmd,type=CephChoices,strings=list_missing " \
	"name=offset,type=CephString,req=false",
	"list missing objects on this pg, perhaps starting at an offset given in JSON",
	"osd", "r", "cli")

// new form: tell <pgid> <cmd> for both cli and rest 

COMMAND("query",
	"show details of a specific pg", "osd", "r", "cli,rest")
COMMAND("mark_unfound_lost " \
	"name=mulcmd,type=CephChoices,strings=revert|delete", \
	"mark all unfound objects in this pg as lost, either removing or reverting to a prior version if one is available",
	"osd", "rw", "cli,rest")
COMMAND("list_missing " \
	"name=offset,type=CephString,req=false",
	"list missing objects on this pg, perhaps starting at an offset given in JSON",
	"osd", "r", "cli,rest")

// tell <osd.n> commands.  Validation of osd.n must be special-cased in client
COMMAND("version", "report version of OSD", "osd", "r", "cli,rest")
COMMAND("injectargs " \
	"name=injected_args,type=CephString,n=N",
	"inject configuration arguments into running OSD",
	"osd", "rw", "cli,rest")
COMMAND("cluster_log " \
	"name=level,type=CephChoices,strings=error,warning,info,debug " \
	"name=message,type=CephString,n=N",
	"log a message to the cluster log",
	"osd", "rw", "cli,rest")
COMMAND("bench " \
	"name=count,type=CephInt,req=false " \
	"name=size,type=CephInt,req=false ", \
	"OSD benchmark: write <count> <size>-byte objects, " \
	"(default 1G size 4MB). Results in log.",
	"osd", "rw", "cli,rest")
COMMAND("flush_pg_stats", "flush pg stats", "osd", "rw", "cli,rest")
COMMAND("heap " \
	"name=heapcmd,type=CephChoices,strings=dump|start_profiler|stop_profiler|release|stats", \
	"show heap usage info (available only if compiled with tcmalloc)", \
	"osd", "rw", "cli,rest")
COMMAND("debug_dump_missing " \
	"name=filename,type=CephFilepath",
	"dump missing objects to a named file", "osd", "r", "cli,rest")
COMMAND("debug kick_recovery_wq " \
	"name=delay,type=CephInt,range=0",
	"set osd_recovery_delay_start to <val>", "osd", "rw", "cli,rest")
COMMAND("cpu_profiler " \
	"name=arg,type=CephChoices,strings=status|flush",
	"run cpu profiling on daemon", "osd", "rw", "cli,rest")
COMMAND("dump_pg_recovery_stats", "dump pg recovery statistics",
	"osd", "r", "cli,rest")
COMMAND("reset_pg_recovery_stats", "reset pg recovery statistics",
	"osd", "rw", "cli,rest")
};

void OSD::do_command(Connection *con, ceph_tid_t tid, vector<string>& cmd, bufferlist& data)
{
  int r = 0;
  stringstream ss, ds;
  string rs;
  bufferlist odata;

  dout(20) << "do_command tid " << tid << " " << cmd << dendl;

  map<string, cmd_vartype> cmdmap;
  string prefix;
  string format;
  string pgidstr;
  boost::scoped_ptr<Formatter> f;

  if (cmd.empty()) {
    ss << "no command given";
    goto out;
  }

  if (!cmdmap_from_json(cmd, &cmdmap, ss)) {
    r = -EINVAL;
    goto out;
  }

  cmd_getval(cct, cmdmap, "prefix", prefix);

  if (prefix == "get_command_descriptions") {
    int cmdnum = 0;
    JSONFormatter *f = new JSONFormatter();
    f->open_object_section("command_descriptions");
    for (OSDCommand *cp = osd_commands;
	 cp < &osd_commands[ARRAY_SIZE(osd_commands)]; cp++) {

      ostringstream secname;
      secname << "cmd" << setfill('0') << std::setw(3) << cmdnum;
      dump_cmddesc_to_json(f, secname.str(), cp->cmdstring, cp->helpstring,
			   cp->module, cp->perm, cp->availability);
      cmdnum++;
    }
    f->close_section();	// command_descriptions

    f->flush(ds);
    delete f;
    goto out;
  }

  cmd_getval(cct, cmdmap, "format", format);
  f.reset(Formatter::create(format));

  if (prefix == "version") {
    if (f) {
      f->open_object_section("version");
      f->dump_string("version", pretty_version_to_str());
      f->close_section();
      f->flush(ds);
    } else {
      ds << pretty_version_to_str();
    }
    goto out;
  }
  else if (prefix == "injectargs") {
    vector<string> argsvec;
    cmd_getval(cct, cmdmap, "injected_args", argsvec);

    if (argsvec.empty()) {
      r = -EINVAL;
      ss << "ignoring empty injectargs";
      goto out;
    }
    string args = argsvec.front();
    for (vector<string>::iterator a = ++argsvec.begin(); a != argsvec.end(); ++a)
      args += " " + *a;
    osd_lock.Unlock();
    cct->_conf->injectargs(args, &ss);
    osd_lock.Lock();
  }
  else if (prefix == "cluster_log") {
    vector<string> msg;
    cmd_getval(cct, cmdmap, "message", msg);
    if (msg.empty()) {
      r = -EINVAL;
      ss << "ignoring empty log message";
      goto out;
    }
    string message = msg.front();
    for (vector<string>::iterator a = ++msg.begin(); a != msg.end(); ++a)
      message += " " + *a;
    string lvl;
    cmd_getval(cct, cmdmap, "level", lvl);
    clog_type level = string_to_clog_type(lvl);
    if (level < 0) {
      r = -EINVAL;
      ss << "unknown level '" << lvl << "'";
      goto out;
    }
    clog->do_log(level, message);
  }

  // either 'pg <pgid> <command>' or
  // 'tell <pgid>' (which comes in without any of that prefix)?

  else if (prefix == "pg" ||
	   (cmd_getval(cct, cmdmap, "pgid", pgidstr) &&
	     (prefix == "query" ||
	      prefix == "mark_unfound_lost" ||
	      prefix == "list_missing")
	   )) {
    pg_t pgid;

    if (!cmd_getval(cct, cmdmap, "pgid", pgidstr)) {
      ss << "no pgid specified";
      r = -EINVAL;
    } else if (!pgid.parse(pgidstr.c_str())) {
      ss << "couldn't parse pgid '" << pgidstr << "'";
      r = -EINVAL;
    } else {
      spg_t pcand;
      if (osdmap->get_primary_shard(pgid, &pcand) &&
	  _have_pg(pcand)) {
	PG *pg = _lookup_lock_pg(pcand);
	assert(pg);
	if (pg->is_primary()) {
	  // simulate pg <pgid> cmd= for pg->do-command
	  if (prefix != "pg")
	    cmd_putval(cct, cmdmap, "cmd", prefix);
	  r = pg->do_command(cmdmap, ss, data, odata);
	} else {
	  ss << "not primary for pgid " << pgid;

	  // send them the latest diff to ensure they realize the mapping
	  // has changed.
	  service.send_incremental_map(osdmap->get_epoch() - 1, con, osdmap);

	  // do not reply; they will get newer maps and realize they
	  // need to resend.
	  pg->unlock();
	  return;
	}
	pg->unlock();
      } else {
	ss << "i don't have pgid " << pgid;
	r = -ENOENT;
      }
    }
  }

  else if (prefix == "bench") {
    int64_t count;
    int64_t bsize;
    // default count 1G, size 4MB
    cmd_getval(cct, cmdmap, "count", count, (int64_t)1 << 30);
    cmd_getval(cct, cmdmap, "size", bsize, (int64_t)4 << 20);

    uint32_t duration = g_conf->osd_bench_duration;

    if (bsize > (int64_t) g_conf->osd_bench_max_block_size) {
      // let us limit the block size because the next checks rely on it
      // having a sane value.  If we allow any block size to be set things
      // can still go sideways.
      ss << "block 'size' values are capped at "
         << prettybyte_t(g_conf->osd_bench_max_block_size) << ". If you wish to use"
         << " a higher value, please adjust 'osd_bench_max_block_size'";
      r = -EINVAL;
      goto out;
    } else if (bsize < (int64_t) (1 << 20)) {
      // entering the realm of small block sizes.
      // limit the count to a sane value, assuming a configurable amount of
      // IOPS and duration, so that the OSD doesn't get hung up on this,
      // preventing timeouts from going off
      int64_t max_count =
        bsize * duration * g_conf->osd_bench_small_size_max_iops;
      if (count > max_count) {
        ss << "'count' values greater than " << max_count
           << " for a block size of " << prettybyte_t(bsize) << ", assuming "
           << g_conf->osd_bench_small_size_max_iops << " IOPS,"
           << " for " << duration << " seconds,"
           << " can cause ill effects on osd. "
           << " Please adjust 'osd_bench_small_size_max_iops' with a higher"
           << " value if you wish to use a higher 'count'.";
        r = -EINVAL;
        goto out;
      }
    } else {
      // 1MB block sizes are big enough so that we get more stuff done.
      // However, to avoid the osd from getting hung on this and having
      // timers being triggered, we are going to limit the count assuming
      // a configurable throughput and duration.
      // NOTE: max_count is the total amount of bytes that we believe we
      //       will be able to write during 'duration' for the given
      //       throughput.  The block size hardly impacts this unless it's
      //       way too big.  Given we already check how big the block size
      //       is, it's safe to assume everything will check out.
      int64_t max_count =
        g_conf->osd_bench_large_size_max_throughput * duration;
      if (count > max_count) {
        ss << "'count' values greater than " << max_count
           << " for a block size of " << prettybyte_t(bsize) << ", assuming "
           << prettybyte_t(g_conf->osd_bench_large_size_max_throughput) << "/s,"
           << " for " << duration << " seconds,"
           << " can cause ill effects on osd. "
           << " Please adjust 'osd_bench_large_size_max_throughput'"
           << " with a higher value if you wish to use a higher 'count'.";
        r = -EINVAL;
        goto out;
      }
    }

    dout(1) << " bench count " << count
            << " bsize " << prettybyte_t(bsize) << dendl;

    bufferlist bl;
    bufferptr bp(bsize);
    bp.zero();
    bl.push_back(bp);

    ObjectStore::Transaction *cleanupt = new ObjectStore::Transaction;

    store->sync_and_flush();
    utime_t start = ceph_clock_now(cct);
    for (int64_t pos = 0; pos < count; pos += bsize) {
      char nm[30];
      snprintf(nm, sizeof(nm), "disk_bw_test_%lld", (long long)pos);
      object_t oid(nm);
      hobject_t soid(sobject_t(oid, 0));
      ObjectStore::Transaction *t = new ObjectStore::Transaction;
      t->write(META_COLL, soid, 0, bsize, bl);
      store->queue_transaction_and_cleanup(NULL, t);
      cleanupt->remove(META_COLL, soid);
    }
    store->sync_and_flush();
    utime_t end = ceph_clock_now(cct);

    // clean up
    store->queue_transaction_and_cleanup(NULL, cleanupt);

    uint64_t rate = (double)count / (end - start);
    if (f) {
      f->open_object_section("osd_bench_results");
      f->dump_int("bytes_written", count);
      f->dump_int("blocksize", bsize);
      f->dump_float("bytes_per_sec", rate);
      f->close_section();
      f->flush(ss);
    } else {
      ss << "bench: wrote " << prettybyte_t(count)
	 << " in blocks of " << prettybyte_t(bsize) << " in "
	 << (end-start) << " sec at " << prettybyte_t(rate) << "/sec";
    }
  }

  else if (prefix == "flush_pg_stats") {
    flush_pg_stats();
  }
  
  else if (prefix == "heap") {
    if (!ceph_using_tcmalloc()) {
      r = -EOPNOTSUPP;
      ss << "could not issue heap profiler command -- not using tcmalloc!";
    } else {
      string heapcmd;
      cmd_getval(cct, cmdmap, "heapcmd", heapcmd);
      // XXX 1-element vector, change at callee or make vector here?
      vector<string> heapcmd_vec;
      get_str_vec(heapcmd, heapcmd_vec);
      ceph_heap_profiler_handle_command(heapcmd_vec, ds);
    }
  }

  else if (prefix == "debug dump_missing") {
    string file_name;
    cmd_getval(cct, cmdmap, "filename", file_name);
    std::ofstream fout(file_name.c_str());
    if (!fout.is_open()) {
	ss << "failed to open file '" << file_name << "'";
	r = -EINVAL;
	goto out;
    }

    std::set <spg_t> keys;
    RWLock::RLocker l(pg_map_lock);
    for (ceph::unordered_map<spg_t, PG*>::const_iterator pg_map_e = pg_map.begin();
	 pg_map_e != pg_map.end(); ++pg_map_e) {
      keys.insert(pg_map_e->first);
    }

    fout << "*** osd " << whoami << ": dump_missing ***" << std::endl;
    for (std::set <spg_t>::iterator p = keys.begin();
	 p != keys.end(); ++p) {
      ceph::unordered_map<spg_t, PG*>::iterator q = pg_map.find(*p);
      assert(q != pg_map.end());
      PG *pg = q->second;
      pg->lock();

      fout << *pg << std::endl;
      std::map<hobject_t, pg_missing_t::item>::const_iterator mend =
	pg->pg_log.get_missing().missing.end();
      std::map<hobject_t, pg_missing_t::item>::const_iterator mi =
	pg->pg_log.get_missing().missing.begin();
      for (; mi != mend; ++mi) {
	fout << mi->first << " -> " << mi->second << std::endl;
	if (!pg->missing_loc.needs_recovery(mi->first))
	  continue;
	if (pg->missing_loc.is_unfound(mi->first))
	  fout << " unfound ";
	const set<pg_shard_t> &mls(pg->missing_loc.get_locations(mi->first));
	if (mls.empty())
	  continue;
	fout << "missing_loc: " << mls << std::endl;
      }
      pg->unlock();
      fout << std::endl;
    }

    fout.close();
  }
  else if (prefix == "debug kick_recovery_wq") {
    int64_t delay;
    cmd_getval(cct, cmdmap, "delay", delay);
    ostringstream oss;
    oss << delay;
    r = cct->_conf->set_val("osd_recovery_delay_start", oss.str().c_str());
    if (r != 0) {
      ss << "kick_recovery_wq: error setting "
	 << "osd_recovery_delay_start to '" << delay << "': error "
	 << r;
      goto out;
    }
    cct->_conf->apply_changes(NULL);
    ss << "kicking recovery queue. set osd_recovery_delay_start "
       << "to " << cct->_conf->osd_recovery_delay_start;
    defer_recovery_until = ceph_clock_now(cct);
    defer_recovery_until += cct->_conf->osd_recovery_delay_start;
    recovery_wq.wake();
  }

  else if (prefix == "cpu_profiler") {
    string arg;
    cmd_getval(cct, cmdmap, "arg", arg);
    vector<string> argvec;
    get_str_vec(arg, argvec);
    cpu_profiler_handle_command(argvec, ds);
  }

  else if (prefix == "dump_pg_recovery_stats") {
    stringstream s;
    if (f) {
      pg_recovery_stats.dump_formatted(f.get());
      f->flush(ds);
    } else {
      pg_recovery_stats.dump(s);
      ds << "dump pg recovery stats: " << s.str();
    }
  }

  else if (prefix == "reset_pg_recovery_stats") {
    ss << "reset pg recovery stats";
    pg_recovery_stats.reset();
  }

  else {
    ss << "unrecognized command! " << cmd;
    r = -EINVAL;
  }

 out:
  rs = ss.str();
  odata.append(ds);
  dout(0) << "do_command r=" << r << " " << rs << dendl;
  clog->info() << rs << "\n";
  if (con) {
    MCommandReply *reply = new MCommandReply(r, rs);
    reply->set_tid(tid);
    reply->set_data(odata);
    con->send_message(reply);
  }
  return;
}




bool OSD::heartbeat_dispatch(Message *m)
{
  dout(30) << "heartbeat_dispatch " << m << dendl;
  switch (m->get_type()) {
    
  case CEPH_MSG_PING:
    dout(10) << "ping from " << m->get_source_inst() << dendl;
    m->put();
    break;

  case MSG_OSD_PING:
    handle_osd_ping(static_cast<MOSDPing*>(m));
    break;

  case CEPH_MSG_OSD_MAP:
    {
      ConnectionRef self = cluster_messenger->get_loopback_connection();
      self->send_message(m);
    }
    break;

  default:
    dout(0) << "dropping unexpected message " << *m << " from " << m->get_source_inst() << dendl;
    m->put();
  }

  return true;
}

bool OSD::ms_dispatch(Message *m)
{
  if (m->get_type() == MSG_OSD_MARK_ME_DOWN) {
    service.got_stop_ack();
    m->put();
    return true;
  }

  // lock!

  osd_lock.Lock();
  if (is_stopping()) {
    osd_lock.Unlock();
    m->put();
    return true;
  }

  while (dispatch_running) {
    dout(10) << "ms_dispatch waiting for other dispatch thread to complete" << dendl;
    dispatch_cond.Wait(osd_lock);
  }
  dispatch_running = true;

  do_waiters();
  _dispatch(m);
  do_waiters();

  dispatch_running = false;
  dispatch_cond.Signal();

  osd_lock.Unlock();

  return true;
}

void OSD::dispatch_session_waiting(Session *session, OSDMapRef osdmap)
{
  assert(session->session_dispatch_lock.is_locked());
  assert(session->osdmap == osdmap);
  for (list<OpRequestRef>::iterator i = session->waiting_on_map.begin();
       i != session->waiting_on_map.end() && dispatch_op_fast(*i, osdmap);
       session->waiting_on_map.erase(i++));

  if (session->waiting_on_map.empty()) {
    clear_session_waiting_on_map(session);
  } else {
    register_session_waiting_on_map(session);
  }
}


void OSD::update_waiting_for_pg(Session *session, OSDMapRef newmap)
{
  assert(session->session_dispatch_lock.is_locked());
  if (!session->osdmap) {
    session->osdmap = newmap;
    return;
  }

  if (newmap->get_epoch() == session->osdmap->get_epoch())
    return;

  assert(newmap->get_epoch() > session->osdmap->get_epoch());

  map<spg_t, list<OpRequestRef> > from;
  from.swap(session->waiting_for_pg);

  for (map<spg_t, list<OpRequestRef> >::iterator i = from.begin();
       i != from.end();
       from.erase(i++)) {
    set<spg_t> children;
    if (!newmap->have_pg_pool(i->first.pool())) {
      // drop this wait list on the ground
      i->second.clear();
    } else {
      assert(session->osdmap->have_pg_pool(i->first.pool()));
      if (i->first.is_split(
	    session->osdmap->get_pg_num(i->first.pool()),
	    newmap->get_pg_num(i->first.pool()),
	    &children)) {
	for (set<spg_t>::iterator child = children.begin();
	     child != children.end();
	     ++child) {
	  unsigned split_bits = child->get_split_bits(
	    newmap->get_pg_num(child->pool()));
	  list<OpRequestRef> child_ops;
	  OSD::split_list(&i->second, &child_ops, child->ps(), split_bits);
	  if (!child_ops.empty()) {
	    session->waiting_for_pg[*child].swap(child_ops);
	    register_session_waiting_on_pg(session, *child);
	  }
	}
      }
    }
    if (i->second.empty()) {
      clear_session_waiting_on_pg(session, i->first);
    } else {
      session->waiting_for_pg[i->first].swap(i->second);
    }
  }

  session->osdmap = newmap;
}

void OSD::session_notify_pg_create(
  Session *session, OSDMapRef osdmap, spg_t pgid)
{
  assert(session->session_dispatch_lock.is_locked());
  update_waiting_for_pg(session, osdmap);
  map<spg_t, list<OpRequestRef> >::iterator i =
    session->waiting_for_pg.find(pgid);
  if (i != session->waiting_for_pg.end()) {
    session->waiting_on_map.splice(
      session->waiting_on_map.begin(),
      i->second);
    session->waiting_for_pg.erase(i);
  }
  clear_session_waiting_on_pg(session, pgid);
}

void OSD::session_notify_pg_cleared(
  Session *session, OSDMapRef osdmap, spg_t pgid)
{
  assert(session->session_dispatch_lock.is_locked());
  update_waiting_for_pg(session, osdmap);
  session->waiting_for_pg.erase(pgid);
  clear_session_waiting_on_pg(session, pgid);
}

void OSD::ms_fast_dispatch(Message *m)
{
  if (service.is_stopping()) {
    m->put();
    return;
  }
  OpRequestRef op = op_tracker.create_request<OpRequest>(m);
  {
#ifdef WITH_LTTNG
    osd_reqid_t reqid = op->get_reqid();
#endif
    tracepoint(osd, ms_fast_dispatch, reqid.name._type,
        reqid.name._num, reqid.tid, reqid.inc);
  }
  OSDMapRef nextmap = service.get_nextmap_reserved();
  Session *session = static_cast<Session*>(m->get_connection()->get_priv());
  if (session) {
    {
      Mutex::Locker l(session->session_dispatch_lock);
      update_waiting_for_pg(session, nextmap);
      session->waiting_on_map.push_back(op);
      dispatch_session_waiting(session, nextmap);
    }
    session->put();
  }
  service.release_map(nextmap);
}

void OSD::ms_fast_preprocess(Message *m)
{
  if (m->get_connection()->get_peer_type() == CEPH_ENTITY_TYPE_OSD) {
    if (m->get_type() == CEPH_MSG_OSD_MAP) {
      MOSDMap *mm = static_cast<MOSDMap*>(m);
      Session *s = static_cast<Session*>(m->get_connection()->get_priv());
      if (s) {
	s->received_map_lock.Lock();
	s->received_map_epoch = mm->get_last();
	s->received_map_lock.Unlock();
	s->put();
      }
    }
  }
}

bool OSD::ms_get_authorizer(int dest_type, AuthAuthorizer **authorizer, bool force_new)
{
  dout(10) << "OSD::ms_get_authorizer type=" << ceph_entity_type_name(dest_type) << dendl;

  if (dest_type == CEPH_ENTITY_TYPE_MON)
    return true;

  if (force_new) {
    /* the MonClient checks keys every tick(), so we should just wait for that cycle
       to get through */
    if (monc->wait_auth_rotating(10) < 0)
      return false;
  }

  *authorizer = monc->auth->build_authorizer(dest_type);
  return *authorizer != NULL;
}


bool OSD::ms_verify_authorizer(Connection *con, int peer_type,
			       int protocol, bufferlist& authorizer_data, bufferlist& authorizer_reply,
			       bool& isvalid, CryptoKey& session_key)
{
  AuthAuthorizeHandler *authorize_handler = 0;
  switch (peer_type) {
  case CEPH_ENTITY_TYPE_MDS:
    /*
     * note: mds is technically a client from our perspective, but
     * this makes the 'cluster' consistent w/ monitor's usage.
     */
  case CEPH_ENTITY_TYPE_OSD:
    authorize_handler = authorize_handler_cluster_registry->get_handler(protocol);
    break;
  default:
    authorize_handler = authorize_handler_service_registry->get_handler(protocol);
  }
  if (!authorize_handler) {
    dout(0) << "No AuthAuthorizeHandler found for protocol " << protocol << dendl;
    isvalid = false;
    return true;
  }

  AuthCapsInfo caps_info;
  EntityName name;
  uint64_t global_id;
  uint64_t auid = CEPH_AUTH_UID_DEFAULT;

  isvalid = authorize_handler->verify_authorizer(cct, monc->rotating_secrets,
						 authorizer_data, authorizer_reply, name, global_id, caps_info, session_key, &auid);

  if (isvalid) {
    Session *s = static_cast<Session *>(con->get_priv());
    if (!s) {
      s = new Session(cct);
      con->set_priv(s->get());
      s->con = con;
      dout(10) << " new session " << s << " con=" << s->con << " addr=" << s->con->get_peer_addr() << dendl;
    }

    s->entity_name = name;
    if (caps_info.allow_all)
      s->caps.set_allow_all();
    s->auid = auid;
 
    if (caps_info.caps.length() > 0) {
      bufferlist::iterator p = caps_info.caps.begin();
      string str;
      try {
	::decode(str, p);
      }
      catch (buffer::error& e) {
      }
      bool success = s->caps.parse(str);
      if (success)
	dout(10) << " session " << s << " " << s->entity_name << " has caps " << s->caps << " '" << str << "'" << dendl;
      else
	dout(10) << " session " << s << " " << s->entity_name << " failed to parse caps '" << str << "'" << dendl;
    }
    
    s->put();
  }
  return true;
}

void OSD::do_waiters()
{
  assert(osd_lock.is_locked());
  
  dout(10) << "do_waiters -- start" << dendl;
  finished_lock.Lock();
  while (!finished.empty()) {
    OpRequestRef next = finished.front();
    finished.pop_front();
    finished_lock.Unlock();
    dispatch_op(next);
    finished_lock.Lock();
  }
  finished_lock.Unlock();
  dout(10) << "do_waiters -- finish" << dendl;
}

template<typename T, int MSGTYPE>
epoch_t replica_op_required_epoch(OpRequestRef op)
{
  T *m = static_cast<T *>(op->get_req());
  assert(m->get_type() == MSGTYPE);
  return m->map_epoch;
}

epoch_t op_required_epoch(OpRequestRef op)
{
  switch (op->get_req()->get_type()) {
  case CEPH_MSG_OSD_OP: {
    MOSDOp *m = static_cast<MOSDOp*>(op->get_req());
    return m->get_map_epoch();
  }
  case MSG_OSD_SUBOP:
    return replica_op_required_epoch<MOSDSubOp, MSG_OSD_SUBOP>(op);
  case MSG_OSD_REPOP:
    return replica_op_required_epoch<MOSDRepOp, MSG_OSD_REPOP>(op);
  case MSG_OSD_SUBOPREPLY:
    return replica_op_required_epoch<MOSDSubOpReply, MSG_OSD_SUBOPREPLY>(
      op);
  case MSG_OSD_REPOPREPLY:
    return replica_op_required_epoch<MOSDRepOpReply, MSG_OSD_REPOPREPLY>(
      op);
  case MSG_OSD_PG_PUSH:
    return replica_op_required_epoch<MOSDPGPush, MSG_OSD_PG_PUSH>(
      op);
  case MSG_OSD_PG_PULL:
    return replica_op_required_epoch<MOSDPGPull, MSG_OSD_PG_PULL>(
      op);
  case MSG_OSD_PG_PUSH_REPLY:
    return replica_op_required_epoch<MOSDPGPushReply, MSG_OSD_PG_PUSH_REPLY>(
      op);
  case MSG_OSD_PG_SCAN:
    return replica_op_required_epoch<MOSDPGScan, MSG_OSD_PG_SCAN>(op);
  case MSG_OSD_PG_BACKFILL:
    return replica_op_required_epoch<MOSDPGBackfill, MSG_OSD_PG_BACKFILL>(
      op);
  case MSG_OSD_EC_WRITE:
    return replica_op_required_epoch<MOSDECSubOpWrite, MSG_OSD_EC_WRITE>(op);
  case MSG_OSD_EC_WRITE_REPLY:
    return replica_op_required_epoch<MOSDECSubOpWriteReply, MSG_OSD_EC_WRITE_REPLY>(op);
  case MSG_OSD_EC_READ:
    return replica_op_required_epoch<MOSDECSubOpRead, MSG_OSD_EC_READ>(op);
  case MSG_OSD_EC_READ_REPLY:
    return replica_op_required_epoch<MOSDECSubOpReadReply, MSG_OSD_EC_READ_REPLY>(op);
  default:
    assert(0);
    return 0;
  }
}

void OSD::dispatch_op(OpRequestRef op)
{
  switch (op->get_req()->get_type()) {

  case MSG_OSD_PG_CREATE:
    handle_pg_create(op);
    break;

  case MSG_OSD_PG_NOTIFY:
    handle_pg_notify(op);
    break;
  case MSG_OSD_PG_QUERY:
    handle_pg_query(op);
    break;
  case MSG_OSD_PG_LOG:
    handle_pg_log(op);
    break;
  case MSG_OSD_PG_REMOVE:
    handle_pg_remove(op);
    break;
  case MSG_OSD_PG_INFO:
    handle_pg_info(op);
    break;
  case MSG_OSD_PG_TRIM:
    handle_pg_trim(op);
    break;
  case MSG_OSD_PG_MISSING:
    assert(0 ==
	   "received MOSDPGMissing; this message is supposed to be unused!?!");
    break;

  case MSG_OSD_BACKFILL_RESERVE:
    handle_pg_backfill_reserve(op);
    break;
  case MSG_OSD_RECOVERY_RESERVE:
    handle_pg_recovery_reserve(op);
    break;
  }
}

bool OSD::dispatch_op_fast(OpRequestRef& op, OSDMapRef& osdmap)
{
  if (is_stopping()) {
    // we're shutting down, so drop the op
    return true;
  }

  epoch_t msg_epoch(op_required_epoch(op));
  if (msg_epoch > osdmap->get_epoch()) {
    Session *s = static_cast<Session*>(op->get_req()->
				       get_connection()->get_priv());
    if (s) {
      s->received_map_lock.Lock();
      epoch_t received_epoch = s->received_map_epoch;
      s->received_map_lock.Unlock();
      if (received_epoch < msg_epoch) {
	osdmap_subscribe(msg_epoch, false);
      }
      s->put();
    }
    return false;
  }

  switch(op->get_req()->get_type()) {
  // client ops
  case CEPH_MSG_OSD_OP:
    handle_op(op, osdmap);
    break;
    // for replication etc.
  case MSG_OSD_SUBOP:
    handle_replica_op<MOSDSubOp, MSG_OSD_SUBOP>(op, osdmap);
    break;
  case MSG_OSD_REPOP:
    handle_replica_op<MOSDRepOp, MSG_OSD_REPOP>(op, osdmap);
    break;
  case MSG_OSD_SUBOPREPLY:
    handle_replica_op<MOSDSubOpReply, MSG_OSD_SUBOPREPLY>(op, osdmap);
    break;
  case MSG_OSD_REPOPREPLY:
    handle_replica_op<MOSDRepOpReply, MSG_OSD_REPOPREPLY>(op, osdmap);
    break;
  case MSG_OSD_PG_PUSH:
    handle_replica_op<MOSDPGPush, MSG_OSD_PG_PUSH>(op, osdmap);
    break;
  case MSG_OSD_PG_PULL:
    handle_replica_op<MOSDPGPull, MSG_OSD_PG_PULL>(op, osdmap);
    break;
  case MSG_OSD_PG_PUSH_REPLY:
    handle_replica_op<MOSDPGPushReply, MSG_OSD_PG_PUSH_REPLY>(op, osdmap);
    break;
  case MSG_OSD_PG_SCAN:
    handle_replica_op<MOSDPGScan, MSG_OSD_PG_SCAN>(op, osdmap);
    break;
  case MSG_OSD_PG_BACKFILL:
    handle_replica_op<MOSDPGBackfill, MSG_OSD_PG_BACKFILL>(op, osdmap);
    break;
  case MSG_OSD_EC_WRITE:
    handle_replica_op<MOSDECSubOpWrite, MSG_OSD_EC_WRITE>(op, osdmap);
    break;
  case MSG_OSD_EC_WRITE_REPLY:
    handle_replica_op<MOSDECSubOpWriteReply, MSG_OSD_EC_WRITE_REPLY>(op, osdmap);
    break;
  case MSG_OSD_EC_READ:
    handle_replica_op<MOSDECSubOpRead, MSG_OSD_EC_READ>(op, osdmap);
    break;
  case MSG_OSD_EC_READ_REPLY:
    handle_replica_op<MOSDECSubOpReadReply, MSG_OSD_EC_READ_REPLY>(op, osdmap);
    break;
  default:
    assert(0);
  }
  return true;
}

void OSD::_dispatch(Message *m)
{
  assert(osd_lock.is_locked());
  dout(20) << "_dispatch " << m << " " << *m << dendl;

  logger->set(l_osd_buf, buffer::get_total_alloc());

  switch (m->get_type()) {

    // -- don't need lock -- 
  case CEPH_MSG_PING:
    dout(10) << "ping from " << m->get_source() << dendl;
    m->put();
    break;

    // -- don't need OSDMap --

    // map and replication
  case CEPH_MSG_OSD_MAP:
    handle_osd_map(static_cast<MOSDMap*>(m));
    break;

    // osd
  case MSG_PGSTATSACK:
    handle_pg_stats_ack(static_cast<MPGStatsAck*>(m));
    break;

  case MSG_MON_COMMAND:
    handle_command(static_cast<MMonCommand*>(m));
    break;
  case MSG_COMMAND:
    handle_command(static_cast<MCommand*>(m));
    break;

  case MSG_OSD_SCRUB:
    handle_scrub(static_cast<MOSDScrub*>(m));
    break;

  case MSG_OSD_REP_SCRUB:
    handle_rep_scrub(static_cast<MOSDRepScrub*>(m));
    break;

    // -- need OSDMap --

  default:
    {
      OpRequestRef op = op_tracker.create_request<OpRequest, Message*>(m);
      op->mark_event("waiting_for_osdmap");
      // no map?  starting up?
      if (!osdmap) {
        dout(7) << "no OSDMap, not booted" << dendl;
        waiting_for_osdmap.push_back(op);
        break;
      }
      
      // need OSDMap
      dispatch_op(op);
    }
  }

  logger->set(l_osd_buf, buffer::get_total_alloc());

}

void OSD::handle_rep_scrub(MOSDRepScrub *m)
{
  dout(10) << __func__ << " " << *m << dendl;
  if (!require_self_aliveness(m, m->map_epoch)) {
    m->put();
    return;
  }
  if (!require_osd_peer(m)) {
    m->put();
    return;
  }
  if (osdmap->get_epoch() >= m->map_epoch &&
      !require_same_peer_instance(m, osdmap, true)) {
    m->put();
    return;
  }

  rep_scrub_wq.queue(m);
}

void OSD::handle_scrub(MOSDScrub *m)
{
  dout(10) << "handle_scrub " << *m << dendl;
  if (!require_mon_peer(m))
    return;
  if (m->fsid != monc->get_fsid()) {
    dout(0) << "handle_scrub fsid " << m->fsid << " != " << monc->get_fsid() << dendl;
    m->put();
    return;
  }

  RWLock::RLocker l(pg_map_lock);
  if (m->scrub_pgs.empty()) {
    for (ceph::unordered_map<spg_t, PG*>::iterator p = pg_map.begin();
	 p != pg_map.end();
	 ++p) {
      PG *pg = p->second;
      pg->lock();
      if (pg->is_primary()) {
	pg->unreg_next_scrub();
	pg->scrubber.must_scrub = true;
	pg->scrubber.must_deep_scrub = m->deep || m->repair;
	pg->scrubber.must_repair = m->repair;
	pg->reg_next_scrub();
	dout(10) << "marking " << *pg << " for scrub" << dendl;
      }
      pg->unlock();
    }
  } else {
    for (vector<pg_t>::iterator p = m->scrub_pgs.begin();
	 p != m->scrub_pgs.end();
	 ++p) {
      spg_t pcand;
      if (osdmap->get_primary_shard(*p, &pcand) &&
	  pg_map.count(pcand)) {
	PG *pg = pg_map[pcand];
	pg->lock();
	if (pg->is_primary()) {
	  pg->unreg_next_scrub();
	  pg->scrubber.must_scrub = true;
	  pg->scrubber.must_deep_scrub = m->deep || m->repair;
	  pg->scrubber.must_repair = m->repair;
	  pg->reg_next_scrub();
	  dout(10) << "marking " << *pg << " for scrub" << dendl;
	}
	pg->unlock();
      }
    }
  }
  
  m->put();
}

bool OSD::scrub_random_backoff()
{
  bool coin_flip = (rand() % 3) == whoami % 3;
  if (!coin_flip) {
    dout(20) << "scrub_random_backoff lost coin flip, randomly backing off" << dendl;
    return true;
  }
  return false;
}

bool OSD::scrub_time_permit(utime_t now)
{
  struct tm bdt; 
  time_t tt = now.sec();
  localtime_r(&tt, &bdt);
  bool time_permit = false;
  if (cct->_conf->osd_scrub_begin_hour < cct->_conf->osd_scrub_end_hour) {
    if (bdt.tm_hour >= cct->_conf->osd_scrub_begin_hour && bdt.tm_hour < cct->_conf->osd_scrub_end_hour) {
      time_permit = true;
    }    
  } else {
    if (bdt.tm_hour >= cct->_conf->osd_scrub_begin_hour || bdt.tm_hour < cct->_conf->osd_scrub_end_hour) {
      time_permit = true;
    }    
  }
  if (!time_permit) {
    dout(20) << "scrub_should_schedule should run between " << cct->_conf->osd_scrub_begin_hour
            << " - " << cct->_conf->osd_scrub_end_hour
            << " now " << bdt.tm_hour << " = no" << dendl;
  } else {
    dout(20) << "scrub_should_schedule should run between " << cct->_conf->osd_scrub_begin_hour
            << " - " << cct->_conf->osd_scrub_end_hour
            << " now " << bdt.tm_hour << " = yes" << dendl;
  }
  return time_permit;
}

bool OSD::scrub_should_schedule()
{
  if (!scrub_time_permit(ceph_clock_now(cct))) {
    return false;
  }
  double loadavgs[1];
  if (getloadavg(loadavgs, 1) != 1) {
    dout(10) << "scrub_should_schedule couldn't read loadavgs\n" << dendl;
    return false;
  }

  if (loadavgs[0] >= cct->_conf->osd_scrub_load_threshold) {
    dout(20) << "scrub_should_schedule loadavg " << loadavgs[0]
	     << " >= max " << cct->_conf->osd_scrub_load_threshold
	     << " = no, load too high" << dendl;
    return false;
  }

  dout(20) << "scrub_should_schedule loadavg " << loadavgs[0]
	   << " < max " << cct->_conf->osd_scrub_load_threshold
	   << " = yes" << dendl;
  return loadavgs[0] < cct->_conf->osd_scrub_load_threshold;
}

void OSD::sched_scrub()
{
  assert(osd_lock.is_locked());

  bool load_is_low = scrub_should_schedule();

  dout(20) << "sched_scrub load_is_low=" << (int)load_is_low << dendl;

  utime_t now = ceph_clock_now(cct);
  
  //dout(20) << " " << last_scrub_pg << dendl;

  pair<utime_t, spg_t> pos;
  if (service.first_scrub_stamp(&pos)) {
    do {
      utime_t t = pos.first;
      spg_t pgid = pos.second;
      dout(30) << "sched_scrub examine " << pgid << " at " << t << dendl;

      utime_t diff = now - t;
      if ((double)diff < cct->_conf->osd_scrub_min_interval) {
	dout(10) << "sched_scrub " << pgid << " at " << t
		 << ": " << (double)diff << " < min (" << cct->_conf->osd_scrub_min_interval << " seconds)" << dendl;
	break;
      }
      if ((double)diff < cct->_conf->osd_scrub_max_interval && !load_is_low) {
	// save ourselves some effort
	dout(10) << "sched_scrub " << pgid << " high load at " << t
		 << ": " << (double)diff << " < max (" << cct->_conf->osd_scrub_max_interval << " seconds)" << dendl;
	break;
      }

      PG *pg = _lookup_lock_pg(pgid);
      if (pg) {
	if (pg->get_pgbackend()->scrub_supported() && pg->is_active() &&
	    (load_is_low ||
	     (double)diff >= cct->_conf->osd_scrub_max_interval ||
	     pg->scrubber.must_scrub)) {
	  dout(10) << "sched_scrub scrubbing " << pgid << " at " << t
		   << (pg->scrubber.must_scrub ? ", explicitly requested" :
		   ( (double)diff >= cct->_conf->osd_scrub_max_interval ? ", diff >= max" : ""))
		   << dendl;
	  if (pg->sched_scrub()) {
	    pg->unlock();
	    break;
	  }
	}
	pg->unlock();
      }
    } while  (service.next_scrub_stamp(pos, &pos));
  }    
  dout(20) << "sched_scrub done" << dendl;
}



// =====================================================
// MAP

void OSD::wait_for_new_map(OpRequestRef op)
{
  // ask?
  if (waiting_for_osdmap.empty()) {
    osdmap_subscribe(osdmap->get_epoch() + 1, true);
  }
  
  logger->inc(l_osd_waiting_for_map);
  waiting_for_osdmap.push_back(op);
  op->mark_delayed("wait for new map");
}


/** update_map
 * assimilate new OSDMap(s).  scan pgs, etc.
 */

void OSD::note_down_osd(int peer)
{
  assert(osd_lock.is_locked());
  cluster_messenger->mark_down(osdmap->get_cluster_addr(peer));

  heartbeat_lock.Lock();
  failure_queue.erase(peer);
  failure_pending.erase(peer);
  map<int,HeartbeatInfo>::iterator p = heartbeat_peers.find(peer);
  if (p != heartbeat_peers.end()) {
    p->second.con_back->mark_down();
    if (p->second.con_front) {
      p->second.con_front->mark_down();
    }
    heartbeat_peers.erase(p);
  }
  heartbeat_lock.Unlock();
}

void OSD::note_up_osd(int peer)
{
  service.forget_peer_epoch(peer, osdmap->get_epoch() - 1);
}

struct C_OnMapApply : public Context {
  OSDService *service;
  boost::scoped_ptr<ObjectStore::Transaction> t;
  list<OSDMapRef> pinned_maps;
  epoch_t e;
  C_OnMapApply(OSDService *service,
	       ObjectStore::Transaction *t,
	       const list<OSDMapRef> &pinned_maps,
	       epoch_t e)
    : service(service), t(t), pinned_maps(pinned_maps), e(e) {}
  void finish(int r) {
    service->clear_map_bl_cache_pins(e);
  }
};

void OSD::osdmap_subscribe(version_t epoch, bool force_request)
{
  OSDMapRef osdmap = service.get_osdmap();
  if (osdmap->get_epoch() >= epoch)
    return;

  if (monc->sub_want_increment("osdmap", epoch, CEPH_SUBSCRIBE_ONETIME) ||
      force_request) {
    monc->renew_subs();
  }
}

void OSD::handle_osd_map(MOSDMap *m)
{
  assert(osd_lock.is_locked());
  list<OSDMapRef> pinned_maps;
  if (m->fsid != monc->get_fsid()) {
    dout(0) << "handle_osd_map fsid " << m->fsid << " != " << monc->get_fsid() << dendl;
    m->put();
    return;
  }
  if (is_initializing()) {
    dout(0) << "ignoring osdmap until we have initialized" << dendl;
    m->put();
    return;
  }

  Session *session = static_cast<Session *>(m->get_connection()->get_priv());
  if (session && !(session->entity_name.is_mon() || session->entity_name.is_osd())) {
    //not enough perms!
    dout(10) << "got osd map from Session " << session
             << " which we can't take maps from (not a mon or osd)" << dendl;
    m->put();
    session->put();
    return;
  }
  if (session)
    session->put();

  // share with the objecter
  service.objecter->handle_osd_map(m);

  epoch_t first = m->get_first();
  epoch_t last = m->get_last();
  dout(3) << "handle_osd_map epochs [" << first << "," << last << "], i have "
	  << osdmap->get_epoch()
	  << ", src has [" << m->oldest_map << "," << m->newest_map << "]"
	  << dendl;

  logger->inc(l_osd_map);
  logger->inc(l_osd_mape, last - first + 1);
  if (first <= osdmap->get_epoch())
    logger->inc(l_osd_mape_dup, osdmap->get_epoch() - first + 1);

  // make sure there is something new, here, before we bother flushing the queues and such
  if (last <= osdmap->get_epoch()) {
    dout(10) << " no new maps here, dropping" << dendl;
    m->put();
    return;
  }

  // missing some?
  bool skip_maps = false;
  if (first > osdmap->get_epoch() + 1) {
    dout(10) << "handle_osd_map message skips epochs " << osdmap->get_epoch() + 1
	     << ".." << (first-1) << dendl;
    if (m->oldest_map <= osdmap->get_epoch() + 1) {
      osdmap_subscribe(osdmap->get_epoch()+1, true);
      m->put();
      return;
    }
    // always try to get the full range of maps--as many as we can.  this
    //  1- is good to have
    //  2- is at present the only way to ensure that we get a *full* map as
    //     the first map!
    if (m->oldest_map < first) {
      osdmap_subscribe(m->oldest_map - 1, true);
      m->put();
      return;
    }
    skip_maps = true;
  }

  ObjectStore::Transaction *_t = new ObjectStore::Transaction;
  ObjectStore::Transaction &t = *_t;

  // store new maps: queue for disk and put in the osdmap cache
  epoch_t last_marked_full = 0;
  epoch_t start = MAX(osdmap->get_epoch() + 1, first);
  for (epoch_t e = start; e <= last; e++) {
    map<epoch_t,bufferlist>::iterator p;
    p = m->maps.find(e);
    if (p != m->maps.end()) {
      dout(10) << "handle_osd_map  got full map for epoch " << e << dendl;
      OSDMap *o = new OSDMap;
      bufferlist& bl = p->second;
      
      o->decode(bl);
      if (o->test_flag(CEPH_OSDMAP_FULL))
	last_marked_full = e;

      hobject_t fulloid = get_osdmap_pobject_name(e);
      t.write(META_COLL, fulloid, 0, bl.length(), bl);
      pin_map_bl(e, bl);
      pinned_maps.push_back(add_map(o));
      continue;
    }

    p = m->incremental_maps.find(e);
    if (p != m->incremental_maps.end()) {
      dout(10) << "handle_osd_map  got inc map for epoch " << e << dendl;
      bufferlist& bl = p->second;
      hobject_t oid = get_inc_osdmap_pobject_name(e);
      t.write(META_COLL, oid, 0, bl.length(), bl);
      pin_map_inc_bl(e, bl);

      OSDMap *o = new OSDMap;
      if (e > 1) {
	bufferlist obl;
	get_map_bl(e - 1, obl);
	o->decode(obl);
      }

      OSDMap::Incremental inc;
      bufferlist::iterator p = bl.begin();
      inc.decode(p);
      if (o->apply_incremental(inc) < 0) {
	derr << "ERROR: bad fsid?  i have " << osdmap->get_fsid() << " and inc has " << inc.fsid << dendl;
	assert(0 == "bad fsid");
      }

      if (o->test_flag(CEPH_OSDMAP_FULL))
	last_marked_full = e;

      bufferlist fbl;
      o->encode(fbl, inc.encode_features | CEPH_FEATURE_RESERVED);

      bool injected_failure = false;
      if (g_conf->osd_inject_bad_map_crc_probability > 0 &&
	  (rand() % 10000) < g_conf->osd_inject_bad_map_crc_probability*10000.0) {
	derr << __func__ << " injecting map crc failure" << dendl;
	injected_failure = true;
      }

      if ((inc.have_crc && o->get_crc() != inc.full_crc) || injected_failure) {
	dout(2) << "got incremental " << e
		<< " but failed to encode full with correct crc; requesting"
		<< dendl;
	clog->warn() << "failed to encode map e" << e << " with expected crc\n";
	delete o;
	MMonGetOSDMap *req = new MMonGetOSDMap;
	req->request_full(e, last);
	monc->send_mon_message(req);
	last = e - 1;
	break;
      }


      hobject_t fulloid = get_osdmap_pobject_name(e);
      t.write(META_COLL, fulloid, 0, fbl.length(), fbl);
      pin_map_bl(e, fbl);
      pinned_maps.push_back(add_map(o));
      continue;
    }

    assert(0 == "MOSDMap lied about what maps it had?");
  }

  // even if this map isn't from a mon, we may have satisfied our subscription
  monc->sub_got("osdmap", last);

  if (last <= osdmap->get_epoch()) {
    dout(10) << " no new maps here, dropping" << dendl;
    delete _t;
    m->put();
    return;
  }

  if (superblock.oldest_map) {
    int num = 0;
    epoch_t min(
      MIN(m->oldest_map,
	  service.map_cache.cached_key_lower_bound()));
    for (epoch_t e = superblock.oldest_map; e < min; ++e) {
      dout(20) << " removing old osdmap epoch " << e << dendl;
      t.remove(META_COLL, get_osdmap_pobject_name(e));
      t.remove(META_COLL, get_inc_osdmap_pobject_name(e));
      superblock.oldest_map = e+1;
      num++;
      if (num >= cct->_conf->osd_target_transaction_size &&
	  (uint64_t)num > (last - first))  // make sure we at least keep pace with incoming maps
	break;
    }
  }

  if (!superblock.oldest_map || skip_maps)
    superblock.oldest_map = first;
  superblock.newest_map = last;

  if (last_marked_full > superblock.last_map_marked_full)
    superblock.last_map_marked_full = last_marked_full;
 
  map_lock.get_write();

  C_Contexts *fin = new C_Contexts(cct);

  // advance through the new maps
  for (epoch_t cur = start; cur <= superblock.newest_map; cur++) {
    dout(10) << " advance to epoch " << cur << " (<= newest " << superblock.newest_map << ")" << dendl;

    OSDMapRef newmap = get_map(cur);
    assert(newmap);  // we just cached it above!

    // start blacklisting messages sent to peers that go down.
    service.pre_publish_map(newmap);

    // kill connections to newly down osds
    bool waited_for_reservations = false;
    set<int> old;
    osdmap->get_all_osds(old);
    for (set<int>::iterator p = old.begin(); p != old.end(); ++p) {
      if (*p != whoami &&
	  osdmap->have_inst(*p) &&                        // in old map
	  (!newmap->exists(*p) || !newmap->is_up(*p))) {  // but not the new one
        if (!waited_for_reservations) {
          service.await_reserved_maps();
          waited_for_reservations = true;
        }
	note_down_osd(*p);
      }
    }
    
    osdmap = newmap;

    superblock.current_epoch = cur;
    advance_map(t, fin);
    had_map_since = ceph_clock_now(cct);
  }

  epoch_t _bind_epoch = service.get_bind_epoch();
  if (osdmap->is_up(whoami) &&
      osdmap->get_addr(whoami) == client_messenger->get_myaddr() &&
      _bind_epoch < osdmap->get_up_from(whoami)) {

    if (is_booting()) {
      dout(1) << "state: booting -> active" << dendl;
      set_state(STATE_ACTIVE);

      // set incarnation so that osd_reqid_t's we generate for our
      // objecter requests are unique across restarts.
      service.objecter->set_client_incarnation(osdmap->get_epoch());
    }
  }

  bool do_shutdown = false;
  bool do_restart = false;
  if (osdmap->get_epoch() > 0 &&
      is_active()) {
    if (!osdmap->exists(whoami)) {
      dout(0) << "map says i do not exist.  shutting down." << dendl;
      do_shutdown = true;   // don't call shutdown() while we have everything paused
    } else if (!osdmap->is_up(whoami) ||
	       !osdmap->get_addr(whoami).probably_equals(client_messenger->get_myaddr()) ||
	       !osdmap->get_cluster_addr(whoami).probably_equals(cluster_messenger->get_myaddr()) ||
	       !osdmap->get_hb_back_addr(whoami).probably_equals(hb_back_server_messenger->get_myaddr()) ||
	       (osdmap->get_hb_front_addr(whoami) != entity_addr_t() &&
                !osdmap->get_hb_front_addr(whoami).probably_equals(hb_front_server_messenger->get_myaddr()))) {
      if (!osdmap->is_up(whoami)) {
	if (service.is_preparing_to_stop() || service.is_stopping()) {
	  service.got_stop_ack();
	} else {
	  clog->warn() << "map e" << osdmap->get_epoch()
		      << " wrongly marked me down";
	}
      }
      else if (!osdmap->get_addr(whoami).probably_equals(client_messenger->get_myaddr()))
	clog->error() << "map e" << osdmap->get_epoch()
		    << " had wrong client addr (" << osdmap->get_addr(whoami)
		     << " != my " << client_messenger->get_myaddr() << ")";
      else if (!osdmap->get_cluster_addr(whoami).probably_equals(cluster_messenger->get_myaddr()))
	clog->error() << "map e" << osdmap->get_epoch()
		    << " had wrong cluster addr (" << osdmap->get_cluster_addr(whoami)
		     << " != my " << cluster_messenger->get_myaddr() << ")";
      else if (!osdmap->get_hb_back_addr(whoami).probably_equals(hb_back_server_messenger->get_myaddr()))
	clog->error() << "map e" << osdmap->get_epoch()
		    << " had wrong hb back addr (" << osdmap->get_hb_back_addr(whoami)
		     << " != my " << hb_back_server_messenger->get_myaddr() << ")";
      else if (osdmap->get_hb_front_addr(whoami) != entity_addr_t() &&
               !osdmap->get_hb_front_addr(whoami).probably_equals(hb_front_server_messenger->get_myaddr()))
	clog->error() << "map e" << osdmap->get_epoch()
		    << " had wrong hb front addr (" << osdmap->get_hb_front_addr(whoami)
		     << " != my " << hb_front_server_messenger->get_myaddr() << ")";
      
      if (!service.is_stopping()) {
        epoch_t up_epoch = 0;
        epoch_t bind_epoch = osdmap->get_epoch();
        service.set_epochs(NULL,&up_epoch, &bind_epoch);
	do_restart = true;

	start_waiting_for_healthy();

	set<int> avoid_ports;
	avoid_ports.insert(cluster_messenger->get_myaddr().get_port());
	avoid_ports.insert(hb_back_server_messenger->get_myaddr().get_port());
	avoid_ports.insert(hb_front_server_messenger->get_myaddr().get_port());

	int r = cluster_messenger->rebind(avoid_ports);
	if (r != 0)
	  do_shutdown = true;  // FIXME: do_restart?

	r = hb_back_server_messenger->rebind(avoid_ports);
	if (r != 0)
	  do_shutdown = true;  // FIXME: do_restart?

	r = hb_front_server_messenger->rebind(avoid_ports);
	if (r != 0)
	  do_shutdown = true;  // FIXME: do_restart?

	hbclient_messenger->mark_down_all();

	reset_heartbeat_peers();
      }
    }
  }


  // note in the superblock that we were clean thru the prior epoch
  epoch_t boot_epoch = service.get_boot_epoch();
  if (boot_epoch && boot_epoch >= superblock.mounted) {
    superblock.mounted = boot_epoch;
    superblock.clean_thru = osdmap->get_epoch();
  }

  // superblock and commit
  write_superblock(t);
  store->queue_transaction(
    0,
    _t,
    new C_OnMapApply(&service, _t, pinned_maps, osdmap->get_epoch()),
    0, fin);
  service.publish_superblock(superblock);

  map_lock.put_write();

  check_osdmap_features(store);

  // yay!
  consume_map();

  if (is_active() || is_waiting_for_healthy())
    maybe_update_heartbeat_peers();

  if (!is_active()) {
    dout(10) << " not yet active; waiting for peering wq to drain" << dendl;
    peering_wq.drain();
  } else {
    activate_map();
  }

  if (m->newest_map && m->newest_map > last) {
    dout(10) << " msg say newest map is " << m->newest_map << ", requesting more" << dendl;
    osdmap_subscribe(osdmap->get_epoch()+1, true);
  }
  else if (is_booting()) {
    start_boot();  // retry
  }
  else if (do_restart)
    start_boot();

  if (do_shutdown)
    shutdown();

  m->put();
}

void OSD::check_osdmap_features(ObjectStore *fs)
{
  // adjust required feature bits?

  // we have to be a bit careful here, because we are accessing the
  // Policy structures without taking any lock.  in particular, only
  // modify integer values that can safely be read by a racing CPU.
  // since we are only accessing existing Policy structures a their
  // current memory location, and setting or clearing bits in integer
  // fields, and we are the only writer, this is not a problem.

  {
    Messenger::Policy p = client_messenger->get_default_policy();
    uint64_t mask;
    uint64_t features = osdmap->get_features(entity_name_t::TYPE_CLIENT, &mask);
    if ((p.features_required & mask) != features) {
      dout(0) << "crush map has features " << features
	      << ", adjusting msgr requires for clients" << dendl;
      p.features_required = (p.features_required & ~mask) | features;
      client_messenger->set_default_policy(p);
    }
  }
  {
    Messenger::Policy p = client_messenger->get_policy(entity_name_t::TYPE_MON);
    uint64_t mask;
    uint64_t features = osdmap->get_features(entity_name_t::TYPE_MON, &mask);
    if ((p.features_required & mask) != features) {
      dout(0) << "crush map has features " << features
	      << " was " << p.features_required
	      << ", adjusting msgr requires for mons" << dendl;
      p.features_required = (p.features_required & ~mask) | features;
      client_messenger->set_policy(entity_name_t::TYPE_MON, p);
    }
  }
  {
    Messenger::Policy p = cluster_messenger->get_policy(entity_name_t::TYPE_OSD);
    uint64_t mask;
    uint64_t features = osdmap->get_features(entity_name_t::TYPE_OSD, &mask);

    if ((p.features_required & mask) != features) {
      dout(0) << "crush map has features " << features
	      << ", adjusting msgr requires for osds" << dendl;
      p.features_required = (p.features_required & ~mask) | features;
      cluster_messenger->set_policy(entity_name_t::TYPE_OSD, p);
    }

    if ((features & CEPH_FEATURE_OSD_ERASURE_CODES) &&
	!fs->get_allow_sharded_objects()) {
      dout(0) << __func__ << " enabling on-disk ERASURE CODES compat feature" << dendl;
      superblock.compat_features.incompat.insert(CEPH_OSD_FEATURE_INCOMPAT_SHARDS);
      ObjectStore::Transaction *t = new ObjectStore::Transaction;
      write_superblock(*t);
      int err = store->queue_transaction_and_cleanup(NULL, t);
      assert(err == 0);
      fs->set_allow_sharded_objects();
    }
  }
}

bool OSD::advance_pg(
  epoch_t osd_epoch, PG *pg,
  ThreadPool::TPHandle &handle,
  PG::RecoveryCtx *rctx,
  set<boost::intrusive_ptr<PG> > *new_pgs)
{
  assert(pg->is_locked());
  epoch_t next_epoch = pg->get_osdmap()->get_epoch() + 1;
  OSDMapRef lastmap = pg->get_osdmap();

  if (lastmap->get_epoch() == osd_epoch)
    return true;
  assert(lastmap->get_epoch() < osd_epoch);

  epoch_t min_epoch = service.get_min_pg_epoch();
  epoch_t max;
  if (min_epoch) {
    max = min_epoch + g_conf->osd_map_max_advance;
  } else {
    max = next_epoch + g_conf->osd_map_max_advance;
  }

  for (;
       next_epoch <= osd_epoch && next_epoch <= max;
       ++next_epoch) {
    OSDMapRef nextmap = service.try_get_map(next_epoch);
    if (!nextmap) {
      dout(20) << __func__ << " missing map " << next_epoch << dendl;
      // make sure max is bumped up so that we can get past any
      // gap in maps
      max = MAX(max, next_epoch + g_conf->osd_map_max_advance);
      continue;
    }

    vector<int> newup, newacting;
    int up_primary, acting_primary;
    nextmap->pg_to_up_acting_osds(
      pg->info.pgid.pgid,
      &newup, &up_primary,
      &newacting, &acting_primary);
    pg->handle_advance_map(
      nextmap, lastmap, newup, up_primary,
      newacting, acting_primary, rctx);

    // Check for split!
    set<spg_t> children;
    spg_t parent(pg->info.pgid);
    if (parent.is_split(
	lastmap->get_pg_num(pg->pool.id),
	nextmap->get_pg_num(pg->pool.id),
	&children)) {
      service.mark_split_in_progress(pg->info.pgid, children);
      split_pgs(
	pg, children, new_pgs, lastmap, nextmap,
	rctx);
    }

    lastmap = nextmap;
    handle.reset_tp_timeout();
  }
  service.pg_update_epoch(pg->info.pgid, lastmap->get_epoch());
  pg->handle_activate_map(rctx);
  if (next_epoch <= osd_epoch) {
    dout(10) << __func__ << " advanced to max " << max
	     << " past min epoch " << min_epoch
	     << " ... will requeue " << *pg << dendl;
    return false;
  }
  return true;
}

/** 
 * scan placement groups, initiate any replication
 * activities.
 */
void OSD::advance_map(ObjectStore::Transaction& t, C_Contexts *tfin)
{
  assert(osd_lock.is_locked());

  dout(7) << "advance_map epoch " << osdmap->get_epoch()
          << dendl;

  epoch_t up_epoch;
  epoch_t boot_epoch;
  service.retrieve_epochs(&boot_epoch, &up_epoch, NULL);
  if (!up_epoch &&
      osdmap->is_up(whoami) &&
      osdmap->get_inst(whoami) == client_messenger->get_myinst()) {
    up_epoch = osdmap->get_epoch();
    dout(10) << "up_epoch is " << up_epoch << dendl;
    if (!boot_epoch) {
      boot_epoch = osdmap->get_epoch();
      dout(10) << "boot_epoch is " << boot_epoch << dendl;
    }
    service.set_epochs(&boot_epoch, &up_epoch, NULL);
  }

  // scan pg creations
  ceph::unordered_map<spg_t, create_pg_info>::iterator n = creating_pgs.begin();
  while (n != creating_pgs.end()) {
    ceph::unordered_map<spg_t, create_pg_info>::iterator p = n++;
    spg_t pgid = p->first;

    // am i still primary?
    vector<int> acting;
    int primary;
    osdmap->pg_to_acting_osds(pgid.pgid, &acting, &primary);
    if (primary != whoami) {
      dout(10) << " no longer primary for " << pgid << ", stopping creation" << dendl;
      creating_pgs.erase(p);
    } else {
      /*
       * adding new ppl to our pg has no effect, since we're still primary,
       * and obviously haven't given the new nodes any data.
       */
      p->second.acting.swap(acting);  // keep the latest
    }
  }
}

void OSD::consume_map()
{
  assert(osd_lock.is_locked());
  dout(7) << "consume_map version " << osdmap->get_epoch() << dendl;

  int num_pg_primary = 0, num_pg_replica = 0, num_pg_stray = 0;
  list<PGRef> to_remove;

  // scan pg's
  {
    RWLock::RLocker l(pg_map_lock);
    for (ceph::unordered_map<spg_t,PG*>::iterator it = pg_map.begin();
        it != pg_map.end();
        ++it) {
      PG *pg = it->second;
      pg->lock();
      if (pg->is_primary())
        num_pg_primary++;
      else if (pg->is_replica())
        num_pg_replica++;
      else
        num_pg_stray++;

      if (!osdmap->have_pg_pool(pg->info.pgid.pool())) {
        //pool is deleted!
        to_remove.push_back(PGRef(pg));
      } else {
        service.init_splits_between(it->first, service.get_osdmap(), osdmap);
      }

      pg->unlock();
    }
  }

  for (list<PGRef>::iterator i = to_remove.begin();
       i != to_remove.end();
       to_remove.erase(i++)) {
    RWLock::WLocker locker(pg_map_lock);
    (*i)->lock();
    _remove_pg(&**i);
    (*i)->unlock();
  }
  to_remove.clear();

  service.expand_pg_num(service.get_osdmap(), osdmap);

  service.pre_publish_map(osdmap);
  service.await_reserved_maps();
  service.publish_map(osdmap);

  dispatch_sessions_waiting_on_map();

  // remove any PGs which we no longer host from the session waiting_for_pg lists
  set<spg_t> pgs_to_check;
  get_pgs_with_waiting_sessions(&pgs_to_check);
  for (set<spg_t>::iterator p = pgs_to_check.begin();
       p != pgs_to_check.end();
       ++p) {
    vector<int> acting;
    int nrep = osdmap->pg_to_acting_osds(p->pgid, acting);
    int role = osdmap->calc_pg_role(whoami, acting, nrep);

    if (role < 0) {
      set<Session*> concerned_sessions;
      get_sessions_possibly_interested_in_pg(*p, &concerned_sessions);
      for (set<Session*>::iterator i = concerned_sessions.begin();
	   i != concerned_sessions.end();
	   ++i) {
	{
	  Mutex::Locker l((*i)->session_dispatch_lock);
	  session_notify_pg_cleared(*i, osdmap, *p);
	}
	(*i)->put();
      }
    }
  }

  // scan pg's
  {
    RWLock::RLocker l(pg_map_lock);
    for (ceph::unordered_map<spg_t,PG*>::iterator it = pg_map.begin();
        it != pg_map.end();
        ++it) {
      PG *pg = it->second;
      pg->lock();
      pg->queue_null(osdmap->get_epoch(), osdmap->get_epoch());
      pg->unlock();
    }

    logger->set(l_osd_pg, pg_map.size());
  }
  logger->set(l_osd_pg_primary, num_pg_primary);
  logger->set(l_osd_pg_replica, num_pg_replica);
  logger->set(l_osd_pg_stray, num_pg_stray);
}

void OSD::activate_map()
{
  assert(osd_lock.is_locked());

  dout(7) << "activate_map version " << osdmap->get_epoch() << dendl;

  if (osdmap->test_flag(CEPH_OSDMAP_FULL)) {
    dout(10) << " osdmap flagged full, doing onetime osdmap subscribe" << dendl;
    osdmap_subscribe(osdmap->get_epoch() + 1, true);
  }

  // norecover?
  if (osdmap->test_flag(CEPH_OSDMAP_NORECOVER)) {
    if (!paused_recovery) {
      dout(1) << "pausing recovery (NORECOVER flag set)" << dendl;
      paused_recovery = true;
      recovery_tp.pause_new();
    }
  } else {
    if (paused_recovery) {
      dout(1) << "resuming recovery (NORECOVER flag cleared)" << dendl;
      paused_recovery = false;
      recovery_tp.unpause();
    }
  }

  service.activate_map();

  // process waiters
  take_waiters(waiting_for_osdmap);
}

bool OSD::require_mon_peer(Message *m)
{
  if (!m->get_connection()->peer_is_mon()) {
    dout(0) << "require_mon_peer received from non-mon "
	    << m->get_connection()->get_peer_addr()
	    << " " << *m << dendl;
    m->put();
    return false;
  }
  return true;
}

bool OSD::require_osd_peer(Message *m)
{
  if (!m->get_connection()->peer_is_osd()) {
    dout(0) << "require_osd_peer received from non-osd "
	    << m->get_connection()->get_peer_addr()
	    << " " << *m << dendl;
    return false;
  }
  return true;
}

bool OSD::require_self_aliveness(Message *m, epoch_t epoch)
{
  epoch_t up_epoch = service.get_up_epoch();
  if (epoch < up_epoch) {
    dout(7) << "from pre-up epoch " << epoch << " < " << up_epoch << dendl;
    return false;
  }

  if (!is_active()) {
    dout(7) << "still in boot state, dropping message " << *m << dendl;
    return false;
  }

  return true;
}

bool OSD::require_same_peer_instance(Message *m, OSDMapRef& map,
				     bool is_fast_dispatch)
{
  int from = m->get_source().num();
  
  if (!map->have_inst(from) ||
      (map->get_cluster_addr(from) != m->get_source_inst().addr)) {
    dout(5) << "from dead osd." << from << ", marking down, "
	    << " msg was " << m->get_source_inst().addr
	    << " expected " << (map->have_inst(from) ?
				map->get_cluster_addr(from) : entity_addr_t())
	    << dendl;
    ConnectionRef con = m->get_connection();
    con->mark_down();
    Session *s = static_cast<Session*>(con->get_priv());
    if (s) {
      if (!is_fast_dispatch)
	s->session_dispatch_lock.Lock();
      clear_session_waiting_on_map(s);
      con->set_priv(NULL);   // break ref <-> session cycle, if any
      if (!is_fast_dispatch)
	s->session_dispatch_lock.Unlock();
      s->put();
    }
    return false;
  }
  return true;
}


/*
 * require that we have same (or newer) map, and that
 * the source is the pg primary.
 */
bool OSD::require_same_or_newer_map(OpRequestRef& op, epoch_t epoch,
				    bool is_fast_dispatch)
{
  Message *m = op->get_req();
  dout(15) << "require_same_or_newer_map " << epoch
	   << " (i am " << osdmap->get_epoch() << ") " << m << dendl;

  assert(osd_lock.is_locked());

  // do they have a newer map?
  if (epoch > osdmap->get_epoch()) {
    dout(7) << "waiting for newer map epoch " << epoch
	    << " > my " << osdmap->get_epoch() << " with " << m << dendl;
    wait_for_new_map(op);
    return false;
  }

  if (!require_self_aliveness(op->get_req(), epoch)) {
    return false;
  }

  // ok, our map is same or newer.. do they still exist?
  if (m->get_connection()->get_messenger() == cluster_messenger &&
      !require_same_peer_instance(op->get_req(), osdmap, is_fast_dispatch)) {
    return false;
  }

  return true;
}





// ----------------------------------------
// pg creation


bool OSD::can_create_pg(spg_t pgid)
{
  assert(creating_pgs.count(pgid));

  // priors empty?
  if (!creating_pgs[pgid].prior.empty()) {
    dout(10) << "can_create_pg " << pgid
	     << " - waiting for priors " << creating_pgs[pgid].prior << dendl;
    return false;
  }

  dout(10) << "can_create_pg " << pgid << " - can create now" << dendl;
  return true;
}

void OSD::split_pgs(
  PG *parent,
  const set<spg_t> &childpgids, set<boost::intrusive_ptr<PG> > *out_pgs,
  OSDMapRef curmap,
  OSDMapRef nextmap,
  PG::RecoveryCtx *rctx)
{
  unsigned pg_num = nextmap->get_pg_num(
    parent->pool.id);
  parent->update_snap_mapper_bits(
    parent->info.pgid.get_split_bits(pg_num)
    );

  vector<object_stat_sum_t> updated_stats(childpgids.size() + 1);
  parent->info.stats.stats.sum.split(updated_stats);

  vector<object_stat_sum_t>::iterator stat_iter = updated_stats.begin();
  for (set<spg_t>::const_iterator i = childpgids.begin();
       i != childpgids.end();
       ++i, ++stat_iter) {
    assert(stat_iter != updated_stats.end());
    dout(10) << "Splitting " << *parent << " into " << *i << dendl;
    assert(service.splitting(*i));
    PG* child = _make_pg(nextmap, *i);
    child->lock(true);
    out_pgs->insert(child);

    unsigned split_bits = i->get_split_bits(pg_num);
    dout(10) << "pg_num is " << pg_num << dendl;
    dout(10) << "m_seed " << i->ps() << dendl;
    dout(10) << "split_bits is " << split_bits << dendl;

    parent->split_colls(
      *i,
      split_bits,
      i->ps(),
      &child->pool.info,
      rctx->transaction);
    parent->split_into(
      i->pgid,
      child,
      split_bits);
    child->info.stats.stats.sum = *stat_iter;

    child->write_if_dirty(*(rctx->transaction));
    child->unlock();
  }
  assert(stat_iter != updated_stats.end());
  parent->info.stats.stats.sum = *stat_iter;
  parent->write_if_dirty(*(rctx->transaction));
}
  
/*
 * holding osd_lock
 */
void OSD::handle_pg_create(OpRequestRef op)
{
  MOSDPGCreate *m = (MOSDPGCreate*)op->get_req();
  assert(m->get_type() == MSG_OSD_PG_CREATE);

  dout(10) << "handle_pg_create " << *m << dendl;

  // drop the next N pg_creates in a row?
  if (debug_drop_pg_create_left < 0 &&
      cct->_conf->osd_debug_drop_pg_create_probability >
      ((((double)(rand()%100))/100.0))) {
    debug_drop_pg_create_left = debug_drop_pg_create_duration;
  }
  if (debug_drop_pg_create_left >= 0) {
    --debug_drop_pg_create_left;
    if (debug_drop_pg_create_left >= 0) {
      dout(0) << "DEBUG dropping/ignoring pg_create, will drop the next "
	      << debug_drop_pg_create_left << " too" << dendl;
      return;
    }
  }

  /* we have to hack around require_mon_peer's interface limits, so
   * grab an extra reference before going in. If the peer isn't
   * a Monitor, the reference is put for us (and then cleared
   * up automatically by our OpTracker infrastructure). Otherwise,
   * we put the extra ref ourself.
   */
  if (!require_mon_peer(op->get_req()->get())) {
    return;
  }
  op->get_req()->put();

  if (!require_same_or_newer_map(op, m->epoch, false))
    return;

  op->mark_started();

  int num_created = 0;

  map<pg_t,utime_t>::iterator ci = m->ctimes.begin();
  for (map<pg_t,pg_create_t>::iterator p = m->mkpg.begin();
       p != m->mkpg.end();
       ++p, ++ci) {
    assert(ci != m->ctimes.end() && ci->first == p->first);
    epoch_t created = p->second.created;
    pg_t parent = p->second.parent;
    if (p->second.split_bits) // Skip split pgs
      continue;
    pg_t on = p->first;

    if (on.preferred() >= 0) {
      dout(20) << "ignoring localized pg " << on << dendl;
      continue;
    }

    if (!osdmap->have_pg_pool(on.pool())) {
      dout(20) << "ignoring pg on deleted pool " << on << dendl;
      continue;
    }

    dout(20) << "mkpg " << on << " e" << created << "@" << ci->second << dendl;
   
    // is it still ours?
    vector<int> up, acting;
    int up_primary = -1;
    int acting_primary = -1;
    osdmap->pg_to_up_acting_osds(on, &up, &up_primary, &acting, &acting_primary);
    int role = osdmap->calc_pg_role(whoami, acting, acting.size());

    if (up_primary != whoami) {
      dout(10) << "mkpg " << on << "  not primary (role="
	       << role << "), skipping" << dendl;
      continue;
    }
    if (up != acting) {
      dout(10) << "mkpg " << on << "  up " << up
	       << " != acting " << acting << ", ignoring" << dendl;
      // we'll get a query soon anyway, since we know the pg
      // must exist. we can ignore this.
      continue;
    }

    spg_t pgid;
    bool mapped = osdmap->get_primary_shard(on, &pgid);
    assert(mapped);

    // does it already exist?
    if (_have_pg(pgid)) {
      dout(10) << "mkpg " << pgid << "  already exists, skipping" << dendl;
      continue;
    }

    // figure history
    pg_history_t history;
    history.epoch_created = created;
    history.last_epoch_clean = created;
    // Newly created PGs don't need to scrub immediately, so mark them
    // as scrubbed at creation time.
    if (ci->second == utime_t()) {
      // Older OSD doesn't send ctime, so just do what we did before
      // The repair_test.py can fail in a mixed cluster
      utime_t now = ceph_clock_now(NULL);
      history.last_scrub_stamp = now;
      history.last_deep_scrub_stamp = now;
    } else {
      history.last_scrub_stamp = ci->second;
      history.last_deep_scrub_stamp = ci->second;
    }
    bool valid_history = project_pg_history(
      pgid, history, created, up, up_primary, acting, acting_primary);
    /* the pg creation message must have come from a mon and therefore
     * cannot be on the other side of a map gap
     */
    assert(valid_history);
    
    // register.
    creating_pgs[pgid].history = history;
    creating_pgs[pgid].parent = parent;
    creating_pgs[pgid].acting.swap(acting);
    calc_priors_during(pgid, created, history.same_interval_since, 
		       creating_pgs[pgid].prior);

    PG::RecoveryCtx rctx = create_context();
    // poll priors
    set<pg_shard_t>& pset = creating_pgs[pgid].prior;
    dout(10) << "mkpg " << pgid << " e" << created
	     << " h " << history
	     << " : querying priors " << pset << dendl;
    for (set<pg_shard_t>::iterator p = pset.begin(); p != pset.end(); ++p)
      if (osdmap->is_up(p->osd))
	(*rctx.query_map)[p->osd][spg_t(pgid.pgid, p->shard)] =
	  pg_query_t(
	    pg_query_t::INFO,
	    p->shard, pgid.shard,
	    history,
	    osdmap->get_epoch());

    PG *pg = NULL;
    if (can_create_pg(pgid)) {
      const pg_pool_t* pp = osdmap->get_pg_pool(pgid.pool());
      PG::_create(*rctx.transaction, pgid);
      PG::_init(*rctx.transaction, pgid, pp);

      pg_interval_map_t pi;
      pg = _create_lock_pg(
	osdmap, pgid, true, false, false,
	0, creating_pgs[pgid].acting, whoami,
	creating_pgs[pgid].acting, whoami,
	history, pi,
	*rctx.transaction);
      pg->info.last_epoch_started = pg->info.history.last_epoch_started;
      creating_pgs.erase(pgid);
      pg->handle_create(&rctx);
      pg->write_if_dirty(*rctx.transaction);
      pg->publish_stats_to_osd();
      pg->unlock();
      num_created++;
      wake_pg_waiters(pg, pgid);
    }
    dispatch_context(rctx, pg, osdmap);
  }

  maybe_update_heartbeat_peers();
}


// ----------------------------------------
// peering and recovery

PG::RecoveryCtx OSD::create_context()
{
  ObjectStore::Transaction *t = new ObjectStore::Transaction;
  C_Contexts *on_applied = new C_Contexts(cct);
  C_Contexts *on_safe = new C_Contexts(cct);
  map<int, map<spg_t,pg_query_t> > *query_map =
    new map<int, map<spg_t, pg_query_t> >;
  map<int,vector<pair<pg_notify_t, pg_interval_map_t> > > *notify_list =
    new map<int, vector<pair<pg_notify_t, pg_interval_map_t> > >;
  map<int,vector<pair<pg_notify_t, pg_interval_map_t> > > *info_map =
    new map<int,vector<pair<pg_notify_t, pg_interval_map_t> > >;
  PG::RecoveryCtx rctx(query_map, info_map, notify_list,
		       on_applied, on_safe, t);
  return rctx;
}

void OSD::dispatch_context_transaction(PG::RecoveryCtx &ctx, PG *pg,
                                       ThreadPool::TPHandle *handle)
{
  if (!ctx.transaction->empty()) {
    ctx.on_applied->add(new ObjectStore::C_DeleteTransaction(ctx.transaction));
    int tr = store->queue_transaction(
      pg->osr.get(),
      ctx.transaction, ctx.on_applied, ctx.on_safe, NULL,
      TrackedOpRef(), handle);
    assert(tr == 0);
    ctx.transaction = new ObjectStore::Transaction;
    ctx.on_applied = new C_Contexts(cct);
    ctx.on_safe = new C_Contexts(cct);
  }
}

bool OSD::compat_must_dispatch_immediately(PG *pg)
{
  assert(pg->is_locked());
  set<pg_shard_t> tmpacting;
  if (!pg->actingbackfill.empty()) {
    tmpacting = pg->actingbackfill;
  } else {
    for (unsigned i = 0; i < pg->acting.size(); ++i) {
      if (pg->acting[i] == CRUSH_ITEM_NONE)
	continue;
      tmpacting.insert(
	pg_shard_t(
	  pg->acting[i],
	  pg->pool.info.ec_pool() ? shard_id_t(i) : shard_id_t::NO_SHARD));
    }
  }

  for (set<pg_shard_t>::iterator i = tmpacting.begin();
       i != tmpacting.end();
       ++i) {
    if (i->osd == whoami || i->osd == CRUSH_ITEM_NONE)
      continue;
    ConnectionRef conn =
      service.get_con_osd_cluster(i->osd, pg->get_osdmap()->get_epoch());
    if (conn && !conn->has_feature(CEPH_FEATURE_INDEP_PG_MAP)) {
      return true;
    }
  }
  return false;
}

void OSD::dispatch_context(PG::RecoveryCtx &ctx, PG *pg, OSDMapRef curmap,
                           ThreadPool::TPHandle *handle)
{
  if (service.get_osdmap()->is_up(whoami) &&
      is_active()) {
    do_notifies(*ctx.notify_list, curmap);
    do_queries(*ctx.query_map, curmap);
    do_infos(*ctx.info_map, curmap);
  }
  delete ctx.notify_list;
  delete ctx.query_map;
  delete ctx.info_map;
  if ((ctx.on_applied->empty() &&
       ctx.on_safe->empty() &&
       ctx.transaction->empty()) || !pg) {
    delete ctx.transaction;
    delete ctx.on_applied;
    delete ctx.on_safe;
  } else {
    ctx.on_applied->add(new ObjectStore::C_DeleteTransaction(ctx.transaction));
    int tr = store->queue_transaction(
      pg->osr.get(),
      ctx.transaction, ctx.on_applied, ctx.on_safe, NULL, TrackedOpRef(),
      handle);
    assert(tr == 0);
  }
}

/** do_notifies
 * Send an MOSDPGNotify to a primary, with a list of PGs that I have
 * content for, and they are primary for.
 */

void OSD::do_notifies(
  map<int,vector<pair<pg_notify_t,pg_interval_map_t> > >& notify_list,
  OSDMapRef curmap)
{
  for (map<int,
	   vector<pair<pg_notify_t,pg_interval_map_t> > >::iterator it =
	 notify_list.begin();
       it != notify_list.end();
       ++it) {
    if (!curmap->is_up(it->first)) {
      dout(20) << __func__ << " skipping down osd." << it->first << dendl;
      continue;
    }
    ConnectionRef con = service.get_con_osd_cluster(
      it->first, curmap->get_epoch());
    if (!con) {
      dout(20) << __func__ << " skipping osd." << it->first
	       << " (NULL con)" << dendl;
      continue;
    }
    service.share_map_peer(it->first, con.get(), curmap);
    if (con->has_feature(CEPH_FEATURE_INDEP_PG_MAP)) {
      dout(7) << __func__ << " osd " << it->first
	      << " on " << it->second.size() << " PGs" << dendl;
      MOSDPGNotify *m = new MOSDPGNotify(curmap->get_epoch(),
					 it->second);
      con->send_message(m);
    } else {
      dout(7) << __func__ << " osd " << it->first
	      << " sending separate messages" << dendl;
      for (vector<pair<pg_notify_t, pg_interval_map_t> >::iterator i =
	     it->second.begin();
	   i != it->second.end();
	   ++i) {
	vector<pair<pg_notify_t, pg_interval_map_t> > list(1);
	list[0] = *i;
	MOSDPGNotify *m = new MOSDPGNotify(i->first.epoch_sent,
					   list);
	con->send_message(m);
      }
    }
  }
}


/** do_queries
 * send out pending queries for info | summaries
 */
void OSD::do_queries(map<int, map<spg_t,pg_query_t> >& query_map,
		     OSDMapRef curmap)
{
  for (map<int, map<spg_t,pg_query_t> >::iterator pit = query_map.begin();
       pit != query_map.end();
       ++pit) {
    if (!curmap->is_up(pit->first)) {
      dout(20) << __func__ << " skipping down osd." << pit->first << dendl;
      continue;
    }
    int who = pit->first;
    ConnectionRef con = service.get_con_osd_cluster(who, curmap->get_epoch());
    if (!con) {
      dout(20) << __func__ << " skipping osd." << who
	       << " (NULL con)" << dendl;
      continue;
    }
    service.share_map_peer(who, con.get(), curmap);
    if (con->has_feature(CEPH_FEATURE_INDEP_PG_MAP)) {
      dout(7) << __func__ << " querying osd." << who
	      << " on " << pit->second.size() << " PGs" << dendl;
      MOSDPGQuery *m = new MOSDPGQuery(curmap->get_epoch(), pit->second);
      con->send_message(m);
    } else {
      dout(7) << __func__ << " querying osd." << who
	      << " sending seperate messages on " << pit->second.size()
	      << " PGs" << dendl;
      for (map<spg_t, pg_query_t>::iterator i = pit->second.begin();
	   i != pit->second.end();
	   ++i) {
	map<spg_t, pg_query_t> to_send;
	to_send.insert(*i);
	MOSDPGQuery *m = new MOSDPGQuery(i->second.epoch_sent, to_send);
	con->send_message(m);
      }
    }
  }
}


void OSD::do_infos(map<int,
		       vector<pair<pg_notify_t, pg_interval_map_t> > >& info_map,
		   OSDMapRef curmap)
{
  for (map<int,
	   vector<pair<pg_notify_t, pg_interval_map_t> > >::iterator p =
	 info_map.begin();
       p != info_map.end();
       ++p) { 
    if (!curmap->is_up(p->first)) {
      dout(20) << __func__ << " skipping down osd." << p->first << dendl;
      continue;
    }
    for (vector<pair<pg_notify_t,pg_interval_map_t> >::iterator i = p->second.begin();
	 i != p->second.end();
	 ++i) {
      dout(20) << __func__ << " sending info " << i->first.info
	       << " to shard " << p->first << dendl;
    }
    ConnectionRef con = service.get_con_osd_cluster(
      p->first, curmap->get_epoch());
    if (!con) {
      dout(20) << __func__ << " skipping osd." << p->first
	       << " (NULL con)" << dendl;
      continue;
    }
    service.share_map_peer(p->first, con.get(), curmap);
    if (con->has_feature(CEPH_FEATURE_INDEP_PG_MAP)) {
      MOSDPGInfo *m = new MOSDPGInfo(curmap->get_epoch());
      m->pg_list = p->second;
      con->send_message(m);
    } else {
      for (vector<pair<pg_notify_t, pg_interval_map_t> >::iterator i =
	     p->second.begin();
	   i != p->second.end();
	   ++i) {
	vector<pair<pg_notify_t, pg_interval_map_t> > to_send(1);
	to_send[0] = *i;
	MOSDPGInfo *m = new MOSDPGInfo(i->first.epoch_sent);
	m->pg_list = to_send;
	con->send_message(m);
      }
    }
  }
  info_map.clear();
}


/** PGNotify
 * from non-primary to primary
 * includes pg_info_t.
 * NOTE: called with opqueue active.
 */
void OSD::handle_pg_notify(OpRequestRef op)
{
  MOSDPGNotify *m = (MOSDPGNotify*)op->get_req();
  assert(m->get_type() == MSG_OSD_PG_NOTIFY);

  dout(7) << "handle_pg_notify from " << m->get_source() << dendl;
  int from = m->get_source().num();

  if (!require_osd_peer(op->get_req()))
    return;

  if (!require_same_or_newer_map(op, m->get_epoch(), false))
    return;

  op->mark_started();

  for (vector<pair<pg_notify_t, pg_interval_map_t> >::iterator it = m->get_pg_list().begin();
       it != m->get_pg_list().end();
       ++it) {

    if (it->first.info.pgid.preferred() >= 0) {
      dout(20) << "ignoring localized pg " << it->first.info.pgid << dendl;
      continue;
    }

    handle_pg_peering_evt(
      spg_t(it->first.info.pgid.pgid, it->first.to),
      it->first.info, it->second,
      it->first.query_epoch, pg_shard_t(from, it->first.from), true,
      PG::CephPeeringEvtRef(
	new PG::CephPeeringEvt(
	  it->first.epoch_sent, it->first.query_epoch,
	  PG::MNotifyRec(pg_shard_t(from, it->first.from), it->first,
          op->get_req()->get_connection()->get_features())))
      );
  }
}

void OSD::handle_pg_log(OpRequestRef op)
{
  MOSDPGLog *m = (MOSDPGLog*) op->get_req();
  assert(m->get_type() == MSG_OSD_PG_LOG);
  dout(7) << "handle_pg_log " << *m << " from " << m->get_source() << dendl;

  if (!require_osd_peer(op->get_req()))
    return;

  int from = m->get_source().num();
  if (!require_same_or_newer_map(op, m->get_epoch(), false))
    return;

  if (m->info.pgid.preferred() >= 0) {
    dout(10) << "ignoring localized pg " << m->info.pgid << dendl;
    return;
  }

  op->mark_started();
  handle_pg_peering_evt(
    spg_t(m->info.pgid.pgid, m->to),
    m->info, m->past_intervals, m->get_epoch(),
    pg_shard_t(from, m->from), false,
    PG::CephPeeringEvtRef(
      new PG::CephPeeringEvt(
	m->get_epoch(), m->get_query_epoch(),
	PG::MLogRec(pg_shard_t(from, m->from), m)))
    );
}

void OSD::handle_pg_info(OpRequestRef op)
{
  MOSDPGInfo *m = static_cast<MOSDPGInfo *>(op->get_req());
  assert(m->get_type() == MSG_OSD_PG_INFO);
  dout(7) << "handle_pg_info " << *m << " from " << m->get_source() << dendl;

  if (!require_osd_peer(op->get_req()))
    return;

  int from = m->get_source().num();
  if (!require_same_or_newer_map(op, m->get_epoch(), false))
    return;

  op->mark_started();

  for (vector<pair<pg_notify_t,pg_interval_map_t> >::iterator p = m->pg_list.begin();
       p != m->pg_list.end();
       ++p) {
    if (p->first.info.pgid.preferred() >= 0) {
      dout(10) << "ignoring localized pg " << p->first.info.pgid << dendl;
      continue;
    }

    handle_pg_peering_evt(
      spg_t(p->first.info.pgid.pgid, p->first.to),
      p->first.info, p->second, p->first.epoch_sent,
      pg_shard_t(from, p->first.from), false,
      PG::CephPeeringEvtRef(
	new PG::CephPeeringEvt(
	  p->first.epoch_sent, p->first.query_epoch,
	  PG::MInfoRec(
	    pg_shard_t(
	      from, p->first.from), p->first.info, p->first.epoch_sent)))
      );
  }
}

void OSD::handle_pg_trim(OpRequestRef op)
{
  MOSDPGTrim *m = (MOSDPGTrim *)op->get_req();
  assert(m->get_type() == MSG_OSD_PG_TRIM);

  dout(7) << "handle_pg_trim " << *m << " from " << m->get_source() << dendl;

  if (!require_osd_peer(op->get_req()))
    return;

  int from = m->get_source().num();
  if (!require_same_or_newer_map(op, m->epoch, false))
    return;

  if (m->pgid.preferred() >= 0) {
    dout(10) << "ignoring localized pg " << m->pgid << dendl;
    return;
  }

  op->mark_started();

  if (!_have_pg(m->pgid)) {
    dout(10) << " don't have pg " << m->pgid << dendl;
  } else {
    PG *pg = _lookup_lock_pg(m->pgid);
    if (m->epoch < pg->info.history.same_interval_since) {
      dout(10) << *pg << " got old trim to " << m->trim_to << ", ignoring" << dendl;
      pg->unlock();
      return;
    }
    assert(pg);

    if (pg->is_primary()) {
      // peer is informing us of their last_complete_ondisk
      dout(10) << *pg << " replica osd." << from << " lcod " << m->trim_to << dendl;
      pg->peer_last_complete_ondisk[pg_shard_t(from, m->pgid.shard)] =
	m->trim_to;
      if (pg->calc_min_last_complete_ondisk()) {
	dout(10) << *pg << " min lcod now " << pg->min_last_complete_ondisk << dendl;
	pg->trim_peers();
      }
    } else {
      // primary is instructing us to trim
      ObjectStore::Transaction *t = new ObjectStore::Transaction;
      PG::PGLogEntryHandler handler;
      pg->pg_log.trim(&handler, m->trim_to, pg->info);
      handler.apply(pg, t);
      pg->dirty_info = true;
      pg->write_if_dirty(*t);
      int tr = store->queue_transaction(
	pg->osr.get(), t,
	new ObjectStore::C_DeleteTransaction(t));
      assert(tr == 0);
    }
    pg->unlock();
  }
}

void OSD::handle_pg_backfill_reserve(OpRequestRef op)
{
  MBackfillReserve *m = static_cast<MBackfillReserve*>(op->get_req());
  assert(m->get_type() == MSG_OSD_BACKFILL_RESERVE);

  if (!require_osd_peer(op->get_req()))
    return;
  if (!require_same_or_newer_map(op, m->query_epoch, false))
    return;

  PG::CephPeeringEvtRef evt;
  if (m->type == MBackfillReserve::REQUEST) {
    evt = PG::CephPeeringEvtRef(
      new PG::CephPeeringEvt(
	m->query_epoch,
	m->query_epoch,
	PG::RequestBackfillPrio(m->priority)));
  } else if (m->type == MBackfillReserve::GRANT) {
    evt = PG::CephPeeringEvtRef(
      new PG::CephPeeringEvt(
	m->query_epoch,
	m->query_epoch,
	PG::RemoteBackfillReserved()));
  } else if (m->type == MBackfillReserve::REJECT) {
    evt = PG::CephPeeringEvtRef(
      new PG::CephPeeringEvt(
	m->query_epoch,
	m->query_epoch,
	PG::RemoteReservationRejected()));
  } else {
    assert(0);
  }

  if (service.splitting(m->pgid)) {
    peering_wait_for_split[m->pgid].push_back(evt);
    return;
  }

  PG *pg = 0;
  if (!_have_pg(m->pgid))
    return;

  pg = _lookup_lock_pg(m->pgid);
  assert(pg);

  pg->queue_peering_event(evt);
  pg->unlock();
}

void OSD::handle_pg_recovery_reserve(OpRequestRef op)
{
  MRecoveryReserve *m = static_cast<MRecoveryReserve*>(op->get_req());
  assert(m->get_type() == MSG_OSD_RECOVERY_RESERVE);

  if (!require_osd_peer(op->get_req()))
    return;
  if (!require_same_or_newer_map(op, m->query_epoch, false))
    return;

  PG::CephPeeringEvtRef evt;
  if (m->type == MRecoveryReserve::REQUEST) {
    evt = PG::CephPeeringEvtRef(
      new PG::CephPeeringEvt(
	m->query_epoch,
	m->query_epoch,
	PG::RequestRecovery()));
  } else if (m->type == MRecoveryReserve::GRANT) {
    evt = PG::CephPeeringEvtRef(
      new PG::CephPeeringEvt(
	m->query_epoch,
	m->query_epoch,
	PG::RemoteRecoveryReserved()));
  } else if (m->type == MRecoveryReserve::RELEASE) {
    evt = PG::CephPeeringEvtRef(
      new PG::CephPeeringEvt(
	m->query_epoch,
	m->query_epoch,
	PG::RecoveryDone()));
  } else {
    assert(0);
  }

  if (service.splitting(m->pgid)) {
    peering_wait_for_split[m->pgid].push_back(evt);
    return;
  }

  PG *pg = 0;
  if (!_have_pg(m->pgid))
    return;

  pg = _lookup_lock_pg(m->pgid);
  assert(pg);

  pg->queue_peering_event(evt);
  pg->unlock();
}


/** PGQuery
 * from primary to replica | stray
 * NOTE: called with opqueue active.
 */
void OSD::handle_pg_query(OpRequestRef op)
{
  assert(osd_lock.is_locked());

  MOSDPGQuery *m = (MOSDPGQuery*)op->get_req();
  assert(m->get_type() == MSG_OSD_PG_QUERY);

  if (!require_osd_peer(op->get_req()))
    return;

  dout(7) << "handle_pg_query from " << m->get_source() << " epoch " << m->get_epoch() << dendl;
  int from = m->get_source().num();
  
  if (!require_same_or_newer_map(op, m->get_epoch(), false))
    return;

  op->mark_started();

  map< int, vector<pair<pg_notify_t, pg_interval_map_t> > > notify_list;
  
  for (map<spg_t,pg_query_t>::iterator it = m->pg_list.begin();
       it != m->pg_list.end();
       ++it) {
    spg_t pgid = it->first;

    if (pgid.preferred() >= 0) {
      dout(10) << "ignoring localized pg " << pgid << dendl;
      continue;
    }

    if (service.splitting(pgid)) {
      peering_wait_for_split[pgid].push_back(
	PG::CephPeeringEvtRef(
	  new PG::CephPeeringEvt(
	    it->second.epoch_sent, it->second.epoch_sent,
	    PG::MQuery(pg_shard_t(from, it->second.from),
		       it->second, it->second.epoch_sent))));
      continue;
    }

    {
      RWLock::RLocker l(pg_map_lock);
      if (pg_map.count(pgid)) {
        PG *pg = 0;
        pg = _lookup_lock_pg_with_map_lock_held(pgid);
        pg->queue_query(
            it->second.epoch_sent, it->second.epoch_sent,
            pg_shard_t(from, it->second.from), it->second);
        pg->unlock();
        continue;
      }
    }

    if (!osdmap->have_pg_pool(pgid.pool()))
      continue;

    // get active crush mapping
    int up_primary, acting_primary;
    vector<int> up, acting;
    osdmap->pg_to_up_acting_osds(
      pgid.pgid, &up, &up_primary, &acting, &acting_primary);

    // same primary?
    pg_history_t history = it->second.history;
    bool valid_history = project_pg_history(
      pgid, history, it->second.epoch_sent,
      up, up_primary, acting, acting_primary);

    if (!valid_history ||
        it->second.epoch_sent < history.same_interval_since) {
      dout(10) << " pg " << pgid << " dne, and pg has changed in "
	       << history.same_interval_since
	       << " (msg from " << it->second.epoch_sent << ")" << dendl;
      continue;
    }

    dout(10) << " pg " << pgid << " dne" << dendl;
    pg_info_t empty(spg_t(pgid.pgid, it->second.to));
    /* This is racy, but that should be ok: if we complete the deletion
     * before the pg is recreated, we'll just start it off backfilling
     * instead of just empty */
    if (service.deleting_pgs.lookup(pgid))
      empty.last_backfill = hobject_t();
    if (it->second.type == pg_query_t::LOG ||
	it->second.type == pg_query_t::FULLLOG) {
      ConnectionRef con = service.get_con_osd_cluster(from, osdmap->get_epoch());
      if (con) {
	MOSDPGLog *mlog = new MOSDPGLog(
	  it->second.from, it->second.to,
	  osdmap->get_epoch(), empty,
	  it->second.epoch_sent);
	service.share_map_peer(from, con.get(), osdmap);
	con->send_message(mlog);
      }
    } else {
      notify_list[from].push_back(
	make_pair(
	  pg_notify_t(
	    it->second.from, it->second.to,
	    it->second.epoch_sent,
	    osdmap->get_epoch(),
	    empty),
	  pg_interval_map_t()));
    }
  }
  do_notifies(notify_list, osdmap);
}


void OSD::handle_pg_remove(OpRequestRef op)
{
  MOSDPGRemove *m = (MOSDPGRemove *)op->get_req();
  assert(m->get_type() == MSG_OSD_PG_REMOVE);
  assert(osd_lock.is_locked());

  if (!require_osd_peer(op->get_req()))
    return;

  dout(7) << "handle_pg_remove from " << m->get_source() << " on "
	  << m->pg_list.size() << " pgs" << dendl;
  
  if (!require_same_or_newer_map(op, m->get_epoch(), false))
    return;
  
  op->mark_started();

  for (vector<spg_t>::iterator it = m->pg_list.begin();
       it != m->pg_list.end();
       ++it) {
    spg_t pgid = *it;
    if (pgid.preferred() >= 0) {
      dout(10) << "ignoring localized pg " << pgid << dendl;
      continue;
    }
    
    RWLock::WLocker l(pg_map_lock);
    if (pg_map.count(pgid) == 0) {
      dout(10) << " don't have pg " << pgid << dendl;
      continue;
    }
    dout(5) << "queue_pg_for_deletion: " << pgid << dendl;
    PG *pg = _lookup_lock_pg_with_map_lock_held(pgid);
    pg_history_t history = pg->info.history;
    int up_primary, acting_primary;
    vector<int> up, acting;
    osdmap->pg_to_up_acting_osds(
      pgid.pgid, &up, &up_primary, &acting, &acting_primary);
    bool valid_history = project_pg_history(
      pg->info.pgid, history, pg->get_osdmap()->get_epoch(),
      up, up_primary, acting, acting_primary);
    if (valid_history &&
        history.same_interval_since <= m->get_epoch()) {
      assert(pg->get_primary().osd == m->get_source().num());
      PGRef _pg(pg);
      _remove_pg(pg);
      pg->unlock();
    } else {
      dout(10) << *pg << " ignoring remove request, pg changed in epoch "
	       << history.same_interval_since
	       << " > " << m->get_epoch() << dendl;
      pg->unlock();
    }
  }
}

void OSD::_remove_pg(PG *pg)
{
  ObjectStore::Transaction *rmt = new ObjectStore::Transaction;

  // on_removal, which calls remove_watchers_and_notifies, and the erasure from
  // the pg_map must be done together without unlocking the pg lock,
  // to avoid racing with watcher cleanup in ms_handle_reset
  // and handle_notify_timeout
  pg->on_removal(rmt);

  service.cancel_pending_splits_for_parent(pg->info.pgid);

  store->queue_transaction(
    pg->osr.get(), rmt,
    new ObjectStore::C_DeleteTransactionHolder<
      SequencerRef>(rmt, pg->osr),
    new ContainerContext<
      SequencerRef>(pg->osr));

  DeletingStateRef deleting = service.deleting_pgs.lookup_or_create(
    pg->info.pgid,
    make_pair(
      pg->info.pgid,
      PGRef(pg))
    );
  remove_wq.queue(make_pair(PGRef(pg), deleting));

  service.pg_remove_epoch(pg->info.pgid);

  // remove from map
  pg_map.erase(pg->info.pgid);
  pg->put("PGMap"); // since we've taken it out of map
}


// =========================================================
// RECOVERY

/*
 * caller holds osd_lock
 */
void OSD::check_replay_queue()
{
  assert(osd_lock.is_locked());

  utime_t now = ceph_clock_now(cct);
  list< pair<spg_t,utime_t> > pgids;
  replay_queue_lock.Lock();
  while (!replay_queue.empty() &&
	 replay_queue.front().second <= now) {
    pgids.push_back(replay_queue.front());
    replay_queue.pop_front();
  }
  replay_queue_lock.Unlock();

  for (list< pair<spg_t,utime_t> >::iterator p = pgids.begin(); p != pgids.end(); ++p) {
    spg_t pgid = p->first;
    pg_map_lock.get_read();
    if (pg_map.count(pgid)) {
      PG *pg = _lookup_lock_pg_with_map_lock_held(pgid);
      pg_map_lock.unlock();
      dout(10) << "check_replay_queue " << *pg << dendl;
      if (pg->is_active() &&
          pg->is_replay() &&
          pg->is_primary() &&
          pg->replay_until == p->second) {
        pg->replay_queued_ops();
      }
      pg->unlock();
    } else {
      pg_map_lock.unlock();
      dout(10) << "check_replay_queue pgid " << pgid << " (not found)" << dendl;
    }
  }
}

bool OSD::_recover_now()
{
  if (recovery_ops_active >= cct->_conf->osd_recovery_max_active) {
    dout(15) << "_recover_now active " << recovery_ops_active
	     << " >= max " << cct->_conf->osd_recovery_max_active << dendl;
    return false;
  }
  if (ceph_clock_now(cct) < defer_recovery_until) {
    dout(15) << "_recover_now defer until " << defer_recovery_until << dendl;
    return false;
  }

  return true;
}

void OSD::do_recovery(PG *pg, ThreadPool::TPHandle &handle)
{
  // see how many we should try to start.  note that this is a bit racy.
  recovery_wq.lock();
  int max = MIN(cct->_conf->osd_recovery_max_active - recovery_ops_active,
      cct->_conf->osd_recovery_max_single_start);
  if (max > 0) {
    dout(10) << "do_recovery can start " << max << " (" << recovery_ops_active << "/" << cct->_conf->osd_recovery_max_active
	     << " rops)" << dendl;
    recovery_ops_active += max;  // take them now, return them if we don't use them.
  } else {
    dout(10) << "do_recovery can start 0 (" << recovery_ops_active << "/" << cct->_conf->osd_recovery_max_active
	     << " rops)" << dendl;
  }
  recovery_wq.unlock();

  if (max <= 0) {
    dout(10) << "do_recovery raced and failed to start anything; requeuing " << *pg << dendl;
    recovery_wq.queue(pg);
    return;
  } else {
    pg->lock_suspend_timeout(handle);
    if (pg->deleting || !(pg->is_active() && pg->is_primary())) {
      pg->unlock();
      goto out;
    }
    
    dout(10) << "do_recovery starting " << max << " " << *pg << dendl;
#ifdef DEBUG_RECOVERY_OIDS
    dout(20) << "  active was " << recovery_oids[pg->info.pgid] << dendl;
#endif
    
    PG::RecoveryCtx rctx = create_context();
    rctx.handle = &handle;

    int started;
    bool more = pg->start_recovery_ops(max, &rctx, handle, &started);
    dout(10) << "do_recovery started " << started << "/" << max << " on " << *pg << dendl;

    /*
     * if we couldn't start any recovery ops and things are still
     * unfound, see if we can discover more missing object locations.
     * It may be that our initial locations were bad and we errored
     * out while trying to pull.
     */
    if (!more && pg->have_unfound()) {
      pg->discover_all_missing(*rctx.query_map);
      if (rctx.query_map->empty()) {
	dout(10) << "do_recovery  no luck, giving up on this pg for now" << dendl;
	recovery_wq.lock();
	recovery_wq._dequeue(pg);
	recovery_wq.unlock();
      }
    }

    pg->write_if_dirty(*rctx.transaction);
    OSDMapRef curmap = pg->get_osdmap();
    pg->unlock();
    dispatch_context(rctx, pg, curmap);
  }

 out:
  recovery_wq.lock();
  if (max > 0) {
    assert(recovery_ops_active >= max);
    recovery_ops_active -= max;
  }
  recovery_wq._wake();
  recovery_wq.unlock();
}

void OSD::start_recovery_op(PG *pg, const hobject_t& soid)
{
  recovery_wq.lock();
  dout(10) << "start_recovery_op " << *pg << " " << soid
	   << " (" << recovery_ops_active << "/" << cct->_conf->osd_recovery_max_active << " rops)"
	   << dendl;
  assert(recovery_ops_active >= 0);
  recovery_ops_active++;

#ifdef DEBUG_RECOVERY_OIDS
  dout(20) << "  active was " << recovery_oids[pg->info.pgid] << dendl;
  assert(recovery_oids[pg->info.pgid].count(soid) == 0);
  recovery_oids[pg->info.pgid].insert(soid);
#endif

  recovery_wq.unlock();
}

void OSD::finish_recovery_op(PG *pg, const hobject_t& soid, bool dequeue)
{
  recovery_wq.lock();
  dout(10) << "finish_recovery_op " << *pg << " " << soid
	   << " dequeue=" << dequeue
	   << " (" << recovery_ops_active << "/" << cct->_conf->osd_recovery_max_active << " rops)"
	   << dendl;

  // adjust count
  recovery_ops_active--;
  assert(recovery_ops_active >= 0);

#ifdef DEBUG_RECOVERY_OIDS
  dout(20) << "  active oids was " << recovery_oids[pg->info.pgid] << dendl;
  assert(recovery_oids[pg->info.pgid].count(soid));
  recovery_oids[pg->info.pgid].erase(soid);
#endif

  if (dequeue)
    recovery_wq._dequeue(pg);
  else {
    recovery_wq._queue_front(pg);
  }

  recovery_wq._wake();
  recovery_wq.unlock();
}

// =========================================================
// OPS

class C_SendMap : public GenContext<ThreadPool::TPHandle&> {
  OSD *osd;
  entity_name_t name;
  ConnectionRef con;
  OSDMapRef osdmap;
  epoch_t map_epoch;

public:
  C_SendMap(OSD *osd, entity_name_t n, const ConnectionRef& con,
            OSDMapRef& osdmap, epoch_t map_epoch) :
    osd(osd), name(n), con(con), osdmap(osdmap), map_epoch(map_epoch) {
  }

  void finish(ThreadPool::TPHandle& tp) {
    OSD::Session *session = static_cast<OSD::Session *>(
        con->get_priv());
    if (session) {
      session->sent_epoch_lock.Lock();
    }
    osd->service.share_map(
	name,
        con.get(),
        map_epoch,
        osdmap,
        session ? &session->last_sent_epoch : NULL);
    if (session) {
      session->sent_epoch_lock.Unlock();
      session->put();
    }
  }
};

struct send_map_on_destruct {
  OSD *osd;
  entity_name_t name;
  ConnectionRef con;
  OSDMapRef osdmap;
  epoch_t map_epoch;
  bool should_send;
  send_map_on_destruct(OSD *osd, Message *m,
                       OSDMapRef& osdmap, epoch_t map_epoch)
    : osd(osd), name(m->get_source()), con(m->get_connection()),
      osdmap(osdmap), map_epoch(map_epoch),
      should_send(true) { }
  ~send_map_on_destruct() {
    if (!should_send)
      return;
    osd->service.op_gen_wq.queue(new C_SendMap(osd, name, con,
					       osdmap, map_epoch));
  }
};

void OSD::handle_op(OpRequestRef& op, OSDMapRef& osdmap)
{
  MOSDOp *m = static_cast<MOSDOp*>(op->get_req());
  assert(m->get_type() == CEPH_MSG_OSD_OP);
  if (op_is_discardable(m)) {
    dout(10) << " discardable " << *m << dendl;
    return;
  }

  // we don't need encoded payload anymore
  m->clear_payload();

  // object name too long?
  unsigned max_name_len = MIN(g_conf->osd_max_object_name_len,
			      store->get_max_object_name_length());
  if (m->get_oid().name.size() > max_name_len) {
    dout(4) << "handle_op '" << m->get_oid().name << "' is longer than "
	    << max_name_len << " bytes" << dendl;
    service.reply_op_error(op, -ENAMETOOLONG);
    return;
  }

  // blacklisted?
  if (osdmap->is_blacklisted(m->get_source_addr())) {
    dout(4) << "handle_op " << m->get_source_addr() << " is blacklisted" << dendl;
    service.reply_op_error(op, -EBLACKLISTED);
    return;
  }

  // set up a map send if the Op gets blocked for some reason
  send_map_on_destruct share_map(this, m, osdmap, m->get_map_epoch());
  Session *client_session =
      static_cast<Session*>(m->get_connection()->get_priv());
  if (client_session) {
    client_session->sent_epoch_lock.Lock();
  }
  share_map.should_send = service.should_share_map(
      m->get_source(), m->get_connection().get(), m->get_map_epoch(),
      osdmap, &client_session->last_sent_epoch);
  if (client_session) {
    client_session->sent_epoch_lock.Unlock();
    client_session->put();
  }

  if (op->rmw_flags == 0) {
    int r = init_op_flags(op);
    if (r) {
      service.reply_op_error(op, r);
      return;
    }
  }

  if (cct->_conf->osd_debug_drop_op_probability > 0 &&
      !m->get_source().is_mds()) {
    if ((double)rand() / (double)RAND_MAX < cct->_conf->osd_debug_drop_op_probability) {
      dout(0) << "handle_op DEBUG artificially dropping op " << *m << dendl;
      return;
    }
  }

  if (op->may_write()) {
    // full?
    if ((service.check_failsafe_full() ||
	 osdmap->test_flag(CEPH_OSDMAP_FULL) ||
	 m->get_map_epoch() < superblock.last_map_marked_full) &&
	!m->get_source().is_mds()) {  // FIXME: we'll exclude mds writes for now.
      // Drop the request, since the client will retry when the full
      // flag is unset.
      return;
    }

    // invalid?
    if (m->get_snapid() != CEPH_NOSNAP) {
      service.reply_op_error(op, -EINVAL);
      return;
    }

    // too big?
    if (cct->_conf->osd_max_write_size &&
	m->get_data_len() > cct->_conf->osd_max_write_size << 20) {
      // journal can't hold commit!
      derr << "handle_op msg data len " << m->get_data_len()
	   << " > osd_max_write_size " << (cct->_conf->osd_max_write_size << 20)
	   << " on " << *m << dendl;
      service.reply_op_error(op, -OSD_WRITETOOBIG);
      return;
    }
  }

  // calc actual pgid
  pg_t _pgid = m->get_pg();
  int64_t pool = _pgid.pool();
  if ((m->get_flags() & CEPH_OSD_FLAG_PGOP) == 0 &&
      osdmap->have_pg_pool(pool))
    _pgid = osdmap->raw_pg_to_pg(_pgid);

  spg_t pgid;
  if (!osdmap->get_primary_shard(_pgid, &pgid)) {
    // missing pool or acting set empty -- drop
    return;
  }

  OSDMapRef send_map = service.try_get_map(m->get_map_epoch());
  // check send epoch
  if (!send_map) {
    dout(7) << "don't have sender's osdmap; assuming it was valid and that"
	    << " client will resend" << dendl;
    return;
  }
  if (!send_map->have_pg_pool(pgid.pool())) {
    dout(7) << "dropping request; pool did not exist" << dendl;
    clog->warn() << m->get_source_inst() << " invalid " << m->get_reqid()
		      << " pg " << m->get_pg()
		      << " to osd." << whoami
		      << " in e" << osdmap->get_epoch()
		      << ", client e" << m->get_map_epoch()
		      << " when pool " << m->get_pg().pool() << " did not exist"
		      << "\n";
    return;
  }
  if (!send_map->osd_is_valid_op_target(pgid.pgid, whoami)) {
    dout(7) << "we are invalid target" << dendl;
    clog->warn() << m->get_source_inst() << " misdirected " << m->get_reqid()
		      << " pg " << m->get_pg()
		      << " to osd." << whoami
		      << " in e" << osdmap->get_epoch()
		      << ", client e" << m->get_map_epoch()
		      << " pg " << pgid
		      << " features " << m->get_connection()->get_features()
		      << "\n";
    service.reply_op_error(op, -ENXIO);
    return;
  }

  // check against current map too
  if (!osdmap->have_pg_pool(pgid.pool()) ||
      !osdmap->osd_is_valid_op_target(pgid.pgid, whoami)) {
    dout(7) << "dropping; no longer have PG (or pool); client will retarget"
	    << dendl;
    return;
  }

  PG *pg = get_pg_or_queue_for_pg(pgid, op);
  if (pg) {
    op->send_map_update = share_map.should_send;
    op->sent_epoch = m->get_map_epoch();
    enqueue_op(pg, op);
    share_map.should_send = false;
  }
}

template<typename T, int MSGTYPE>
void OSD::handle_replica_op(OpRequestRef& op, OSDMapRef& osdmap)
{
  T *m = static_cast<T *>(op->get_req());
  assert(m->get_type() == MSGTYPE);

  dout(10) << __func__ << " " << *m << " epoch " << m->map_epoch << dendl;
  if (!require_self_aliveness(op->get_req(), m->map_epoch))
    return;
  if (!require_osd_peer(op->get_req()))
    return;
  if (osdmap->get_epoch() >= m->map_epoch &&
      !require_same_peer_instance(op->get_req(), osdmap, true))
    return;

  // must be a rep op.
  assert(m->get_source().is_osd());
  
  // share our map with sender, if they're old
  bool should_share_map = false;
  Session *peer_session =
      static_cast<Session*>(m->get_connection()->get_priv());
  if (peer_session) {
    peer_session->sent_epoch_lock.Lock();
  }
  should_share_map = service.should_share_map(
      m->get_source(), m->get_connection().get(), m->map_epoch,
      osdmap,
      peer_session ? &peer_session->last_sent_epoch : NULL);
  if (peer_session) {
    peer_session->sent_epoch_lock.Unlock();
    peer_session->put();
  }

  PG *pg = get_pg_or_queue_for_pg(m->pgid, op);
  if (pg) {
    op->send_map_update = should_share_map;
    op->sent_epoch = m->map_epoch;
    enqueue_op(pg, op);
  } else if (should_share_map && m->get_connection()->is_connected()) {
    C_SendMap *send_map = new C_SendMap(this, m->get_source(),
					m->get_connection(),
                                        osdmap, m->map_epoch);
    service.op_gen_wq.queue(send_map);
  }
}

bool OSD::op_is_discardable(MOSDOp *op)
{
  // drop client request if they are not connected and can't get the
  // reply anyway.  unless this is a replayed op, in which case we
  // want to do what we can to apply it.
  if (!op->get_connection()->is_connected() &&
      op->get_version().version == 0) {
    return true;
  }
  return false;
}

void OSD::enqueue_op(PG *pg, OpRequestRef& op)
{
  utime_t latency = ceph_clock_now(cct) - op->get_req()->get_recv_stamp();
  dout(15) << "enqueue_op " << op << " prio " << op->get_req()->get_priority()
	   << " cost " << op->get_req()->get_cost()
	   << " latency " << latency
	   << " " << *(op->get_req()) << dendl;
  pg->queue_op(op);
}

void OSD::ShardedOpWQ::_process(uint32_t thread_index, heartbeat_handle_d *hb ) {

  uint32_t shard_index = thread_index % num_shards;

  ShardData* sdata = shard_list[shard_index];
  assert(NULL != sdata);
  sdata->sdata_op_ordering_lock.Lock();
  if (sdata->pqueue.empty()) {
    sdata->sdata_op_ordering_lock.Unlock();
    osd->cct->get_heartbeat_map()->reset_timeout(hb, 4, 0);
    sdata->sdata_lock.Lock();
    sdata->sdata_cond.WaitInterval(osd->cct, sdata->sdata_lock, utime_t(2, 0));
    sdata->sdata_lock.Unlock();
    sdata->sdata_op_ordering_lock.Lock();
    if(sdata->pqueue.empty()) {
      sdata->sdata_op_ordering_lock.Unlock();
      return;
    }
  }
  pair<PGRef, OpRequestRef> item = sdata->pqueue.dequeue();
  sdata->pg_for_processing[&*(item.first)].push_back(item.second);
  sdata->sdata_op_ordering_lock.Unlock();
  ThreadPool::TPHandle tp_handle(osd->cct, hb, timeout_interval, 
    suicide_interval);

  (item.first)->lock_suspend_timeout(tp_handle);

  OpRequestRef op;
  {
    Mutex::Locker l(sdata->sdata_op_ordering_lock);
    if (!sdata->pg_for_processing.count(&*(item.first))) {
      (item.first)->unlock();
      return;
    }
    assert(sdata->pg_for_processing[&*(item.first)].size());
    op = sdata->pg_for_processing[&*(item.first)].front();
    sdata->pg_for_processing[&*(item.first)].pop_front();
    if (!(sdata->pg_for_processing[&*(item.first)].size()))
      sdata->pg_for_processing.erase(&*(item.first));
  }  

  // osd:opwq_process marks the point at which an operation has been dequeued
  // and will begin to be handled by a worker thread.
  {
#ifdef WITH_LTTNG
    osd_reqid_t reqid = op->get_reqid();
#endif
    tracepoint(osd, opwq_process_start, reqid.name._type,
        reqid.name._num, reqid.tid, reqid.inc);
  }

  lgeneric_subdout(osd->cct, osd, 30) << "dequeue status: ";
  Formatter *f = Formatter::create("json");
  f->open_object_section("q");
  dump(f);
  f->close_section();
  f->flush(*_dout);
  delete f;
  *_dout << dendl;

  osd->dequeue_op(item.first, op, tp_handle);

  {
#ifdef WITH_LTTNG
    osd_reqid_t reqid = op->get_reqid();
#endif
    tracepoint(osd, opwq_process_finish, reqid.name._type,
        reqid.name._num, reqid.tid, reqid.inc);
  }

  (item.first)->unlock();
}

void OSD::ShardedOpWQ::_enqueue(pair<PGRef, OpRequestRef> item) {

  uint32_t shard_index = (((item.first)->get_pgid().ps())% shard_list.size());

  ShardData* sdata = shard_list[shard_index];
  assert (NULL != sdata);
  unsigned priority = item.second->get_req()->get_priority();
  unsigned cost = item.second->get_req()->get_cost();
  sdata->sdata_op_ordering_lock.Lock();
 
  if (priority >= CEPH_MSG_PRIO_LOW)
    sdata->pqueue.enqueue_strict(
      item.second->get_req()->get_source_inst(), priority, item);
  else
    sdata->pqueue.enqueue(item.second->get_req()->get_source_inst(),
      priority, cost, item);
  sdata->sdata_op_ordering_lock.Unlock();

  sdata->sdata_lock.Lock();
  sdata->sdata_cond.SignalOne();
  sdata->sdata_lock.Unlock();

}

void OSD::ShardedOpWQ::_enqueue_front(pair<PGRef, OpRequestRef> item) {

  uint32_t shard_index = (((item.first)->get_pgid().ps())% shard_list.size());

  ShardData* sdata = shard_list[shard_index];
  assert (NULL != sdata);
  sdata->sdata_op_ordering_lock.Lock();
  if (sdata->pg_for_processing.count(&*(item.first))) {
    sdata->pg_for_processing[&*(item.first)].push_front(item.second);
    item.second = sdata->pg_for_processing[&*(item.first)].back();
    sdata->pg_for_processing[&*(item.first)].pop_back();
  }
  unsigned priority = item.second->get_req()->get_priority();
  unsigned cost = item.second->get_req()->get_cost();
  if (priority >= CEPH_MSG_PRIO_LOW)
    sdata->pqueue.enqueue_strict_front(
      item.second->get_req()->get_source_inst(),priority, item);
  else
    sdata->pqueue.enqueue_front(item.second->get_req()->get_source_inst(),
      priority, cost, item);

  sdata->sdata_op_ordering_lock.Unlock();
  sdata->sdata_lock.Lock();
  sdata->sdata_cond.SignalOne();
  sdata->sdata_lock.Unlock();

}


/*
 * NOTE: dequeue called in worker thread, with pg lock
 */
void OSD::dequeue_op(
  PGRef pg, OpRequestRef op,
  ThreadPool::TPHandle &handle)
{
  utime_t now = ceph_clock_now(cct);
  op->set_dequeued_time(now);
  utime_t latency = now - op->get_req()->get_recv_stamp();
  dout(10) << "dequeue_op " << op << " prio " << op->get_req()->get_priority()
	   << " cost " << op->get_req()->get_cost()
	   << " latency " << latency
	   << " " << *(op->get_req())
	   << " pg " << *pg << dendl;

  // share our map with sender, if they're old
  if (op->send_map_update) {
    Message *m = op->get_req();
    Session *session = static_cast<Session *>(m->get_connection()->get_priv());
    if (session) {
      session->sent_epoch_lock.Lock();
    }
    service.share_map(
        m->get_source(),
        m->get_connection().get(),
        op->sent_epoch,
        osdmap,
        session ? &session->last_sent_epoch : NULL);
    if (session) {
      session->sent_epoch_lock.Unlock();
      session->put();
    }
  }

  if (pg->deleting)
    return;

  op->mark_reached_pg();

  pg->do_request(op, handle);

  // finish
  dout(10) << "dequeue_op " << op << " finish" << dendl;
}


struct C_CompleteSplits : public Context {
  OSD *osd;
  set<boost::intrusive_ptr<PG> > pgs;
  C_CompleteSplits(OSD *osd, const set<boost::intrusive_ptr<PG> > &in)
    : osd(osd), pgs(in) {}
  void finish(int r) {
    Mutex::Locker l(osd->osd_lock);
    if (osd->is_stopping())
      return;
    PG::RecoveryCtx rctx = osd->create_context();
    set<spg_t> to_complete;
    for (set<boost::intrusive_ptr<PG> >::iterator i = pgs.begin();
	 i != pgs.end();
	 ++i) {
      osd->pg_map_lock.get_write();
      (*i)->lock();
      osd->add_newly_split_pg(&**i, &rctx);
      if (!((*i)->deleting)) {
        to_complete.insert((*i)->info.pgid);
        osd->service.complete_split(to_complete);
      }
      osd->pg_map_lock.put_write();
      osd->dispatch_context_transaction(rctx, &**i);
	to_complete.insert((*i)->info.pgid);
      (*i)->unlock();
      osd->wake_pg_waiters(&**i, (*i)->info.pgid);
      to_complete.clear();
    }

    osd->dispatch_context(rctx, 0, osd->service.get_osdmap());
  }
};

void OSD::process_peering_events(
  const list<PG*> &pgs,
  ThreadPool::TPHandle &handle
  )
{
  bool need_up_thru = false;
  epoch_t same_interval_since = 0;
  OSDMapRef curmap = service.get_osdmap();
  PG::RecoveryCtx rctx = create_context();
  rctx.handle = &handle;
  for (list<PG*>::const_iterator i = pgs.begin();
       i != pgs.end();
       ++i) {
    set<boost::intrusive_ptr<PG> > split_pgs;
    PG *pg = *i;
    pg->lock_suspend_timeout(handle);
    curmap = service.get_osdmap();
    if (pg->deleting) {
      pg->unlock();
      continue;
    }
    if (!advance_pg(curmap->get_epoch(), pg, handle, &rctx, &split_pgs)) {
      // we need to requeue the PG explicitly since we didn't actually
      // handle an event
      peering_wq.queue(pg);
    } else {
      assert(!pg->peering_queue.empty());
      PG::CephPeeringEvtRef evt = pg->peering_queue.front();
      pg->peering_queue.pop_front();
      pg->handle_peering_event(evt, &rctx);
    }
    need_up_thru = pg->need_up_thru || need_up_thru;
    same_interval_since = MAX(pg->info.history.same_interval_since,
			      same_interval_since);
    pg->write_if_dirty(*rctx.transaction);
    if (!split_pgs.empty()) {
      rctx.on_applied->add(new C_CompleteSplits(this, split_pgs));
      split_pgs.clear();
    }
    if (compat_must_dispatch_immediately(pg)) {
      dispatch_context(rctx, pg, curmap, &handle);
      rctx = create_context();
      rctx.handle = &handle;
    } else {
      dispatch_context_transaction(rctx, pg, &handle);
    }
    pg->unlock();
    handle.reset_tp_timeout();
  }
  if (need_up_thru)
    queue_want_up_thru(same_interval_since);
  dispatch_context(rctx, 0, curmap, &handle);

  service.send_pg_temp();
}

// --------------------------------

const char** OSD::get_tracked_conf_keys() const
{
  static const char* KEYS[] = {
    "osd_max_backfills",
    "osd_min_recovery_priority",
    "osd_op_complaint_time", "osd_op_log_threshold",
    "osd_op_history_size", "osd_op_history_duration",
    "osd_map_cache_size",
    "osd_map_max_advance",
    "osd_pg_epoch_persisted_max_stale",
    "osd_disk_thread_ioprio_class",
    "osd_disk_thread_ioprio_priority",
    // clog & admin clog
    "clog_to_monitors",
    "clog_to_syslog",
    "clog_to_syslog_facility",
    "clog_to_syslog_level",
    NULL
  };
  return KEYS;
}

void OSD::handle_conf_change(const struct md_config_t *conf,
			     const std::set <std::string> &changed)
{
  if (changed.count("osd_max_backfills")) {
    service.local_reserver.set_max(cct->_conf->osd_max_backfills);
    service.remote_reserver.set_max(cct->_conf->osd_max_backfills);
  }
  if (changed.count("osd_min_recovery_priority")) {
    service.local_reserver.set_min_priority(cct->_conf->osd_min_recovery_priority);
    service.remote_reserver.set_min_priority(cct->_conf->osd_min_recovery_priority);
  }
  if (changed.count("osd_op_complaint_time") ||
      changed.count("osd_op_log_threshold")) {
    op_tracker.set_complaint_and_threshold(cct->_conf->osd_op_complaint_time,
                                           cct->_conf->osd_op_log_threshold);
  }
  if (changed.count("osd_op_history_size") ||
      changed.count("osd_op_history_duration")) {
    op_tracker.set_history_size_and_duration(cct->_conf->osd_op_history_size,
                                             cct->_conf->osd_op_history_duration);
  }
  if (changed.count("osd_disk_thread_ioprio_class") ||
      changed.count("osd_disk_thread_ioprio_priority")) {
    set_disk_tp_priority();
  }
  if (changed.count("osd_map_cache_size")) {
    service.map_cache.set_size(cct->_conf->osd_map_cache_size);
    service.map_bl_cache.set_size(cct->_conf->osd_map_cache_size);
    service.map_bl_inc_cache.set_size(cct->_conf->osd_map_cache_size);
  }
  if (changed.count("clog_to_monitors") ||
      changed.count("clog_to_syslog") ||
      changed.count("clog_to_syslog_level") ||
      changed.count("clog_to_syslog_facility")) {
    update_log_config();
  }

  check_config();
}

void OSD::update_log_config()
{
  map<string,string> log_to_monitors;
  map<string,string> log_to_syslog;
  map<string,string> log_channel;
  map<string,string> log_prio;
  if (parse_log_client_options(g_ceph_context, log_to_monitors, log_to_syslog,
			       log_channel, log_prio) == 0)
    clog->update_config(log_to_monitors, log_to_syslog,
			log_channel, log_prio);
  derr << "log_to_monitors " << log_to_monitors << dendl;
}

void OSD::check_config()
{
  // some sanity checks
  if (g_conf->osd_map_cache_size <= g_conf->osd_map_max_advance + 2) {
    clog->warn() << "osd_map_cache_size (" << g_conf->osd_map_cache_size << ")"
		<< " is not > osd_map_max_advance ("
		<< g_conf->osd_map_max_advance << ")";
  }
  if (g_conf->osd_map_cache_size <= (int)g_conf->osd_pg_epoch_persisted_max_stale + 2) {
    clog->warn() << "osd_map_cache_size (" << g_conf->osd_map_cache_size << ")"
		<< " is not > osd_pg_epoch_persisted_max_stale ("
		<< g_conf->osd_pg_epoch_persisted_max_stale << ")";
  }
}

void OSD::set_disk_tp_priority()
{
  dout(10) << __func__
	   << " class " << cct->_conf->osd_disk_thread_ioprio_class
	   << " priority " << cct->_conf->osd_disk_thread_ioprio_priority
	   << dendl;
  if (cct->_conf->osd_disk_thread_ioprio_class.empty() ||
      cct->_conf->osd_disk_thread_ioprio_priority < 0)
    return;
  int cls =
    ceph_ioprio_string_to_class(cct->_conf->osd_disk_thread_ioprio_class);
  if (cls < 0)
    derr << __func__ << cpp_strerror(cls) << ": "
	 << "osd_disk_thread_ioprio_class is " << cct->_conf->osd_disk_thread_ioprio_class
	 << " but only the following values are allowed: idle, be or rt" << dendl;
  else
    disk_tp.set_ioprio(cls, cct->_conf->osd_disk_thread_ioprio_priority);
}

// --------------------------------

void OSD::get_latest_osdmap()
{
  dout(10) << __func__ << " -- start" << dendl;

  C_SaferCond cond;
  service.objecter->wait_for_latest_osdmap(&cond);
  cond.wait();

  dout(10) << __func__ << " -- finish" << dendl;
}

// --------------------------------

int OSD::init_op_flags(OpRequestRef& op)
{
  MOSDOp *m = static_cast<MOSDOp*>(op->get_req());
  vector<OSDOp>::iterator iter;

  // client flags have no bearing on whether an op is a read, write, etc.
  op->rmw_flags = 0;

  // set bits based on op codes, called methods.
  for (iter = m->ops.begin(); iter != m->ops.end(); ++iter) {
    if (ceph_osd_op_mode_modify(iter->op.op))
      op->set_write();
    if (ceph_osd_op_mode_read(iter->op.op))
      op->set_read();

    // set READ flag if there are src_oids
    if (iter->soid.oid.name.length())
      op->set_read();

    // set PGOP flag if there are PG ops
    if (ceph_osd_op_type_pg(iter->op.op))
      op->set_pg_op();

    if (ceph_osd_op_mode_cache(iter->op.op))
      op->set_cache();

    switch (iter->op.op) {
    case CEPH_OSD_OP_CALL:
      {
	bufferlist::iterator bp = iter->indata.begin();
	int is_write, is_read;
	string cname, mname;
	bp.copy(iter->op.cls.class_len, cname);
	bp.copy(iter->op.cls.method_len, mname);

	ClassHandler::ClassData *cls;
	int r = class_handler->open_class(cname, &cls);
	if (r) {
	  derr << "class " << cname << " open got " << cpp_strerror(r) << dendl;
	  if (r == -ENOENT)
	    r = -EOPNOTSUPP;
	  else
	    r = -EIO;
	  return r;
	}
	int flags = cls->get_method_flags(mname.c_str());
	if (flags < 0) {
	  if (flags == -ENOENT)
	    r = -EOPNOTSUPP;
	  else
	    r = flags;
	  return r;
	}
	is_read = flags & CLS_METHOD_RD;
	is_write = flags & CLS_METHOD_WR;

	dout(10) << "class " << cname << " method " << mname
		<< " flags=" << (is_read ? "r" : "") << (is_write ? "w" : "") << dendl;
	if (is_read)
	  op->set_class_read();
	if (is_write)
	  op->set_class_write();
	break;
      }

    case CEPH_OSD_OP_WATCH:
      // force the read bit for watch since it is depends on previous
      // watch state (and may return early if the watch exists) or, in
      // the case of ping, is simply a read op.
      op->set_read();
      // fall through
    case CEPH_OSD_OP_NOTIFY:
    case CEPH_OSD_OP_NOTIFY_ACK:
      {
        op->set_promote();
        break;
      }

    default:
      break;
    }
  }

  if (op->rmw_flags == 0)
    return -EINVAL;

  return 0;
}

bool OSD::RecoveryWQ::_enqueue(PG *pg) {
  if (!pg->recovery_item.is_on_list()) {
    pg->get("RecoveryWQ");
    osd->recovery_queue.push_back(&pg->recovery_item);

    if (osd->cct->_conf->osd_recovery_delay_start > 0) {
      osd->defer_recovery_until = ceph_clock_now(osd->cct);
      osd->defer_recovery_until += osd->cct->_conf->osd_recovery_delay_start;
    }
    return true;
  }
  return false;
}

void OSD::PeeringWQ::_dequeue(list<PG*> *out) {
  set<PG*> got;
  for (list<PG*>::iterator i = peering_queue.begin();
      i != peering_queue.end() &&
      out->size() < osd->cct->_conf->osd_peering_wq_batch_size;
      ) {
        if (in_use.count(*i)) {
          ++i;
        } else {
          out->push_back(*i);
          got.insert(*i);
          peering_queue.erase(i++);
        }
  }
  in_use.insert(got.begin(), got.end());
}
