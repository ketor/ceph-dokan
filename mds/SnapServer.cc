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

#include "SnapServer.h"
#include "MDS.h"
#include "osd/OSDMap.h"
#include "osdc/Objecter.h"
#include "mon/MonClient.h"

#include "include/types.h"
#include "messages/MMDSTableRequest.h"
#include "messages/MRemoveSnaps.h"

#include "msg/Messenger.h"

#include "common/config.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix *_dout << "mds." << rank << ".snap "


void SnapServer::reset_state()
{
  last_snap = 1;  /* snapid 1 reserved for initial root snaprealm */
  snaps.clear();
  need_to_purge.clear();
}


// SERVER

void SnapServer::_prepare(bufferlist &bl, uint64_t reqid, mds_rank_t bymds)
{
  bufferlist::iterator p = bl.begin();
  __u32 op;
  ::decode(op, p);

  switch (op) {
  case TABLE_OP_CREATE:
    {
      version++;

      SnapInfo info;
      ::decode(info.ino, p);
      if (!p.end()) {
	::decode(info.name, p);
	::decode(info.stamp, p);
	info.snapid = ++last_snap;
	pending_create[version] = info;
	dout(10) << "prepare v" << version << " create " << info << dendl;
      } else {
	pending_noop.insert(version);
	dout(10) << "prepare v" << version << " noop" << dendl;
      }
      bl.clear();
      ::encode(last_snap, bl);
    }
    break;

  case TABLE_OP_DESTROY:
    {
      inodeno_t ino;
      snapid_t snapid;
      ::decode(ino, p);    // not used, currently.
      ::decode(snapid, p);
      version++;

      // bump last_snap... we use it as a version value on the snaprealm.
      ++last_snap;

      pending_destroy[version] = pair<snapid_t,snapid_t>(snapid, last_snap);
      dout(10) << "prepare v" << version << " destroy " << snapid << " seq " << last_snap << dendl;

      bl.clear();
      ::encode(last_snap, bl);
    }
    break;

  default:
    assert(0);
  }
  //dump();
}

bool SnapServer::_is_prepared(version_t tid)
{
  return 
    pending_create.count(tid) ||
    pending_destroy.count(tid);
}

bool SnapServer::_commit(version_t tid, MMDSTableRequest *req)
{
  if (pending_create.count(tid)) {
    dout(7) << "commit " << tid << " create " << pending_create[tid] << dendl;
    snaps[pending_create[tid].snapid] = pending_create[tid];
    pending_create.erase(tid);
  } 

  else if (pending_destroy.count(tid)) {
    snapid_t sn = pending_destroy[tid].first;
    snapid_t seq = pending_destroy[tid].second;
    dout(7) << "commit " << tid << " destroy " << sn << " seq " << seq << dendl;
    snaps.erase(sn);

    for (set<int64_t>::const_iterator p = mds->mdsmap->get_data_pools().begin();
	 p != mds->mdsmap->get_data_pools().end();
	 ++p) {
      need_to_purge[*p].insert(sn);
      need_to_purge[*p].insert(seq);
    }

    pending_destroy.erase(tid);
  }
  else if (pending_noop.count(tid)) {
    dout(7) << "commit " << tid << " noop" << dendl;
    pending_noop.erase(tid);
  }
  else
    assert(0);

  // bump version.
  version++;
  //dump();
  return true;
}

void SnapServer::_rollback(version_t tid) 
{
  if (pending_create.count(tid)) {
    dout(7) << "rollback " << tid << " create " << pending_create[tid] << dendl;
    pending_create.erase(tid);
  } 

  else if (pending_destroy.count(tid)) {
    dout(7) << "rollback " << tid << " destroy " << pending_destroy[tid] << dendl;
    pending_destroy.erase(tid);
  }
  
  else if (pending_noop.count(tid)) {
    dout(7) << "rollback " << tid << " noop" << dendl;
    pending_noop.erase(tid);
  }    

  else
    assert(0);

  // bump version.
  version++;
  //dump();
}

void SnapServer::_server_update(bufferlist& bl)
{
  bufferlist::iterator p = bl.begin();
  map<int, vector<snapid_t> > purge;
  ::decode(purge, p);

  dout(7) << "_server_update purged " << purge << dendl;
  for (map<int, vector<snapid_t> >::iterator p = purge.begin();
       p != purge.end();
       ++p) {
    for (vector<snapid_t>::iterator q = p->second.begin();
	 q != p->second.end();
	 ++q)
      need_to_purge[p->first].erase(*q);
    if (need_to_purge[p->first].empty())
      need_to_purge.erase(p->first);
  }

  version++;
}

void SnapServer::handle_query(MMDSTableRequest *req)
{
  req->put();
}



void SnapServer::check_osd_map(bool force)
{
  if (!force && version == last_checked_osdmap) {
    dout(10) << "check_osd_map - version unchanged" << dendl;
    return;
  }
  dout(10) << "check_osd_map need_to_purge=" << need_to_purge << dendl;

  map<int, vector<snapid_t> > all_purge;
  map<int, vector<snapid_t> > all_purged;

  const OSDMap *osdmap = mds->objecter->get_osdmap_read();
  for (map<int, set<snapid_t> >::iterator p = need_to_purge.begin();
       p != need_to_purge.end();
       ++p) {
    int id = p->first;
    const pg_pool_t *pi = osdmap->get_pg_pool(id);
    for (set<snapid_t>::iterator q = p->second.begin();
	 q != p->second.end();
	 ++q) {
      if (pi->is_removed_snap(*q)) {
	dout(10) << " osdmap marks " << *q << " as removed" << dendl;
	all_purged[id].push_back(*q);
      } else {
	all_purge[id].push_back(*q);
      }
    }
  }
  mds->objecter->put_osdmap_read();

  if (!all_purged.empty()) {
    // prepare to remove from need_to_purge list
    bufferlist bl;
    ::encode(all_purged, bl);
    do_server_update(bl);
  }

  if (!all_purge.empty()) {
    dout(10) << "requesting removal of " << all_purge << dendl;
    MRemoveSnaps *m = new MRemoveSnaps(all_purge);
    mds->monc->send_mon_message(m);
  }

  last_checked_osdmap = version;
}


void SnapServer::dump(Formatter *f) const
{
  f->open_object_section("snapserver");

  f->dump_int("last_snap", last_snap.val);

  f->open_array_section("pending_noop");
  for(set<version_t>::const_iterator i = pending_noop.begin(); i != pending_noop.end(); ++i) {
    f->dump_unsigned("version", *i);
  }
  f->close_section();

  f->open_array_section("snaps");
  for (map<snapid_t, SnapInfo>::const_iterator i = snaps.begin(); i != snaps.end(); ++i) {
    f->open_object_section("snap");
    i->second.dump(f);
    f->close_section();
  }
  f->close_section();

  f->open_object_section("need_to_purge");
  for (map<int, set<snapid_t> >::const_iterator i = need_to_purge.begin(); i != need_to_purge.end(); ++i) {
    stringstream pool_id;
    pool_id << i->first;
    f->open_array_section(pool_id.str().c_str());
    for (set<snapid_t>::const_iterator s = i->second.begin(); s != i->second.end(); ++s) {
      f->dump_unsigned("snapid", s->val);
    }
    f->close_section();
  }
  f->close_section();

  f->open_array_section("pending_create");
  for(map<version_t, SnapInfo>::const_iterator i = pending_create.begin(); i != pending_create.end(); ++i) {
    f->open_object_section("snap");
    f->dump_unsigned("version", i->first);
    f->open_object_section("snapinfo");
    i->second.dump(f);
    f->close_section();
    f->close_section();
  }
  f->close_section();

  f->open_array_section("pending_destroy");
  for(map<version_t, pair<snapid_t, snapid_t> >::const_iterator i = pending_destroy.begin(); i != pending_destroy.end(); ++i) {
    f->open_object_section("snap");
    f->dump_unsigned("version", i->first);
    f->dump_unsigned("removed_snap", i->second.first);
    f->dump_unsigned("seq", i->second.second);
    f->close_section();
  }
  f->close_section();

  f->close_section();
}

void SnapServer::generate_test_instances(list<SnapServer*>& ls)
{
  list<SnapInfo*> snapinfo_instances;
  SnapInfo::generate_test_instances(snapinfo_instances);
  SnapInfo populated_snapinfo = *(snapinfo_instances.back());
  for (list<SnapInfo*>::iterator i = snapinfo_instances.begin(); i != snapinfo_instances.end(); ++i) {
    delete *i;
  }

  SnapServer *blank = new SnapServer();
  ls.push_back(blank);
  SnapServer *populated = new SnapServer();
  populated->last_snap = 123;
  populated->snaps[456] = populated_snapinfo;
  populated->need_to_purge[2].insert(012);
  populated->pending_create[234] = populated_snapinfo;
  populated->pending_destroy[345].first = 567;
  populated->pending_destroy[345].second = 768;
  populated->pending_noop.insert(890);

  ls.push_back(populated);

}
