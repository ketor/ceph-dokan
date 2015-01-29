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



#ifndef CEPH_CINODE_H
#define CEPH_CINODE_H

#include "common/config.h"
#include "include/elist.h"
#include "include/types.h"
#include "include/lru.h"

#include "mdstypes.h"
#include "flock.h"

#include "CDentry.h"
#include "SimpleLock.h"
#include "ScatterLock.h"
#include "LocalLock.h"
#include "Capability.h"
#include "SnapRealm.h"

#include <list>
#include <set>
#include <map>
//#include <iostream>

class Context;
class CDentry;
class CDir;
class Message;
class CInode;
class MDCache;
class LogSegment;
struct SnapRealm;
class Session;
class MClientCaps;
struct ObjectOperation;
class EMetaBlob;
struct MDRequestImpl;
typedef ceph::shared_ptr<MDRequestImpl> MDRequestRef;


ostream& operator<<(ostream& out, const CInode& in);

struct cinode_lock_info_t {
  int lock;
  int wr_caps;
};

extern cinode_lock_info_t cinode_lock_info[];
extern int num_cinode_locks;


/**
 * Base class for CInode, containing the backing store data and
 * serialization methods.  This exists so that we can read and
 * handle CInodes from the backing store without hitting all
 * the business logic in CInode proper.
 */
class InodeStore {
public:
  inode_t                    inode;        // the inode itself
  std::string                symlink;      // symlink dest, if symlink
  std::map<std::string, bufferptr> xattrs;
  fragtree_t                 dirfragtree;  // dir frag tree, if any.  always consistent with our dirfrag map.
  std::map<snapid_t, old_inode_t> old_inodes;   // key = last, value.first = first
  bufferlist		     snap_blob;    // Encoded copy of SnapRealm, because we can't
                                           // rehydrate it without full MDCache

  /* Helpers */
  bool is_file() const    { return inode.is_file(); }
  bool is_symlink() const { return inode.is_symlink(); }
  bool is_dir() const     { return inode.is_dir(); }
  static object_t get_object_name(inodeno_t ino, frag_t fg, const char *suffix);

  /* Full serialization for use in ".inode" root inode objects */
  void encode(bufferlist &bl) const;
  void decode(bufferlist::iterator &bl);

  /* Serialization without ENCODE_START/FINISH blocks for use embedded in dentry */
  void encode_bare(bufferlist &bl) const;
  void decode_bare(bufferlist::iterator &bl, __u8 struct_v=4);

  /* For use in debug and ceph-dencoder */
  void dump(Formatter *f) const;
  static void generate_test_instances(std::list<InodeStore*>& ls);
};

// cached inode wrapper
class CInode : public MDSCacheObject, public InodeStore {
  /*
   * This class uses a boost::pool to handle allocation. This is *not*
   * thread-safe, so don't do allocations from multiple threads!
   *
   * Alternatively, switch the pool to use a boost::singleton_pool.
   */

private:
  static boost::pool<> pool;
public:
  static void *operator new(size_t num_bytes) { 
    void *n = pool.malloc();
    if (!n)
      throw std::bad_alloc();
    return n;
  }
  void operator delete(void *p) {
    pool.free(p);
  }


 public:
  // -- pins --
  static const int PIN_DIRFRAG =         -1; 
  static const int PIN_CAPS =             2;  // client caps
  static const int PIN_IMPORTING =       -4;  // importing
  static const int PIN_OPENINGDIR =       7;
  static const int PIN_REMOTEPARENT =     8;
  static const int PIN_BATCHOPENJOURNAL = 9;
  static const int PIN_SCATTERED =        10;
  static const int PIN_STICKYDIRS =       11;
  //static const int PIN_PURGING =         -12;	
  static const int PIN_FREEZING =         13;
  static const int PIN_FROZEN =           14;
  static const int PIN_IMPORTINGCAPS =   -15;
  static const int PIN_PASTSNAPPARENT =  -16;
  static const int PIN_OPENINGSNAPPARENTS = 17;
  static const int PIN_TRUNCATING =       18;
  static const int PIN_STRAY =            19;  // we pin our stray inode while active
  static const int PIN_NEEDSNAPFLUSH =    20;
  static const int PIN_DIRTYRSTAT =       21;
  static const int PIN_EXPORTINGCAPS =    22;
  static const int PIN_DIRTYPARENT =      23;
  static const int PIN_DIRWAITER =        24;

  const char *pin_name(int p) const {
    switch (p) {
    case PIN_DIRFRAG: return "dirfrag";
    case PIN_CAPS: return "caps";
    case PIN_IMPORTING: return "importing";
    case PIN_OPENINGDIR: return "openingdir";
    case PIN_REMOTEPARENT: return "remoteparent";
    case PIN_BATCHOPENJOURNAL: return "batchopenjournal";
    case PIN_SCATTERED: return "scattered";
    case PIN_STICKYDIRS: return "stickydirs";
      //case PIN_PURGING: return "purging";
    case PIN_FREEZING: return "freezing";
    case PIN_FROZEN: return "frozen";
    case PIN_IMPORTINGCAPS: return "importingcaps";
    case PIN_EXPORTINGCAPS: return "exportingcaps";
    case PIN_PASTSNAPPARENT: return "pastsnapparent";
    case PIN_OPENINGSNAPPARENTS: return "openingsnapparents";
    case PIN_TRUNCATING: return "truncating";
    case PIN_STRAY: return "stray";
    case PIN_NEEDSNAPFLUSH: return "needsnapflush";
    case PIN_DIRTYRSTAT: return "dirtyrstat";
    case PIN_DIRTYPARENT: return "dirtyparent";
    case PIN_DIRWAITER: return "dirwaiter";
    default: return generic_pin_name(p);
    }
  }

  // -- state --
  static const int STATE_EXPORTING =   (1<<2);   // on nonauth bystander.
  static const int STATE_OPENINGDIR =  (1<<5);
  static const int STATE_FREEZING =    (1<<7);
  static const int STATE_FROZEN =      (1<<8);
  static const int STATE_AMBIGUOUSAUTH = (1<<9);
  static const int STATE_EXPORTINGCAPS = (1<<10);
  static const int STATE_NEEDSRECOVER = (1<<11);
  static const int STATE_RECOVERING =   (1<<12);
  static const int STATE_PURGING =     (1<<13);
  static const int STATE_DIRTYPARENT =  (1<<14);
  static const int STATE_DIRTYRSTAT =  (1<<15);
  static const int STATE_STRAYPINNED = (1<<16);
  static const int STATE_FROZENAUTHPIN = (1<<17);
  static const int STATE_DIRTYPOOL =   (1<<18);
  // orphan inode needs notification of releasing reference
  static const int STATE_ORPHAN =	STATE_NOTIFYREF;

  static const int MASK_STATE_EXPORTED =
    (STATE_DIRTY|STATE_NEEDSRECOVER|STATE_DIRTYPARENT|STATE_DIRTYPOOL);
  static const int MASK_STATE_EXPORT_KEPT =
    (STATE_FROZEN|STATE_AMBIGUOUSAUTH|STATE_EXPORTINGCAPS);

  // -- waiters --
  static const uint64_t WAIT_DIR         = (1<<0);
  static const uint64_t WAIT_FROZEN      = (1<<1);
  static const uint64_t WAIT_TRUNC       = (1<<2);
  static const uint64_t WAIT_FLOCK       = (1<<3);
  
  static const uint64_t WAIT_ANY_MASK	= (uint64_t)(-1);

  // misc
  static const unsigned EXPORT_NONCE = 1; // nonce given to replicas created by export

  ostream& print_db_line_prefix(ostream& out);

 public:
  MDCache *mdcache;

  SnapRealm        *snaprealm;
  SnapRealm        *containing_realm;
  snapid_t          first, last;
  std::set<snapid_t> dirty_old_rstats;

  bool is_multiversion() const {
    return snaprealm ||  // other snaprealms will link to me
      inode.is_dir() ||  // links to me in other snaps
      inode.nlink > 1 || // there are remote links, possibly snapped, that will need to find me
      !old_inodes.empty(); // once multiversion, always multiversion.  until old_inodes gets cleaned out.
  }
  snapid_t get_oldest_snap();

  uint64_t last_journaled;       // log offset for the last time i was journaled
  //loff_t last_open_journaled;  // log offset for the last journaled EOpen
  utime_t last_dirstat_prop;


  // list item node for when we have unpropagated rstat data
  elist<CInode*>::item dirty_rstat_item;

  bool is_dirty_rstat() {
    return state_test(STATE_DIRTYRSTAT);
  }
  void mark_dirty_rstat();
  void clear_dirty_rstat();

  //bool hack_accessed;
  //utime_t hack_load_stamp;

  /**
   * Projection methods, used to store inode changes until they have been journaled,
   * at which point they are popped.
   * Usage:
   * project_inode as needed. If you're also projecting xattrs, pass
   * in an xattr map (by pointer), then edit the map.
   * If you're also projecting the snaprealm, call project_snaprealm after
   * calling project_inode, and modify the snaprealm as necessary.
   *
   * Then, journal. Once journaling is done, pop_and_dirty_projected_inode.
   * This function will take care of the inode itself, the xattrs, and the snaprealm.
   */

  struct projected_inode_t {
    inode_t *inode;
    std::map<std::string,bufferptr> *xattrs;
    sr_t *snapnode;

    projected_inode_t()
      : inode(NULL), xattrs(NULL), snapnode(NULL) {}
    projected_inode_t(inode_t *in, sr_t *sn)
      : inode(in), xattrs(NULL), snapnode(sn) {}
    projected_inode_t(inode_t *in, std::map<std::string, bufferptr> *xp = NULL, sr_t *sn = NULL)
      : inode(in), xattrs(xp), snapnode(sn) {}
  };
  std::list<projected_inode_t*> projected_nodes;   // projected values (only defined while dirty)
  
  inode_t *project_inode(std::map<std::string,bufferptr> *px=0);
  void pop_and_dirty_projected_inode(LogSegment *ls);

  projected_inode_t *get_projected_node() {
    if (projected_nodes.empty())
      return NULL;
    else
      return projected_nodes.back();
  }

  version_t get_projected_version() const {
    if (projected_nodes.empty())
      return inode.version;
    else
      return projected_nodes.back()->inode->version;
  }
  bool is_projected() const {
    return !projected_nodes.empty();
  }

  const inode_t *get_projected_inode() const {
    if (projected_nodes.empty())
      return &inode;
    else
      return projected_nodes.back()->inode;
  }
  inode_t *get_projected_inode() {
    if (projected_nodes.empty())
      return &inode;
    else
      return projected_nodes.back()->inode;
  }
  inode_t *get_previous_projected_inode() {
    assert(!projected_nodes.empty());
    std::list<projected_inode_t*>::reverse_iterator p = projected_nodes.rbegin();
    ++p;
    if (p != projected_nodes.rend())
      return (*p)->inode;
    else
      return &inode;
  }

  std::map<std::string,bufferptr> *get_projected_xattrs() {
    for (std::list<projected_inode_t*>::reverse_iterator p = projected_nodes.rbegin();
	 p != projected_nodes.rend();
	 ++p)
      if ((*p)->xattrs)
	return (*p)->xattrs;
    return &xattrs;
  }
  std::map<std::string,bufferptr> *get_previous_projected_xattrs() {
      std::list<projected_inode_t*>::reverse_iterator p = projected_nodes.rbegin();
    for (++p;  // skip the most recent projected value
	 p != projected_nodes.rend();
	 ++p)
      if ((*p)->xattrs)
	return (*p)->xattrs;
    return &xattrs;
  }

  sr_t *project_snaprealm(snapid_t snapid=0);
  const sr_t *get_projected_srnode() const {
    if (projected_nodes.empty()) {
      if (snaprealm)
	return &snaprealm->srnode;
      else
	return NULL;
    } else {
      for (std::list<projected_inode_t*>::const_reverse_iterator p = projected_nodes.rbegin();
          p != projected_nodes.rend();
          ++p)
        if ((*p)->snapnode)
          return (*p)->snapnode;
    }
    return &snaprealm->srnode;
  }
  sr_t *get_projected_srnode() {
    if (projected_nodes.empty()) {
      if (snaprealm)
	return &snaprealm->srnode;
      else
	return NULL;
    } else {
      for (std::list<projected_inode_t*>::reverse_iterator p = projected_nodes.rbegin();
          p != projected_nodes.rend();
          ++p)
        if ((*p)->snapnode)
          return (*p)->snapnode;
    }
    return &snaprealm->srnode;
  }
  void project_past_snaprealm_parent(SnapRealm *newparent);

private:
  void pop_projected_snaprealm(sr_t *next_snaprealm);

public:
  old_inode_t& cow_old_inode(snapid_t follows, bool cow_head);
  old_inode_t *pick_old_inode(snapid_t last);
  void pre_cow_old_inode();
  void purge_stale_snap_data(const std::set<snapid_t>& snaps);

  // -- cache infrastructure --
private:
  std::map<frag_t,CDir*> dirfrags; // cached dir fragments under this Inode
  int stickydir_ref;

public:
  __u32 hash_dentry_name(const std::string &dn);
  frag_t pick_dirfrag(const std::string &dn);
  bool has_dirfrags() { return !dirfrags.empty(); }
  CDir* get_dirfrag(frag_t fg) {
    if (dirfrags.count(fg)) {
      //assert(g_conf->debug_mds < 2 || dirfragtree.is_leaf(fg)); // performance hack FIXME
      return dirfrags[fg];
    } else
      return NULL;
  }
  bool get_dirfrags_under(frag_t fg, std::list<CDir*>& ls);
  CDir* get_approx_dirfrag(frag_t fg);
  void get_dirfrags(std::list<CDir*>& ls);
  void get_nested_dirfrags(std::list<CDir*>& ls);
  void get_subtree_dirfrags(std::list<CDir*>& ls);
  CDir *get_or_open_dirfrag(MDCache *mdcache, frag_t fg);
  CDir *add_dirfrag(CDir *dir);
  void close_dirfrag(frag_t fg);
  void close_dirfrags();
  bool has_subtree_root_dirfrag(int auth=-1);
  bool has_subtree_or_exporting_dirfrag();

  void force_dirfrags();
  void verify_dirfrags();

  void get_stickydirs();
  void put_stickydirs();  

 protected:
  // parent dentries in cache
  CDentry         *parent;             // primary link
  std::set<CDentry*>    remote_parents;     // if hard linked

  std::list<CDentry*>   projected_parent;   // for in-progress rename, (un)link, etc.

  mds_authority_t inode_auth;

  // -- distributed state --
protected:
  // file capabilities
  std::map<client_t, Capability*> client_caps;         // client -> caps
  std::map<int32_t, int32_t>      mds_caps_wanted;     // [auth] mds -> caps wanted
  int                   replica_caps_wanted; // [replica] what i've requested from auth

  std::map<int, std::set<client_t> > client_snap_caps;     // [auth] [snap] dirty metadata we still need from the head
public:
  std::map<snapid_t, std::set<client_t> > client_need_snapflush;

  void add_need_snapflush(CInode *snapin, snapid_t snapid, client_t client);
  void remove_need_snapflush(CInode *snapin, snapid_t snapid, client_t client);

protected:

  ceph_lock_state_t fcntl_locks;
  ceph_lock_state_t flock_locks;

  void clear_file_locks() {
    fcntl_locks.clear();
    flock_locks.clear();
  }

  // LogSegment lists i (may) belong to
public:
  elist<CInode*>::item item_dirty;
  elist<CInode*>::item item_caps;
  elist<CInode*>::item item_open_file;
  elist<CInode*>::item item_dirty_parent;
  elist<CInode*>::item item_dirty_dirfrag_dir;
  elist<CInode*>::item item_dirty_dirfrag_nest;
  elist<CInode*>::item item_dirty_dirfrag_dirfragtree;

private:
  // auth pin
  int auth_pins;
  int nested_auth_pins;
public:
#ifdef MDS_AUTHPIN_SET
  multiset<void*> auth_pin_set;
#endif
  int auth_pin_freeze_allowance;

  inode_load_vec_t pop;

  // friends
  friend class Server;
  friend class Locker;
  friend class Migrator;
  friend class MDCache;
  friend class CDir;
  friend class CInodeExport;

  // ---------------------------
  CInode(MDCache *c, bool auth=true, snapid_t f=2, snapid_t l=CEPH_NOSNAP) : 
    mdcache(c),
    snaprealm(0), containing_realm(0),
    first(f), last(l),
    last_journaled(0), //last_open_journaled(0), 
    //hack_accessed(true),
    stickydir_ref(0),
    parent(0),
    inode_auth(CDIR_AUTH_DEFAULT),
    replica_caps_wanted(0),
    fcntl_locks(g_ceph_context), flock_locks(g_ceph_context),
    item_dirty(this), item_caps(this), item_open_file(this), item_dirty_parent(this),
    item_dirty_dirfrag_dir(this), 
    item_dirty_dirfrag_nest(this), 
    item_dirty_dirfrag_dirfragtree(this), 
    auth_pins(0), nested_auth_pins(0),
    auth_pin_freeze_allowance(0),
    pop(ceph_clock_now(g_ceph_context)),
    versionlock(this, &versionlock_type),
    authlock(this, &authlock_type),
    linklock(this, &linklock_type),
    dirfragtreelock(this, &dirfragtreelock_type),
    filelock(this, &filelock_type),
    xattrlock(this, &xattrlock_type),
    snaplock(this, &snaplock_type),
    nestlock(this, &nestlock_type),
    flocklock(this, &flocklock_type),
    policylock(this, &policylock_type),
    loner_cap(-1), want_loner_cap(-1)
  {
    g_num_ino++;
    g_num_inoa++;
    state = 0;  
    if (auth) state_set(STATE_AUTH);
  }
  ~CInode() {
    g_num_ino--;
    g_num_inos++;
    close_dirfrags();
    close_snaprealm();
  }
  

  // -- accessors --
  bool is_root() const { return inode.ino == MDS_INO_ROOT; }
  bool is_stray() const { return MDS_INO_IS_STRAY(inode.ino); }
  bool is_mdsdir() const { return MDS_INO_IS_MDSDIR(inode.ino); }
  bool is_base() const { return is_root() || is_mdsdir(); }
  bool is_system() const { return inode.ino < MDS_INO_SYSTEM_BASE; }

  bool is_head() const { return last == CEPH_NOSNAP; }

  // note: this overloads MDSCacheObject
  bool is_ambiguous_auth() const {
    return state_test(STATE_AMBIGUOUSAUTH) ||
      MDSCacheObject::is_ambiguous_auth();
  }
  void set_ambiguous_auth() {
    state_set(STATE_AMBIGUOUSAUTH);
  }
  void clear_ambiguous_auth(std::list<MDSInternalContextBase*>& finished);
  void clear_ambiguous_auth();

  inodeno_t ino() const { return inode.ino; }
  vinodeno_t vino() const { return vinodeno_t(inode.ino, last); }
  int d_type() const { return IFTODT(inode.mode); }

  inode_t& get_inode() { return inode; }
  CDentry* get_parent_dn() { return parent; }
  const CDentry* get_projected_parent_dn() const { return !projected_parent.empty() ? projected_parent.back() : parent; }
  CDentry* get_projected_parent_dn() { return !projected_parent.empty() ? projected_parent.back() : parent; }
  CDir *get_parent_dir();
  const CDir *get_projected_parent_dir() const;
  CDir *get_projected_parent_dir();
  CInode *get_parent_inode();
  
  bool is_lt(const MDSCacheObject *r) const {
    const CInode *o = static_cast<const CInode*>(r);
    return ino() < o->ino() ||
      (ino() == o->ino() && last < o->last);
  }

  // -- misc -- 
  bool is_projected_ancestor_of(CInode *other);
  void make_path_string(std::string& s, bool force=false, CDentry *use_parent=NULL) const;
  void make_path_string_projected(std::string& s) const;
  void make_path(filepath& s) const;
  void name_stray_dentry(std::string& dname);
  
  // -- dirtyness --
  version_t get_version() const { return inode.version; }

  version_t pre_dirty();
  void _mark_dirty(LogSegment *ls);
  void mark_dirty(version_t projected_dirv, LogSegment *ls);
  void mark_clean();

  void store(MDSInternalContextBase *fin);
  void _stored(int r, version_t cv, Context *fin);
  /**
   * Flush a CInode to disk. This includes the backtrace, the parent
   * directory's link, and the Inode object itself (if a base directory).
   * @pre is_auth() on both the inode and its containing directory
   * @pre can_auth_pin()
   * @param fin The Context to call when the flush is completed.
   */
  void flush(MDSInternalContextBase *fin);
  void fetch(MDSInternalContextBase *fin);
  void _fetched(bufferlist& bl, bufferlist& bl2, Context *fin);  


  void build_backtrace(int64_t pool, inode_backtrace_t& bt);
  void store_backtrace(MDSInternalContextBase *fin, int op_prio=-1);
  void _stored_backtrace(int r, version_t v, Context *fin);
  void fetch_backtrace(Context *fin, bufferlist *backtrace);
  void _mark_dirty_parent(LogSegment *ls, bool dirty_pool=false);
  void clear_dirty_parent();
  void verify_diri_backtrace(bufferlist &bl, int err);
  bool is_dirty_parent() { return state_test(STATE_DIRTYPARENT); }
  bool is_dirty_pool() { return state_test(STATE_DIRTYPOOL); }

  void encode_snap_blob(bufferlist &bl);
  void decode_snap_blob(bufferlist &bl);
  void encode_store(bufferlist& bl);
  void decode_store(bufferlist::iterator& bl);

  void encode_replica(mds_rank_t rep, bufferlist& bl) {
    assert(is_auth());
    
    // relax locks?
    if (!is_replicated())
      replicate_relax_locks();
    
    __u32 nonce = add_replica(rep);
    ::encode(nonce, bl);
    
    _encode_base(bl);
    _encode_locks_state_for_replica(bl);
  }
  void decode_replica(bufferlist::iterator& p, bool is_new) {
    __u32 nonce;
    ::decode(nonce, p);
    replica_nonce = nonce;
    
    _decode_base(p);
    _decode_locks_state(p, is_new);
  }

  // -- waiting --
protected:
  std::map<frag_t, std::list<MDSInternalContextBase*> > waiting_on_dir;
public:
  void add_dir_waiter(frag_t fg, MDSInternalContextBase *c);
  void take_dir_waiting(frag_t fg, std::list<MDSInternalContextBase*>& ls);
  bool is_waiting_for_dir(frag_t fg) {
    return waiting_on_dir.count(fg);
  }
  void add_waiter(uint64_t tag, MDSInternalContextBase *c);
  void take_waiting(uint64_t tag, std::list<MDSInternalContextBase*>& ls);

  // -- encode/decode helpers --
  void _encode_base(bufferlist& bl);
  void _decode_base(bufferlist::iterator& p);
  void _encode_locks_full(bufferlist& bl);
  void _decode_locks_full(bufferlist::iterator& p);
  void _encode_locks_state_for_replica(bufferlist& bl);
  void _encode_locks_state_for_rejoin(bufferlist& bl, int rep);
  void _decode_locks_state(bufferlist::iterator& p, bool is_new);
  void _decode_locks_rejoin(bufferlist::iterator& p, std::list<MDSInternalContextBase*>& waiters,
			    std::list<SimpleLock*>& eval_locks);

  // -- import/export --
  void encode_export(bufferlist& bl);
  void finish_export(utime_t now);
  void abort_export() {
    put(PIN_TEMPEXPORTING);
    assert(state_test(STATE_EXPORTINGCAPS));
    state_clear(STATE_EXPORTINGCAPS);
    put(PIN_EXPORTINGCAPS);
  }
  void decode_import(bufferlist::iterator& p, LogSegment *ls);
  

  // for giving to clients
  int encode_inodestat(bufferlist& bl, Session *session, SnapRealm *realm,
		       snapid_t snapid=CEPH_NOSNAP, unsigned max_bytes=0,
		       int getattr_wants=0);
  void encode_cap_message(MClientCaps *m, Capability *cap);


  // -- locks --
public:
  static LockType versionlock_type;
  static LockType authlock_type;
  static LockType linklock_type;
  static LockType dirfragtreelock_type;
  static LockType filelock_type;
  static LockType xattrlock_type;
  static LockType snaplock_type;
  static LockType nestlock_type;
  static LockType flocklock_type;
  static LockType policylock_type;

  LocalLock  versionlock;
  SimpleLock authlock;
  SimpleLock linklock;
  ScatterLock dirfragtreelock;
  ScatterLock filelock;
  SimpleLock xattrlock;
  SimpleLock snaplock;
  ScatterLock nestlock;
  SimpleLock flocklock;
  SimpleLock policylock;

  SimpleLock* get_lock(int type) {
    switch (type) {
    case CEPH_LOCK_IFILE: return &filelock;
    case CEPH_LOCK_IAUTH: return &authlock;
    case CEPH_LOCK_ILINK: return &linklock;
    case CEPH_LOCK_IDFT: return &dirfragtreelock;
    case CEPH_LOCK_IXATTR: return &xattrlock;
    case CEPH_LOCK_ISNAP: return &snaplock;
    case CEPH_LOCK_INEST: return &nestlock;
    case CEPH_LOCK_IFLOCK: return &flocklock;
    case CEPH_LOCK_IPOLICY: return &policylock;
    }
    return 0;
  }

  void set_object_info(MDSCacheObjectInfo &info);
  void encode_lock_state(int type, bufferlist& bl);
  void decode_lock_state(int type, bufferlist& bl);

  void _finish_frag_update(CDir *dir, MutationRef& mut);

  void clear_dirty_scattered(int type);
  bool is_dirty_scattered();
  void clear_scatter_dirty();  // on rejoin ack

  void start_scatter(ScatterLock *lock);
  void finish_scatter_update(ScatterLock *lock, CDir *dir,
			     version_t inode_version, version_t dir_accounted_version);
  void finish_scatter_gather_update(int type);
  void finish_scatter_gather_update_accounted(int type, MutationRef& mut, EMetaBlob *metablob);

  // -- snap --
  void open_snaprealm(bool no_split=false);
  void close_snaprealm(bool no_join=false);
  SnapRealm *find_snaprealm();
  void encode_snap(bufferlist& bl);
  void decode_snap(bufferlist::iterator& p);

  // -- caps -- (new)
  // client caps
  client_t loner_cap, want_loner_cap;

  client_t get_loner() const { return loner_cap; }
  client_t get_wanted_loner() const { return want_loner_cap; }

  // this is the loner state our locks should aim for
  client_t get_target_loner() const {
    if (loner_cap == want_loner_cap)
      return loner_cap;
    else
      return -1;
  }

  client_t calc_ideal_loner();
  client_t choose_ideal_loner();
  bool try_set_loner();
  void set_loner_cap(client_t l);
  bool try_drop_loner();

  // choose new lock state during recovery, based on issued caps
  void choose_lock_state(SimpleLock *lock, int allissued);
  void choose_lock_states();

  int count_nonstale_caps() {
    int n = 0;
    for (std::map<client_t,Capability*>::iterator it = client_caps.begin();
         it != client_caps.end();
         ++it) 
      if (!it->second->is_stale())
	n++;
    return n;
  }
  bool multiple_nonstale_caps() {
    int n = 0;
    for (std::map<client_t,Capability*>::iterator it = client_caps.begin();
         it != client_caps.end();
         ++it) 
      if (!it->second->is_stale()) {
	if (n)
	  return true;
	n++;
      }
    return false;
  }

  bool is_any_caps() { return !client_caps.empty(); }
  bool is_any_nonstale_caps() { return count_nonstale_caps(); }

  const std::map<int32_t,int32_t>& get_mds_caps_wanted() const { return mds_caps_wanted; }
  std::map<int32_t,int32_t>& get_mds_caps_wanted() { return mds_caps_wanted; }

  const std::map<client_t,Capability*>& get_client_caps() const { return client_caps; }
  Capability *get_client_cap(client_t client) {
    if (client_caps.count(client))
      return client_caps[client];
    return 0;
  }
  int get_client_cap_pending(client_t client) const {
    if (client_caps.count(client)) {
      std::map<client_t, Capability*>::const_iterator found = client_caps.find(client);
      return found->second->pending();
    } else {
      return 0;
    }
  }

  Capability *add_client_cap(client_t client, Session *session, SnapRealm *conrealm=0);
  void remove_client_cap(client_t client);
  void move_to_realm(SnapRealm *realm);

  Capability *reconnect_cap(client_t client, ceph_mds_cap_reconnect& icr, Session *session);
  void clear_client_caps_after_export();
  void export_client_caps(std::map<client_t,Capability::Export>& cl);

  // caps allowed
  int get_caps_liked() const;
  int get_caps_allowed_ever() const;
  int get_caps_allowed_by_type(int type) const;
  int get_caps_careful() const;
  int get_xlocker_mask(client_t client) const;
  int get_caps_allowed_for_client(client_t client) const;

  // caps issued, wanted
  int get_caps_issued(int *ploner = 0, int *pother = 0, int *pxlocker = 0,
		      int shift = 0, int mask = -1);
  bool is_any_caps_wanted() const;
  int get_caps_wanted(int *ploner = 0, int *pother = 0, int shift = 0, int mask = -1) const;
  bool issued_caps_need_gather(SimpleLock *lock);
  void replicate_relax_locks();


  // -- authority --
  mds_authority_t authority() const;


  // -- auth pins --
  bool is_auth_pinned() const { return auth_pins || nested_auth_pins; }
  int get_num_auth_pins() const { return auth_pins; }
  int get_num_nested_auth_pins() const { return nested_auth_pins; }
  void adjust_nested_auth_pins(int a, void *by);
  bool can_auth_pin() const;
  void auth_pin(void *by);
  void auth_unpin(void *by);

  // -- freeze --
  bool is_freezing_inode() const { return state_test(STATE_FREEZING); }
  bool is_frozen_inode() const { return state_test(STATE_FROZEN); }
  bool is_frozen_auth_pin() const { return state_test(STATE_FROZENAUTHPIN); }
  bool is_frozen() const;
  bool is_frozen_dir() const;
  bool is_freezing() const;

  /* Freeze the inode. auth_pin_allowance lets the caller account for any
   * auth_pins it is itself holding/responsible for. */
  bool freeze_inode(int auth_pin_allowance=0);
  void unfreeze_inode(std::list<MDSInternalContextBase*>& finished);
  void unfreeze_inode();

  void freeze_auth_pin();
  void unfreeze_auth_pin();

  // -- reference counting --
  void bad_put(int by) {
    generic_dout(0) << " bad put " << *this << " by " << by << " " << pin_name(by) << " was " << ref
#ifdef MDS_REF_SET
		    << " (" << ref_map << ")"
#endif
		    << dendl;
#ifdef MDS_REF_SET
    assert(ref_map[by] > 0);
#endif
    assert(ref > 0);
  }
  void bad_get(int by) {
    generic_dout(0) << " bad get " << *this << " by " << by << " " << pin_name(by) << " was " << ref
#ifdef MDS_REF_SET
		    << " (" << ref_map << ")"
#endif
		    << dendl;
#ifdef MDS_REF_SET
    assert(ref_map[by] >= 0);
#endif
  }
  void first_get();
  void last_put();
  void _put();


  // -- hierarchy stuff --
public:
  void set_primary_parent(CDentry *p) {
    assert(parent == 0);
    parent = p;
  }
  void remove_primary_parent(CDentry *dn) {
    assert(dn == parent);
    parent = 0;
  }
  void add_remote_parent(CDentry *p);
  void remove_remote_parent(CDentry *p);
  int num_remote_parents() {
    return remote_parents.size(); 
  }

  void push_projected_parent(CDentry *dn) {
    projected_parent.push_back(dn);
  }
  void pop_projected_parent() {
    assert(projected_parent.size());
    parent = projected_parent.front();
    projected_parent.pop_front();
  }

  void print(ostream& out);

  /**
   * @defgroup Scrubbing and fsck
   * @{
   */

  /**
   * Report the results of validation against a particular inode.
   * Each member is a pair of bools.
   * <member>.first represents if validation was performed against the member.
   * <member.second represents if the member passed validation.
   * performed_validation is set to true if the validation was actually
   * run. It might not be run if, for instance, the inode is marked as dirty.
   * passed_validation is set to true if everything that was checked
   * passed its validation.
   */
  struct validated_data {
    template<typename T>struct member_status {
      bool checked;
      bool passed;
      int ondisk_read_retval;
      T ondisk_value;
      T memory_value;
      std::stringstream error_str;
      member_status() : checked(false), passed(false),
          ondisk_read_retval(0) {}
    };

    bool performed_validation;
    bool passed_validation;

    member_status<inode_backtrace_t> backtrace;
    member_status<inode_t> inode;
    member_status<nest_info_t> raw_rstats;

    validated_data() : performed_validation(false),
        passed_validation(false) {}

    void dump(Formatter *f) const;
  };

  /**
   * Validate that the on-disk state of an inode matches what
   * we expect from our memory state. Currently this checks that:
   * 1) The backtrace associated with the file data exists and is correct
   * 2) For directories, the actual inode metadata matches our memory state,
   * 3) For directories, the rstats match
   *
   * @param results A freshly-created validated_data struct, with values set
   * as described in the struct documentation.
   * @param Context The callback to activate once the validation has
   * been completed.
   */
  void validate_disk_state(validated_data *results,
                           MDRequestRef& mdr);
  static void dump_validation_results(const validated_data& results,
                                      Formatter *f);
private:
  bool _validate_disk_state(class ValidationContinuation *c,
                            int rval, int stage);
  friend class ValidationContinuation;
  /** @} Scrubbing and fsck */
};

#endif
