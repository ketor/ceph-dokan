// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_CLIENT_METAREQUEST_H
#define CEPH_CLIENT_METAREQUEST_H


#include "include/types.h"
#include "msg/msg_types.h"
#include "include/xlist.h"
#include "include/filepath.h"
#include "include/atomic.h"
#include "mds/mdstypes.h"

#include "common/Mutex.h"

#include "messages/MClientRequest.h"

class MClientReply;
struct Inode;
class Dentry;

struct MetaRequest {
private:
  Inode *_inode;
  Inode *_old_inode, *_other_inode;
  Dentry *_dentry; //associated with path
  Dentry *_old_dentry; //associated with path2
public:
  uint64_t tid;
  utime_t  op_stamp;
  ceph_mds_request_head head;
  filepath path, path2;
  bufferlist data;
  int inode_drop; //the inode caps this operation will drop
  int inode_unless; //unless we have these caps already
  int old_inode_drop, old_inode_unless;
  int dentry_drop, dentry_unless;
  int old_dentry_drop, old_dentry_unless;
  int other_inode_drop, other_inode_unless;
  vector<MClientRequest::Release> cap_releases;

  int regetattr_mask;          // getattr mask if i need to re-stat after a traceless reply
 
  utime_t  sent_stamp;
  mds_rank_t mds;                // who i am asking
  mds_rank_t resend_mds;         // someone wants you to (re)send the request here
  bool     send_to_auth;       // must send to auth mds
  __u32    sent_on_mseq;       // mseq at last submission of this request
  int      num_fwd;            // # of times i've been forwarded
  int      retry_attempt;
  atomic_t ref;
  
  MClientReply *reply;         // the reply
  bool kick;
  bool aborted;
  
  // readdir result
  frag_t readdir_frag;
  string readdir_start;  // starting _after_ this name
  uint64_t readdir_offset;

  frag_t readdir_reply_frag;
  vector<pair<string,Inode*> > readdir_result;
  bool readdir_end;
  int readdir_num;
  string readdir_last_name;

  //possible responses
  bool got_unsafe;

  xlist<MetaRequest*>::item item;
  xlist<MetaRequest*>::item unsafe_item;
  Mutex lock; //for get/set sync

  Cond  *caller_cond;          // who to take up
  Cond  *dispatch_cond;        // who to kick back

  Inode *target;

  MetaRequest(int op) :
    _inode(NULL), _old_inode(NULL), _other_inode(NULL),
    _dentry(NULL), _old_dentry(NULL),
    tid(0),
    inode_drop(0), inode_unless(0),
    old_inode_drop(0), old_inode_unless(0),
    dentry_drop(0), dentry_unless(0),
    old_dentry_drop(0), old_dentry_unless(0),
    other_inode_drop(0), other_inode_unless(0),
    regetattr_mask(0),
    mds(-1), resend_mds(-1), send_to_auth(false), sent_on_mseq(0),
    num_fwd(0), retry_attempt(0),
    ref(1), reply(0), 
    kick(false), aborted(false),
    readdir_offset(0), readdir_end(false), readdir_num(0),
    got_unsafe(false), item(this), unsafe_item(this),
    lock("MetaRequest lock"),
    caller_cond(0), dispatch_cond(0),
    target(0) {
    memset(&head, 0, sizeof(ceph_mds_request_head));
    head.op = op;
  }
  ~MetaRequest();

  void set_inode(Inode *in);
  Inode *inode();
  Inode *take_inode() {
    Inode *i = _inode;
    _inode = 0;
    return i;
  }
  void set_old_inode(Inode *in);
  Inode *old_inode();
  Inode *take_old_inode() {
    Inode *i = _old_inode;
    _old_inode = NULL;
    return i;
  }
  void set_other_inode(Inode *in);
  Inode *other_inode();
  Inode *take_other_inode() {
    Inode *i = _other_inode;
    _other_inode = 0;
    return i;
  }
  void set_dentry(Dentry *d);
  Dentry *dentry();
  void set_old_dentry(Dentry *d);
  Dentry *old_dentry();

  MetaRequest* get() {
    ref.inc();
    return this;
  }

  /// psuedo-private put method; use Client::put_request()
  bool _put() {
    int v = ref.dec();
    return v == 0;
  }

  // normal fields
  void set_tid(ceph_tid_t t) { tid = t; }
  void set_oldest_client_tid(ceph_tid_t t) { head.oldest_client_tid = t; }
  void inc_num_fwd() { head.num_fwd = head.num_fwd + 1; }
  void set_retry_attempt(int a) { head.num_retry = a; }
  void set_filepath(const filepath& fp) { path = fp; }
  void set_filepath2(const filepath& fp) { path2 = fp; }
  void set_string2(const char *s) { path2.set_path(s, 0); }
  void set_caller_uid(unsigned u) { head.caller_uid = u; }
  void set_caller_gid(unsigned g) { head.caller_gid = g; }
  void set_data(const bufferlist &d) { data = d; }
  void set_dentry_wanted() {
    head.flags = head.flags | CEPH_MDS_FLAG_WANT_DENTRY;
  }
  int get_op() { return head.op; }
  ceph_tid_t get_tid() { return tid; }
  filepath& get_filepath() { return path; }
  filepath& get_filepath2() { return path2; }

  bool is_write() {
    return
      (head.op & CEPH_MDS_OP_WRITE) || 
      (head.op == CEPH_MDS_OP_OPEN && !(head.args.open.flags & (O_CREAT|O_TRUNC))) ||
      (head.op == CEPH_MDS_OP_CREATE && !(head.args.open.flags & (O_CREAT|O_TRUNC)));
  }
  bool can_forward() {
    if (is_write() ||
	head.op == CEPH_MDS_OP_OPEN ||   // do not forward _any_ open request.
	head.op == CEPH_MDS_OP_CREATE)   // do not forward _any_ open request.
      return false;
    return true;
  }
  bool auth_is_best() {
    if (is_write()) 
      return true;
    if (head.op == CEPH_MDS_OP_OPEN ||
	head.op == CEPH_MDS_OP_CREATE ||
	head.op == CEPH_MDS_OP_READDIR) 
      return true;
    return false;    
  }

  void dump(Formatter *f) const;

};

#endif
