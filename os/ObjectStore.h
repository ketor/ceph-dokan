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
#ifndef CEPH_OBJECTSTORE_H
#define CEPH_OBJECTSTORE_H

#include "include/Context.h"
#include "include/buffer.h"
#include "include/types.h"
#include "osd/osd_types.h"
#include "common/TrackedOp.h"
#include "common/WorkQueue.h"
#include "ObjectMap.h"

#include <errno.h>
#include <sys/stat.h>
#include <vector>
#include <map>

#if defined(DARWIN) || defined(__FreeBSD__)
#include <sys/statvfs.h>
#else
#include <sys/vfs.h>    /* or <sys/statfs.h> */
#endif /* DARWIN */

#define OPS_PER_PTR 32

class CephContext;

using std::vector;
using std::string;
using std::map;

namespace ceph {
  class Formatter;
}

enum {
  l_os_first = 84000,
  l_os_jq_max_ops,
  l_os_jq_ops,
  l_os_j_ops,
  l_os_jq_max_bytes,
  l_os_jq_bytes,
  l_os_j_bytes,
  l_os_j_lat,
  l_os_j_wr,
  l_os_j_wr_bytes,
  l_os_j_full,
  l_os_committing,
  l_os_commit,
  l_os_commit_len,
  l_os_commit_lat,
  l_os_oq_max_ops,
  l_os_oq_ops,
  l_os_ops,
  l_os_oq_max_bytes,
  l_os_oq_bytes,
  l_os_bytes,
  l_os_apply_lat,
  l_os_queue_lat,
  l_os_last,
};


/*
 * low-level interface to the local OSD file system
 */

class Logger;


static inline void encode(const map<string,bufferptr> *attrset, bufferlist &bl) {
  ::encode(*attrset, bl);
}

// Flag bits
typedef uint32_t osflagbits_t;
const int SKIP_JOURNAL_REPLAY = 1 << 0;
const int SKIP_MOUNT_OMAP = 1 << 1;

class ObjectStore {
protected:
  string path;

public:
  /**
   * create - create an ObjectStore instance.
   *
   * This is invoked once at initialization time.
   *
   * @param type type of store. This is a string from the configuration file.
   * @param data path (or other descriptor) for data
   * @param journal path (or other descriptor) for journal (optional)
   * @param flags which filestores should check if applicable
   */
  static ObjectStore *create(CephContext *cct,
			     const string& type,
			     const string& data,
			     const string& journal,
			     osflagbits_t flag = 0);

  Logger *logger;

  /**
   * Fetch Object Store statistics.
   *
   * Currently only latency of write and apply times are measured.
   *
   * This appears to be called with nothing locked.
   */
  virtual objectstore_perf_stat_t get_cur_stats() = 0;

  /**
   * a sequencer orders transactions
   *
   * Any transactions queued under a given sequencer will be applied in
   * sequence.  Transactions queued under different sequencers may run
   * in parallel.
   *
   * Clients of ObjectStore create and maintain their own Sequencer objects.
   * When a list of transactions is queued the caller specifies a Sequencer to be used.
   *
   */

  /**
   * ABC for Sequencer implementation, private to the ObjectStore derived class.
   * created in ...::queue_transaction(s)
   */
  struct Sequencer_impl {
    virtual void flush() = 0;

    /**
     * Async flush_commit
     *
     * There are two cases:
     * 1) sequencer is currently idle: the method returns true and
     *    c is deleted
     * 2) sequencer is not idle: the method returns false and c is
     *    called asyncronously with a value of 0 once all transactions
     *    queued on this sequencer prior to the call have been applied
     *    and committed.
     */
    virtual bool flush_commit(
      Context *c ///< [in] context to call upon flush/commit
      ) = 0; ///< @return true if idle, false otherwise

    virtual ~Sequencer_impl() {}
  };

  /**
   * External (opaque) sequencer implementation
   */
  struct Sequencer {
    string name;
    Sequencer_impl *p;

    Sequencer(string n)
      : name(n), p(NULL) {}
    ~Sequencer() {
      delete p;
    }

    /// return a unique string identifier for this sequencer
    const string& get_name() const {
      return name;
    }
    /// wait for any queued transactions on this sequencer to apply
    void flush() {
      if (p)
	p->flush();
    }

    /// @see Sequencer_impl::flush_commit()
    bool flush_commit(Context *c) {
      if (!p) {
	delete c;
	return true;
      } else {
	return p->flush_commit(c);
      }
    }
  };

  /*********************************
   *
   * Object Contents and semantics
   *
   * All ObjectStore objects are identified as a named object
   * (ghobject_t and hobject_t) in a named collection (coll_t).
   * ObjectStore operations support the creation, mutation, deletion
   * and enumeration of objects within a collection.  Enumeration is
   * in sorted key order (where keys are sorted by hash). Object names
   * are globally unique.
   *
   * Each object has four distinct parts: byte data, xattrs, omap_header
   * and omap entries.
   *
   * The data portion of an object is conceptually equivalent to a
   * file in a file system. Random and Partial access for both read
   * and write operations is required. The ability to have a sparse
   * implementation of the data portion of an object is beneficial for
   * some workloads, but not required. There is a system-wide limit on
   * the maximum size of an object, which is typically around 100 MB.
   *
   * Xattrs are equivalent to the extended attributes of file
   * systems. Xattrs are a set of key/value pairs.  Sub-value access
   * is not required. It is possible to enumerate the set of xattrs in
   * key order.  At the implementation level, xattrs are used
   * exclusively internal to Ceph and the implementer can expect the
   * total size of all of the xattrs on an object to be relatively
   * small, i.e., less than 64KB. Much of Ceph assumes that accessing
   * xattrs on temporally adjacent object accesses (recent past or
   * near future) is inexpensive.
   *
   * omap_header is a single blob of data. It can be read or written
   * in total.
   *
   * Omap entries are conceptually the same as xattrs
   * but in a different address space. In other words, you can have
   * the same key as an xattr and an omap entry and they have distinct
   * values. Enumeration of xattrs doesn't include omap entries and
   * vice versa. The size and access characteristics of omap entries
   * are very different from xattrs. In particular, the value portion
   * of an omap entry can be quite large (MBs).  More importantly, the
   * interface must support efficient range queries on omap entries even
   * when there are a large numbers of entries.
   *
   *********************************/

  /*******************************
   *
   * Collections
   *
   * A collection is simply a grouping of objects. Collections have
   * names (coll_t) and can be enumerated in order.  Like an
   * individual object, a collection also has a set of xattrs.
   *
   *
   */


  /*********************************
   * transaction
   *
   * A Transaction represents a sequence of primitive mutation
   * operations.
   *
   * Three events in the life of a Transaction result in
   * callbacks. Any Transaction can contain any number of callback
   * objects (Context) for any combination of the three classes of
   * callbacks:
   *
   *    on_applied_sync, on_applied, and on_commit.
   *
   * The "on_applied" and "on_applied_sync" callbacks are invoked when
   * the modifications requested by the Transaction are visible to
   * subsequent ObjectStore operations, i.e., the results are
   * readable. The only conceptual difference between on_applied and
   * on_applied_sync is the specific thread and locking environment in
   * which the callbacks operate.  "on_applied_sync" is called
   * directly by an ObjectStore execution thread. It is expected to
   * execute quickly and must not acquire any locks of the calling
   * environment. Conversely, "on_applied" is called from the separate
   * Finisher thread, meaning that it can contend for calling
   * environment locks. NB, on_applied and on_applied_sync are
   * sometimes called on_readable and on_readable_sync.
   *
   * The "on_commit" callback is also called from the Finisher thread
   * and indicates that all of the mutations have been durably
   * committed to stable storage (i.e., are now software/hardware
   * crashproof).
   *
   * At the implementation level, each mutation primitive (and its
   * associated data) can be serialized to a single buffer.  That
   * serialization, however, does not copy any data, but (using the
   * bufferlist library) will reference the original buffers.  This
   * implies that the buffer that contains the data being submitted
   * must remain stable until the on_commit callback completes.  In
   * practice, bufferlist handles all of this for you and this
   * subtlety is only relevant if you are referencing an existing
   * buffer via buffer::raw_static.
   *
   * Some implementations of ObjectStore choose to implement their own
   * form of journaling that uses the serialized form of a
   * Transaction. This requires that the encode/decode logic properly
   * version itself and handle version upgrades that might change the
   * format of the encoded Transaction. This has already happened a
   * couple of times and the Transaction object contains some helper
   * variables that aid in this legacy decoding:
   *
   *   sobject_encoding detects an older/simpler version of oid
   *   present in pre-bobtail versions of ceph.  use_pool_override
   *   also detects a situation where the pool of an oid can be
   *   override for legacy operations/buffers.  For non-legacy
   *   implementation of ObjectStore, neither of these fields is
   *   relevant.
   *
   *
   * TRANSACTION ISOLATION
   *
   * Except as noted below, isolation is the responsibility of the
   * caller. In other words, if any storage element (storage element
   * == any of the four portions of an object as described above) is
   * altered by a transaction (including deletion), the caller
   * promises not to attempt to read that element while the
   * transaction is pending (here pending means from the time of
   * issuance until the "on_applied_sync" callback has been
   * received). Violations of isolation need not be detected by
   * ObjectStore and there is no corresponding error mechanism for
   * reporting an isolation violation (crashing would be the
   * appropriate way to report an isolation violation if detected).
   *
   * Enumeration operations may violate transaction isolation as
   * described above when a storage element is being created or
   * deleted as part of a transaction. In this case, ObjectStore is
   * allowed to consider the enumeration operation to either precede
   * or follow the violating transaction element. In other words, the
   * presence/absence of the mutated element in the enumeration is
   * entirely at the discretion of ObjectStore. The arbitrary ordering
   * applies independently to each transaction element. For example,
   * if a transaction contains two mutating elements "create A" and
   * "delete B". And an enumeration operation is performed while this
   * transaction is pending. It is permissable for ObjectStore to
   * report any of the four possible combinations of the existence of
   * A and B.
   *
   */
  class Transaction {
  public:
    enum {
      OP_NOP =          0,
      OP_TOUCH =        9,   // cid, oid
      OP_WRITE =        10,  // cid, oid, offset, len, bl
      OP_ZERO =         11,  // cid, oid, offset, len
      OP_TRUNCATE =     12,  // cid, oid, len
      OP_REMOVE =       13,  // cid, oid
      OP_SETATTR =      14,  // cid, oid, attrname, bl
      OP_SETATTRS =     15,  // cid, oid, attrset
      OP_RMATTR =       16,  // cid, oid, attrname
      OP_CLONE =        17,  // cid, oid, newoid
      OP_CLONERANGE =   18,  // cid, oid, newoid, offset, len
      OP_CLONERANGE2 =  30,  // cid, oid, newoid, srcoff, len, dstoff

      OP_TRIMCACHE =    19,  // cid, oid, offset, len  **DEPRECATED**

      OP_MKCOLL =       20,  // cid
      OP_RMCOLL =       21,  // cid
      OP_COLL_ADD =     22,  // cid, oldcid, oid
      OP_COLL_REMOVE =  23,  // cid, oid
      OP_COLL_SETATTR = 24,  // cid, attrname, bl
      OP_COLL_RMATTR =  25,  // cid, attrname
      OP_COLL_SETATTRS = 26,  // cid, attrset
      OP_COLL_MOVE =    8,   // newcid, oldcid, oid

      OP_STARTSYNC =    27,  // start a sync

      OP_RMATTRS =      28,  // cid, oid
      OP_COLL_RENAME =       29,  // cid, newcid

      OP_OMAP_CLEAR = 31,   // cid
      OP_OMAP_SETKEYS = 32, // cid, attrset
      OP_OMAP_RMKEYS = 33,  // cid, keyset
      OP_OMAP_SETHEADER = 34, // cid, header
      OP_SPLIT_COLLECTION = 35, // cid, bits, destination
      OP_SPLIT_COLLECTION2 = 36, /* cid, bits, destination
				    doesn't create the destination */
      OP_OMAP_RMKEYRANGE = 37,  // cid, oid, firstkey, lastkey
      OP_COLL_MOVE_RENAME = 38,   // oldcid, oldoid, newcid, newoid

      OP_SETALLOCHINT = 39,  // cid, oid, object_size, write_size
      OP_COLL_HINT = 40, // cid, type, bl
    };

    // Transaction hint type
    enum {
      COLL_HINT_EXPECTED_NUM_OBJECTS = 1,
    };

    struct Op {
      __le32 op;
      __le32 cid;
      __le32 oid;
      __le64 off;
      __le64 len;
      __le32 dest_cid;
      __le32 dest_oid;                  //OP_CLONE, OP_CLONERANGE
      __le64 dest_off;                  //OP_CLONERANGE
      __le32 hint_type;                 //OP_COLL_HINT
      __le64 expected_object_size;      //OP_SETALLOCHINT
      __le64 expected_write_size;       //OP_SETALLOCHINT
      __le32 split_bits;                //OP_SPLIT_COLLECTION2
      __le32 split_rem;                 //OP_SPLIT_COLLECTION2
    } __attribute__ ((packed)) ;

    struct TransactionData {
      __le64 ops;
      __le32 largest_data_len;
      __le32 largest_data_off;
      __le32 largest_data_off_in_tbl;
      __le32 fadvise_flags;

      TransactionData() :
        ops(0),
        largest_data_len(0),
        largest_data_off(0),
        largest_data_off_in_tbl(0),
	fadvise_flags(0) { }

      void encode(bufferlist& bl) const {
        bl.append((char*)this, sizeof(TransactionData));
      }
      void decode(bufferlist::iterator &bl) {
        bl.copy(sizeof(TransactionData), (char*)this);
      }
    } __attribute__ ((packed)) ;

  private:
    TransactionData data;

    void *osr; // NULL on replay

    bool use_tbl;   //use_tbl for encode/decode
    bufferlist tbl;

    map<coll_t, __le32> coll_index;
    map<ghobject_t, __le32> object_index;

    __le32 coll_id;
    __le32 object_id;

    bufferlist data_bl;
    bufferlist op_bl;

    bufferptr op_ptr;

    list<Context *> on_applied;
    list<Context *> on_commit;
    list<Context *> on_applied_sync;

  public:

    /* Operations on callback contexts */
    void register_on_applied(Context *c) {
      if (!c) return;
      on_applied.push_back(c);
    }
    void register_on_commit(Context *c) {
      if (!c) return;
      on_commit.push_back(c);
    }
    void register_on_applied_sync(Context *c) {
      if (!c) return;
      on_applied_sync.push_back(c);
    }
    void register_on_complete(Context *c) {
      if (!c) return;
      RunOnDeleteRef _complete(new RunOnDelete(c));
      register_on_applied(new ContainerContext<RunOnDeleteRef>(_complete));
      register_on_commit(new ContainerContext<RunOnDeleteRef>(_complete));
    }

    static void collect_contexts(
      list<Transaction *> &t,
      Context **out_on_applied,
      Context **out_on_commit,
      Context **out_on_applied_sync) {
      assert(out_on_applied);
      assert(out_on_commit);
      assert(out_on_applied_sync);
      list<Context *> on_applied, on_commit, on_applied_sync;
      for (list<Transaction *>::iterator i = t.begin();
	   i != t.end();
	   ++i) {
	on_applied.splice(on_applied.end(), (*i)->on_applied);
	on_commit.splice(on_commit.end(), (*i)->on_commit);
	on_applied_sync.splice(on_applied_sync.end(), (*i)->on_applied_sync);
      }
      *out_on_applied = C_Contexts::list_to_context(on_applied);
      *out_on_commit = C_Contexts::list_to_context(on_commit);
      *out_on_applied_sync = C_Contexts::list_to_context(on_applied_sync);
    }

    Context *get_on_applied() {
      return C_Contexts::list_to_context(on_applied);
    }
    Context *get_on_commit() {
      return C_Contexts::list_to_context(on_commit);
    }
    Context *get_on_applied_sync() {
      return C_Contexts::list_to_context(on_applied_sync);
    }

    void set_fadvise_flags(uint32_t flags) {
      data.fadvise_flags = flags;
    }
    void set_fadvise_flag(uint32_t flag) {
      data.fadvise_flags = data.fadvise_flags | flag;
    }
    uint32_t get_fadvise_flags() { return data.fadvise_flags; }

    void set_use_tbl(bool value) {
      use_tbl = value;
    }
    bool get_use_tbl() {
      return use_tbl;
    }

    void swap(Transaction& other) {
      std::swap(data, other.data);
      std::swap(on_applied, other.on_applied);
      std::swap(on_commit, other.on_commit);
      std::swap(on_applied_sync, other.on_applied_sync);

      std::swap(use_tbl, other.use_tbl);
      tbl.swap(other.tbl);

      std::swap(coll_index, other.coll_index);
      std::swap(object_index, other.object_index);
      std::swap(coll_id, other.coll_id);
      std::swap(object_id, other.object_id);
      op_bl.swap(other.op_bl);
      data_bl.swap(other.data_bl);
    }

    void _update_op(Op* op,
      vector<__le32> &cm,
      vector<__le32> &om) {

      switch (op->op) {
      case OP_NOP:
      case OP_STARTSYNC:
        break;

      case OP_TOUCH:
      case OP_REMOVE:
      case OP_SETATTR:
      case OP_SETATTRS:
      case OP_RMATTR:
      case OP_RMATTRS:
      case OP_COLL_REMOVE:
      case OP_OMAP_CLEAR:
      case OP_OMAP_SETKEYS:
      case OP_OMAP_RMKEYS:
      case OP_OMAP_RMKEYRANGE:
      case OP_OMAP_SETHEADER:
      case OP_WRITE:
      case OP_ZERO:
      case OP_TRUNCATE:
      case OP_CLONERANGE2:
      case OP_SETALLOCHINT:
        assert(op->cid < cm.size());
        assert(op->oid < om.size());
        op->cid = cm[op->cid];
        op->oid = om[op->oid];
        break;

      case OP_CLONE:
        assert(op->cid < cm.size());
        assert(op->oid < om.size());
        assert(op->dest_oid < om.size());
        op->cid = cm[op->cid];
        op->oid = om[op->oid];
        op->dest_oid = om[op->dest_oid];
        break;

      case OP_MKCOLL:
      case OP_RMCOLL:
      case OP_COLL_SETATTR:
      case OP_COLL_RMATTR:
      case OP_COLL_SETATTRS:
      case OP_COLL_HINT:
        assert(op->cid < cm.size());
        op->cid = cm[op->cid];
        break;

      case OP_COLL_ADD:
        assert(op->cid < cm.size());
        assert(op->oid < om.size());
        assert(op->dest_cid < om.size());
        op->cid = cm[op->cid];
        op->dest_cid = cm[op->dest_cid];
        op->oid = om[op->oid];
        break;

      case OP_COLL_MOVE_RENAME:
        assert(op->cid < cm.size());
        assert(op->oid < om.size());
        assert(op->dest_cid < cm.size());
        assert(op->dest_oid < om.size());
        op->cid = cm[op->cid];
        op->oid = om[op->oid];
        op->dest_cid = cm[op->dest_cid];
        op->dest_oid = om[op->dest_oid];
        break;

      case OP_SPLIT_COLLECTION2:
        assert(op->cid < cm.size());
        op->dest_cid = cm[op->dest_oid];
        op->cid = cm[op->cid];
        op->dest_cid = cm[op->dest_cid];
        break;

      default:
        assert(0 == "Unkown OP");
      }
    }
    void _update_op_bl(
      bufferlist& bl,
      vector<__le32> &cm,
      vector<__le32> &om) {

      list<bufferptr> list = bl.buffers();
      std::list<bufferptr>::iterator p;

      for(p = list.begin(); p != list.end(); p++) {
        assert(p->length() % sizeof(Op) == 0);

        char* raw_p = p->c_str();
        char* raw_end = raw_p + p->length();
        while (raw_p < raw_end) {
          _update_op((Op*)raw_p, cm, om);
          raw_p += sizeof(Op);
        }
      }
    }
    /// Append the operations of the parameter to this Transaction. Those operations are removed from the parameter Transaction
    void append(Transaction& other) {
      assert(use_tbl == other.use_tbl);

      data.ops += other.data.ops;
      if (other.data.largest_data_len > data.largest_data_len) {
	data.largest_data_len = other.data.largest_data_len;
	data.largest_data_off = other.data.largest_data_off;
	data.largest_data_off_in_tbl = tbl.length() + other.data.largest_data_off_in_tbl;
      }
      data.fadvise_flags |= other.data.fadvise_flags;
      tbl.append(other.tbl);
      on_applied.splice(on_applied.end(), other.on_applied);
      on_commit.splice(on_commit.end(), other.on_commit);
      on_applied_sync.splice(on_applied_sync.end(), other.on_applied_sync);

      //append coll_index & object_index
      vector<__le32> cm(other.coll_index.size());
      map<coll_t, __le32>::iterator coll_index_p;
      for (coll_index_p = other.coll_index.begin();
           coll_index_p != other.coll_index.end();
           coll_index_p++) {
        cm[coll_index_p->second] = _get_coll_id(coll_index_p->first);
      }

      vector<__le32> om(other.object_index.size());
      map<ghobject_t, __le32>::iterator object_index_p;
      for (object_index_p = other.object_index.begin();
           object_index_p != other.object_index.end();
           object_index_p++) {
        om[object_index_p->second] = _get_object_id(object_index_p->first);
      }

      //update other.op_bl with cm & om
      //When the other is appended to current transaction, all coll_index and
      //object_index in other.op_buffer should be updated by new index of the
      //combined transaction
      _update_op_bl(other.op_bl, cm, om);

      //append op_bl
      op_bl.append(other.op_bl);
      //append data_bl
      data_bl.append(other.data_bl);
    }

    /** Inquires about the Transaction as a whole. */

    /// How big is the encoded Transaction buffer?
    uint64_t get_encoded_bytes() {
      if (use_tbl)
        return 1 + 8 + 8 + 4 + 4 + 4 + 4 + 4 + tbl.length();
      else {
        //layout: data_bl + op_bl + coll_index + object_index + data
        //TODO: maybe we need better way to get encoded bytes;
        bufferlist bl;
        ::encode(coll_index, bl);
        ::encode(object_index, bl);

        return data_bl.length() +
          op_bl.length() +
          bl.length() +
          sizeof(data);
      }
    }

    uint64_t get_num_bytes() {
      return get_encoded_bytes();
    }
    /// Size of largest data buffer to the "write" operation encountered so far
    uint32_t get_data_length() {
      return data.largest_data_len;
    }
    /// offset within the encoded buffer to the start of the largest data buffer that's encoded
    uint32_t get_data_offset() {
      if (data.largest_data_off_in_tbl) {
        if (use_tbl) {
          return data.largest_data_off_in_tbl +
            sizeof(__u8) +      // encode struct_v
            sizeof(__u8) +      // encode compat_v
            sizeof(__u32) +     // encode len
            sizeof(uint64_t) +  // ops
            sizeof(uint64_t) +  // pad_unused_bytes(unused)
            sizeof(uint32_t) +  // largest_data_len
            sizeof(uint32_t) +  // largest_data_off
            sizeof(uint32_t) +  // largest_data_off_in_tbl
	    sizeof(uint32_t) +   //fadvise_flags
            sizeof(__u32);      // tbl length
        } else {
          return data.largest_data_off_in_tbl +
            sizeof(__u8) +      // encode struct_v
            sizeof(__u8) +      // encode compat_v
            sizeof(__u32);      // encode len
        }
      }
      return 0;  // none
    }
    /// offset of buffer as aligned to destination within object.
    int get_data_alignment() {
      if (!data.largest_data_len)
	return -1;
      return (0 - get_data_offset()) & ~CEPH_PAGE_MASK;
    }
    /// Is the Transaction empty (no operations)
    bool empty() {
      return !data.ops;
    }
    /// Number of operations in the transation
    int get_num_ops() {
      return data.ops;
    }

    void set_osr(void *s) {
      osr = s;
    }

    void *get_osr() {
      return osr;
    }

    /**
     * iterator
     *
     * Helper object to parse Transactions.
     *
     * ObjectStore instances use this object to step down the encoded
     * buffer decoding operation codes and parameters as we go.
     *
     */
    class iterator {
      Transaction *t;

      uint64_t ops;
      char* op_buffer_p;

      bufferlist::iterator data_bl_p;

      vector<coll_t> colls;
      vector<ghobject_t> objects;

      iterator(Transaction *t)
        : t(t),
	  data_bl_p(t->data_bl.begin()),
          colls(t->coll_index.size()),
          objects(t->object_index.size()) {

        ops = t->data.ops;
        op_buffer_p = t->op_bl.get_contiguous(0, t->data.ops * sizeof(Op));

        map<coll_t, __le32>::iterator coll_index_p;
        for (coll_index_p = t->coll_index.begin();
             coll_index_p != t->coll_index.end();
             coll_index_p++) {
          colls[coll_index_p->second] = coll_index_p->first;
        }

        map<ghobject_t, __le32>::iterator object_index_p;
        for (object_index_p = t->object_index.begin();
             object_index_p != t->object_index.end();
             object_index_p++) {
          objects[object_index_p->second] = object_index_p->first;
        }
      }

      friend class Transaction;

    public:

      bool have_op() {
        return ops > 0;
      }
      Op* decode_op() {
        assert(ops > 0);

        Op* op =  (Op*)op_buffer_p;
        op_buffer_p += sizeof(Op);
        ops--;

        return op;
      }
      string decode_string() {
        string s;
        ::decode(s, data_bl_p);
        return s;
      }
      void decode_bl(bufferlist& bl) {
        ::decode(bl, data_bl_p);
      }
      void decode_attrset(map<string,bufferptr>& aset) {
        ::decode(aset, data_bl_p);
      }
      void decode_attrset(map<string,bufferlist>& aset) {
        ::decode(aset, data_bl_p);
      }
      void decode_keyset(set<string> &keys){
        ::decode(keys, data_bl_p);
      }

      ghobject_t get_oid(__le32 oid_id) {
        assert(oid_id < objects.size());
        return objects[oid_id];
      }
      coll_t get_cid(__le32 cid_id) {
        assert(cid_id < colls.size());
        return colls[cid_id];
      }
      uint32_t get_fadvise_flags() const {
	return t->get_fadvise_flags();
      }
    };

    iterator begin() {
      if (use_tbl) {
        _build_actions_from_tbl();
      }
      return iterator(this);
    }

private:
    void _build_actions_from_tbl();

    /**
     * Helper functions to encode the various mutation elements of a
     * transaction.  These are 1:1 with the operation codes (see
     * enumeration above).  These routines ensure that the
     * encoder/creator of a transaction gets the right data in the
     * right place. Sadly, there's no corresponding version nor any
     * form of seat belts for the decoder.
     */
    Op* _get_next_op() {
      if (op_ptr.length() == 0 || op_ptr.offset() >= op_ptr.length()) {
        op_ptr = bufferptr(sizeof(Op) * OPS_PER_PTR);
	op_ptr.zero();
      }
      bufferptr ptr(op_ptr, 0, sizeof(Op));
      op_bl.append(ptr);

      op_ptr.set_offset(op_ptr.offset() + sizeof(Op));

      char* p = ptr.c_str();
      return (Op*)p;
    }
    __le32 _get_coll_id(const coll_t& coll) {
      map<coll_t, __le32>::iterator c = coll_index.find(coll);
      if (c != coll_index.end())
        return c->second;

      __le32 index_id = coll_id++;
      coll_index[coll] = index_id;
      return index_id;
    }
    __le32 _get_object_id(const ghobject_t& oid) {
      map<ghobject_t, __le32>::iterator o = object_index.find(oid);
      if (o != object_index.end())
        return o->second;

      __le32 index_id = object_id++;
      object_index[oid] = index_id;
      return index_id;
    }

public:
    /// Commence a global file system sync operation.
    void start_sync() {
      if (use_tbl) {
        __u32 op = OP_STARTSYNC;
        ::encode(op, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_STARTSYNC;
      }
      data.ops++;
    }
    /// noop. 'nuf said
    void nop() {
      if (use_tbl) {
        __u32 op = OP_NOP;
        ::encode(op, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_NOP;
      }
      data.ops++;
    }
    /**
     * touch
     *
     * Ensure the existance of an object in a collection. Create an
     * empty object if necessary
     */
    void touch(coll_t cid, const ghobject_t& oid) {
      if (use_tbl) {
        __u32 op = OP_TOUCH;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_TOUCH;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
      }
      data.ops++;
    }
    /**
     * Write data to an offset within an object. If the object is too
     * small, it is expanded as needed.  It is possible to specify an
     * offset beyond the current end of an object and it will be
     * expanded as needed. Simple implementations of ObjectStore will
     * just zero the data between the old end of the object and the
     * newly provided data. More sophisticated implementations of
     * ObjectStore will omit the untouched data and store it as a
     * "hole" in the file.
     */
    void write(coll_t cid, const ghobject_t& oid, uint64_t off, uint64_t len,
	       const bufferlist& write_data, uint32_t flags = 0) {
      if (use_tbl) {
        __u32 op = OP_WRITE;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(off, tbl);
        ::encode(len, tbl);
        ::encode(write_data, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_WRITE;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        _op->off = off;
        _op->len = len;
        ::encode(write_data, data_bl);
      }
      assert(len == write_data.length());
      data.fadvise_flags = data.fadvise_flags | flags;
      if (write_data.length() > data.largest_data_len) {
	data.largest_data_len = write_data.length();
	data.largest_data_off = off;
	data.largest_data_off_in_tbl = tbl.length() + sizeof(__u32);  // we are about to
      }
      data.ops++;
    }
    /**
     * zero out the indicated byte range within an object. Some
     * ObjectStore instances may optimize this to release the
     * underlying storage space.
     */
    void zero(coll_t cid, const ghobject_t& oid, uint64_t off, uint64_t len) {
      if (use_tbl) {
        __u32 op = OP_ZERO;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(off, tbl);
        ::encode(len, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_ZERO;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        _op->off = off;
        _op->len = len;
      }
      data.ops++;
    }
    /// Discard all data in the object beyond the specified size.
    void truncate(coll_t cid, const ghobject_t& oid, uint64_t off) {
      if (use_tbl) {
        __u32 op = OP_TRUNCATE;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(off, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_TRUNCATE;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        _op->off = off;
      }
      data.ops++;
    }
    /// Remove an object. All four parts of the object are removed.
    void remove(coll_t cid, const ghobject_t& oid) {
      if (use_tbl) {
        __u32 op = OP_REMOVE;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_REMOVE;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
      }
      data.ops++;
    }
    /// Set an xattr of an object
    void setattr(coll_t cid, const ghobject_t& oid, const char* name, bufferlist& val) {
      string n(name);
      setattr(cid, oid, n, val);
    }
    /// Set an xattr of an object
    void setattr(coll_t cid, const ghobject_t& oid, const string& s, bufferlist& val) {
      if (use_tbl) {
        __u32 op = OP_SETATTR;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(s, tbl);
        ::encode(val, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_SETATTR;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        ::encode(s, data_bl);
        ::encode(val, data_bl);
      }
      data.ops++;
    }
    /// Set multiple xattrs of an object
    void setattrs(coll_t cid, const ghobject_t& oid, map<string,bufferptr>& attrset) {
      if (use_tbl) {
        __u32 op = OP_SETATTRS;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(attrset, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_SETATTRS;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        ::encode(attrset, data_bl);
      }
      data.ops++;
    }
    /// Set multiple xattrs of an object
    void setattrs(coll_t cid, const ghobject_t& oid, map<string,bufferlist>& attrset) {
      if (use_tbl) {
        __u32 op = OP_SETATTRS;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(attrset, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_SETATTRS;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        ::encode(attrset, data_bl);
      }
      data.ops++;
    }
    /// remove an xattr from an object
    void rmattr(coll_t cid, const ghobject_t& oid, const char *name) {
      string n(name);
      rmattr(cid, oid, n);
    }
    /// remove an xattr from an object
    void rmattr(coll_t cid, const ghobject_t& oid, const string& s) {
      if (use_tbl) {
        __u32 op = OP_RMATTR;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(s, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_RMATTR;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        ::encode(s, data_bl);
      }
      data.ops++;
    }
    /// remove all xattrs from an object
    void rmattrs(coll_t cid, const ghobject_t& oid) {
      if (use_tbl) {
        __u32 op = OP_RMATTRS;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_RMATTRS;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
      }
      data.ops++;
    }
    /**
     * Clone an object into another object.
     *
     * Low-cost (e.g., O(1)) cloning (if supported) is best, but
     * fallback to an O(n) copy is allowed.  All four parts of the
     * object are cloned (data, xattrs, omap header, omap
     * entries).
     *
     * The destination named object may already exist, in
     * which case its previous contents are discarded.
     */
    void clone(coll_t cid, const ghobject_t& oid, ghobject_t noid) {
      if (use_tbl) {
        __u32 op = OP_CLONE;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(noid, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_CLONE;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        _op->dest_oid = _get_object_id(noid);
      }
      data.ops++;
    }
    /**
     * Clone a byte range from one object to another.
     *
     * The data portion of the destination object receives a copy of a
     * portion of the data from the source object. None of the other
     * three parts of an object is copied from the source.
     */
    void clone_range(coll_t cid, const ghobject_t& oid, ghobject_t noid,
		     uint64_t srcoff, uint64_t srclen, uint64_t dstoff) {
      if (use_tbl) {
        __u32 op = OP_CLONERANGE2;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(noid, tbl);
        ::encode(srcoff, tbl);
        ::encode(srclen, tbl);
        ::encode(dstoff, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_CLONERANGE2;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        _op->dest_oid = _get_object_id(noid);
        _op->off = srcoff;
        _op->len = srclen;
        _op->dest_off = dstoff;
      }
      data.ops++;
    }
    /// Create the collection
    void create_collection(coll_t cid) {
      if (use_tbl) {
        __u32 op = OP_MKCOLL;
        ::encode(op, tbl);
        ::encode(cid, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_MKCOLL;
        _op->cid = _get_coll_id(cid);
      }
      data.ops++;
    }

    /**
     * Give the collection a hint.
     *
     * @param cid  - collection id.
     * @param type - hint type.
     * @param hint - the hint payload, which contains the customized
     *               data along with the hint type.
     */
    void collection_hint(coll_t cid, uint32_t type, const bufferlist& hint) {
      if (use_tbl) {
        __u32 op = OP_COLL_HINT;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(type, tbl);
        ::encode(hint, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_COLL_HINT;
        _op->cid = _get_coll_id(cid);
        _op->hint_type = type;
        ::encode(hint, data_bl);
      }
      data.ops++;
    }

    /// remove the collection, the collection must be empty
    void remove_collection(coll_t cid) {
      if (use_tbl) {
        __u32 op = OP_RMCOLL;
        ::encode(op, tbl);
        ::encode(cid, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_RMCOLL;
        _op->cid = _get_coll_id(cid);
      }
      data.ops++;
    }
    void collection_move(coll_t cid, coll_t oldcid, const ghobject_t& oid) {
      // NOTE: we encode this as a fixed combo of ADD + REMOVE.  they
      // always appear together, so this is effectively a single MOVE.
      if (use_tbl) {
        __u32 op = OP_COLL_ADD;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oldcid, tbl);
        ::encode(oid, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_COLL_ADD;
        _op->cid = _get_coll_id(oldcid);
        _op->oid = _get_object_id(oid);
        _op->dest_cid = _get_coll_id(cid);
      }
      data.ops++;

      if (use_tbl) {
        __u32 op = OP_COLL_REMOVE;
        ::encode(op, tbl);
        ::encode(oldcid, tbl);
        ::encode(oid, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_COLL_REMOVE;
        _op->cid = _get_coll_id(oldcid);
        _op->oid = _get_object_id(oid);
      }
      data.ops++;
    }
    void collection_move_rename(coll_t oldcid, const ghobject_t& oldoid,
				coll_t cid, const ghobject_t& oid) {
      if (use_tbl) {
        __u32 op = OP_COLL_MOVE_RENAME;
        ::encode(op, tbl);
        ::encode(oldcid, tbl);
        ::encode(oldoid, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_COLL_MOVE_RENAME;
        _op->cid = _get_coll_id(oldcid);
        _op->oid = _get_object_id(oldoid);
        _op->dest_cid = _get_coll_id(cid);
        _op->dest_oid = _get_object_id(oid);
      }
      data.ops++;
    }

    // NOTE: Collection attr operations are all DEPRECATED.  new
    // backends need not implement these at all.

    /// Set an xattr on a collection
    void collection_setattr(coll_t cid, const string& name, bufferlist& val)
      __attribute__ ((deprecated)) {
      if (use_tbl) {
        __u32 op = OP_COLL_SETATTR;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(name, tbl);
        ::encode(val, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_COLL_SETATTR;
        _op->cid = _get_coll_id(cid);
        ::encode(name, data_bl);
        ::encode(val, data_bl);
      }
      data.ops++;
    }

    /// Remove an xattr from a collection
    void collection_rmattr(coll_t cid, const string& name)
      __attribute__ ((deprecated)) {
      if (use_tbl) {
        __u32 op = OP_COLL_RMATTR;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(name, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_COLL_RMATTR;
        _op->cid = _get_coll_id(cid);
        ::encode(name, data_bl);
      }
      data.ops++;
    }
    /// Set multiple xattrs on a collection
    void collection_setattrs(coll_t cid, map<string,bufferptr>& aset)
      __attribute__ ((deprecated)) {
      if (use_tbl) {
        __u32 op = OP_COLL_SETATTRS;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(aset, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_COLL_SETATTRS;
        _op->cid = _get_coll_id(cid);
        ::encode(aset, data_bl);
      }
      data.ops++;
    }
    /// Set multiple xattrs on a collection
    void collection_setattrs(coll_t cid, map<string,bufferlist>& aset)
      __attribute__ ((deprecated)) {
      if (use_tbl) {
        __u32 op = OP_COLL_SETATTRS;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(aset, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_COLL_SETATTRS;
        _op->cid = _get_coll_id(cid);
        ::encode(aset, data_bl);
      }
      data.ops++;
    }
    /// Remove omap from oid
    void omap_clear(
      coll_t cid,           ///< [in] Collection containing oid
      const ghobject_t &oid  ///< [in] Object from which to remove omap
      ) {
      if (use_tbl) {
        __u32 op = OP_OMAP_CLEAR;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_OMAP_CLEAR;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
      }
      data.ops++;
    }
    /// Set keys on oid omap.  Replaces duplicate keys.
    void omap_setkeys(
      coll_t cid,                           ///< [in] Collection containing oid
      const ghobject_t &oid,                ///< [in] Object to update
      const map<string, bufferlist> &attrset ///< [in] Replacement keys and values
      ) {
      if (use_tbl) {
        __u32 op = OP_OMAP_SETKEYS;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(attrset, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_OMAP_SETKEYS;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        ::encode(attrset, data_bl);
      }
      data.ops++;
    }
    /// Remove keys from oid omap
    void omap_rmkeys(
      coll_t cid,             ///< [in] Collection containing oid
      const ghobject_t &oid,  ///< [in] Object from which to remove the omap
      const set<string> &keys ///< [in] Keys to clear
      ) {
      if (use_tbl) {
        __u32 op = OP_OMAP_RMKEYS;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(keys, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_OMAP_RMKEYS;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        ::encode(keys, data_bl);
      }
      data.ops++;
    }

    /// Remove key range from oid omap
    void omap_rmkeyrange(
      coll_t cid,             ///< [in] Collection containing oid
      const ghobject_t &oid,  ///< [in] Object from which to remove the omap keys
      const string& first,    ///< [in] first key in range
      const string& last      ///< [in] first key past range, range is [first,last)
      ) {
      if (use_tbl) {
        __u32 op = OP_OMAP_RMKEYRANGE;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(first, tbl);
        ::encode(last, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_OMAP_RMKEYRANGE;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        ::encode(first, data_bl);
        ::encode(last, data_bl);
      }
      data.ops++;
    }

    /// Set omap header
    void omap_setheader(
      coll_t cid,             ///< [in] Collection containing oid
      const ghobject_t &oid,  ///< [in] Object
      const bufferlist &bl    ///< [in] Header value
      ) {
      if (use_tbl) {
        __u32 op = OP_OMAP_SETHEADER;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(bl, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_OMAP_SETHEADER;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        ::encode(bl, data_bl);
      }
      data.ops++;
    }

    /// Split collection based on given prefixes, objects matching the specified bits/rem are
    /// moved to the new collection
    void split_collection(
      coll_t cid,
      uint32_t bits,
      uint32_t rem,
      coll_t destination) {
      if (use_tbl) {
        __u32 op = OP_SPLIT_COLLECTION2;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(bits, tbl);
        ::encode(rem, tbl);
        ::encode(destination, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_SPLIT_COLLECTION2;
        _op->cid = _get_coll_id(cid);
        _op->dest_cid = _get_coll_id(destination);
        _op->split_bits = bits;
        _op->split_rem = rem;
      }
      data.ops++;
    }

    void set_alloc_hint(
      coll_t cid,
      const ghobject_t &oid,
      uint64_t expected_object_size,
      uint64_t expected_write_size
    ) {
      if (use_tbl) {
        __u32 op = OP_SETALLOCHINT;
        ::encode(op, tbl);
        ::encode(cid, tbl);
        ::encode(oid, tbl);
        ::encode(expected_object_size, tbl);
        ::encode(expected_write_size, tbl);
      } else {
        Op* _op = _get_next_op();
        _op->op = OP_SETALLOCHINT;
        _op->cid = _get_coll_id(cid);
        _op->oid = _get_object_id(oid);
        _op->expected_object_size = expected_object_size;
        _op->expected_write_size = expected_write_size;
      }
      data.ops++;
    }

    // etc.
    Transaction() :
      osr(NULL),
      use_tbl(false),
      coll_id(0),
      object_id(0) { }

    Transaction(bufferlist::iterator &dp) :
      osr(NULL),
      use_tbl(false),
      coll_id(0),
      object_id(0) {
      decode(dp);
    }

    Transaction(bufferlist &nbl) :
      osr(NULL),
      use_tbl(false),
      coll_id(0),
      object_id(0) {
      bufferlist::iterator dp = nbl.begin();
      decode(dp);
    }

    void encode(bufferlist& bl) const {
      if (use_tbl) {
        uint64_t ops = data.ops;
        uint64_t pad_unused_bytes = 0;
        uint32_t largest_data_len = data.largest_data_len;
        uint32_t largest_data_off = data.largest_data_off;
        uint32_t largest_data_off_in_tbl = data.largest_data_off_in_tbl;
        bool tolerate_collection_add_enoent = false;
        ENCODE_START(7, 5, bl);
        ::encode(ops, bl);
        ::encode(pad_unused_bytes, bl);
        ::encode(largest_data_len, bl);
        ::encode(largest_data_off, bl);
        ::encode(largest_data_off_in_tbl, bl);
        ::encode(tbl, bl);
        ::encode(tolerate_collection_add_enoent, bl);
        ENCODE_FINISH(bl);
      } else {
        //layout: data_bl + op_bl + coll_index + object_index + data
        ENCODE_START(8, 5, bl);
        ::encode(data_bl, bl);
        ::encode(op_bl, bl);
        ::encode(coll_index, bl);
        ::encode(object_index, bl);
        data.encode(bl);
        ENCODE_FINISH(bl);
      }
    }
    void decode(bufferlist::iterator &bl) {
      DECODE_START_LEGACY_COMPAT_LEN(8, 5, 5, bl);
      DECODE_OLDEST(2);
      if (struct_v == 8) {
        ::decode(data_bl, bl);
        ::decode(op_bl, bl);
        ::decode(coll_index, bl);
        ::decode(object_index, bl);
        data.decode(bl);
        use_tbl = false;
        coll_id = coll_index.size();
        object_id = object_index.size();
      } else {
        decode7_5(bl, struct_v);
        use_tbl = true;
      }
      DECODE_FINISH(bl);
    }
    void decode7_5(bufferlist::iterator &bl, __u8 struct_v) {
      uint64_t _ops = 0;
      uint64_t _pad_unused_bytes = 0;
      uint32_t _largest_data_len = 0;
      uint32_t _largest_data_off = 0;
      uint32_t _largest_data_off_in_tbl = 0;
      uint32_t _fadvise_flags = 0;
      bool tolerate_collection_add_enoent = false;

      ::decode(_ops, bl);
      ::decode(_pad_unused_bytes, bl);
      if (struct_v >= 3) {
        ::decode(_largest_data_len, bl);
        ::decode(_largest_data_off, bl);
        ::decode(_largest_data_off_in_tbl, bl);
      }
      ::decode(tbl, bl);
      if (struct_v >= 7) {
	::decode(tolerate_collection_add_enoent, bl);
      }
      if (struct_v >= 8) {
	::decode(_fadvise_flags, bl);
      }

      //assign temp to TransactionData
      data.ops = _ops;
      data.largest_data_len = _largest_data_len;
      data.largest_data_off = _largest_data_off;
      data.largest_data_off_in_tbl = _largest_data_off_in_tbl;
      data.fadvise_flags = _fadvise_flags;
    }

    void dump(ceph::Formatter *f);
    static void generate_test_instances(list<Transaction*>& o);
  };

  struct C_DeleteTransaction : public Context {
    ObjectStore::Transaction *t;
    C_DeleteTransaction(ObjectStore::Transaction *tt) : t(tt) {}
    void finish(int r) {
      delete t;
    }
  };
  template<class T>
  struct C_DeleteTransactionHolder : public Context {
    ObjectStore::Transaction *t;
    T obj;
    C_DeleteTransactionHolder(ObjectStore::Transaction *tt, T &obj) :
      t(tt), obj(obj) {}
    void finish(int r) {
      delete t;
    }
  };

  // synchronous wrappers
  unsigned apply_transaction(Transaction& t, Context *ondisk=0) {
    list<Transaction*> tls;
    tls.push_back(&t);
    return apply_transactions(NULL, tls, ondisk);
  }
  unsigned apply_transaction(Sequencer *osr, Transaction& t, Context *ondisk=0) {
    list<Transaction*> tls;
    tls.push_back(&t);
    return apply_transactions(osr, tls, ondisk);
  }
  unsigned apply_transactions(list<Transaction*>& tls, Context *ondisk=0) {
    return apply_transactions(NULL, tls, ondisk);
  }
  unsigned apply_transactions(Sequencer *osr, list<Transaction*>& tls, Context *ondisk=0);

  int queue_transaction_and_cleanup(Sequencer *osr, Transaction* t,
				    ThreadPool::TPHandle *handle = NULL) {
    list<Transaction *> tls;
    tls.push_back(t);
    return queue_transactions(osr, tls, new C_DeleteTransaction(t),
	                      NULL, NULL, TrackedOpRef(), handle);
  }

  int queue_transaction(Sequencer *osr, Transaction *t, Context *onreadable, Context *ondisk=0,
				Context *onreadable_sync=0,
				TrackedOpRef op = TrackedOpRef(),
				ThreadPool::TPHandle *handle = NULL) {
    list<Transaction*> tls;
    tls.push_back(t);
    return queue_transactions(osr, tls, onreadable, ondisk, onreadable_sync,
	                      op, handle);
  }

  int queue_transactions(Sequencer *osr, list<Transaction*>& tls,
			 Context *onreadable, Context *ondisk=0,
			 Context *onreadable_sync=0,
			 TrackedOpRef op = TrackedOpRef(),
			 ThreadPool::TPHandle *handle = NULL) {
    assert(!tls.empty());
    tls.back()->register_on_applied(onreadable);
    tls.back()->register_on_commit(ondisk);
    tls.back()->register_on_applied_sync(onreadable_sync);
    return queue_transactions(osr, tls, op, handle);
  }

  virtual int queue_transactions(
    Sequencer *osr, list<Transaction*>& tls,
    TrackedOpRef op = TrackedOpRef(),
    ThreadPool::TPHandle *handle = NULL) = 0;


  int queue_transactions(
    Sequencer *osr,
    list<Transaction*>& tls,
    Context *onreadable,
    Context *oncommit,
    Context *onreadable_sync,
    Context *oncomplete,
    TrackedOpRef op);

  int queue_transaction(
    Sequencer *osr,
    Transaction* t,
    Context *onreadable,
    Context *oncommit,
    Context *onreadable_sync,
    Context *oncomplete,
    TrackedOpRef op) {
    list<Transaction*> tls;
    tls.push_back(t);
    return queue_transactions(
      osr, tls, onreadable, oncommit, onreadable_sync, oncomplete, op);
  }

 public:
  ObjectStore(const std::string& path_) : path(path_), logger(NULL) {}
  virtual ~ObjectStore() {}

  // no copying
  ObjectStore(const ObjectStore& o);
  const ObjectStore& operator=(const ObjectStore& o);

  // versioning
  virtual int upgrade() {
    return 0;
  }

  // mgmt
  virtual bool test_mount_in_use() = 0;
  virtual int mount() = 0;
  virtual int umount() = 0;
  virtual unsigned get_max_object_name_length() = 0;
  virtual unsigned get_max_attr_name_length() = 0;
  virtual int mkfs() = 0;  // wipe
  virtual int mkjournal() = 0; // journal only
  virtual void set_allow_sharded_objects() = 0;
  virtual bool get_allow_sharded_objects() = 0;

  virtual int statfs(struct statfs *buf) = 0;

  virtual void collect_metadata(map<string,string> *pm) { }

  /**
   * check whether need journal device
   *
   * It's not constant for backend store. FileStore could have journaless mode
   * and KeyValueStore could have journal device for special backend.
   */
  virtual bool need_journal() = 0;

  /**
   * check the journal uuid/fsid, without opening
   */
  virtual int peek_journal_fsid(uuid_d *fsid) = 0;

  /**
   * write_meta - write a simple configuration key out-of-band
   *
   * Write a simple key/value pair for basic store configuration
   * (e.g., a uuid or magic number) to an unopened/unmounted store.
   * The default implementation writes this to a plaintext file in the
   * path.
   *
   * A newline is appended.
   *
   * @param key key name (e.g., "fsid")
   * @param value value (e.g., a uuid rendered as a string)
   * @returns 0 for success, or an error code
   */
  virtual int write_meta(const std::string& key,
			 const std::string& value);

  /**
   * read_meta - read a simple configuration key out-of-band
   *
   * Read a simple key value to an unopened/mounted store.
   *
   * Trailing whitespace is stripped off.
   *
   * @param key key name
   * @param value pointer to value string
   * @returns 0 for success, or an error code
   */
  virtual int read_meta(const std::string& key,
			std::string *value);

  /**
   * get ideal min value for collection_list_partial()
   *
   * default to some arbitrary values; the implementation will override.
   */
  virtual int get_ideal_list_min() { return 32; }

  /**
   * get ideal max value for collection_list_partial()
   *
   * default to some arbitrary values; the implementation will override.
   */
  virtual int get_ideal_list_max() { return 64; }

  /**
   * Synchronous read operations
   */


  /**
   * exists -- Test for existance of object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @returns true if object exists, false otherwise
   */
  virtual bool exists(coll_t cid, const ghobject_t& oid) = 0;                   // useful?

  /**
   * stat -- get information for an object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param st output information for the object
   * @param allow_eio if false, assert on -EIO operation failure
   * @returns 0 on success, negative error code on failure.
   */
  virtual int stat(
    coll_t cid,
    const ghobject_t& oid,
    struct stat *st,
    bool allow_eio = false) = 0; // struct stat?

  /**
   * read -- read a byte range of data from an object
   *
   * Note: if reading from an offset past the end of the object, we
   * return 0 (not, say, -EINVAL).
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param offset location offset of first byte to be read
   * @param len number of bytes to be read
   * @param bl output bufferlist
   * @param op_flags is CEPH_OSD_OP_FLAG_*
   * @param allow_eio if false, assert on -EIO operation failure
   * @returns number of bytes read on success, or negative error code on failure.
   */
   virtual int read(
    coll_t cid,
    const ghobject_t& oid,
    uint64_t offset,
    size_t len,
    bufferlist& bl,
    uint32_t op_flags = 0,
    bool allow_eio = false) = 0;

  /**
   * fiemap -- get extent map of data of an object
   *
   * Returns an encoded map of the extents of an object's data portion
   * (map<offset,size>).
   *
   * A non-enlightened implementation is free to return the extent (offset, len)
   * as the sole extent.
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param offset location offset of first byte to be read
   * @param len number of bytes to be read
   * @param bl output bufferlist for extent map information.
   * @returns 0 on success, negative error code on failure.
   */
  virtual int fiemap(coll_t cid, const ghobject_t& oid, uint64_t offset, size_t len, bufferlist& bl) = 0;

  /**
   * getattr -- get an xattr of an object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param name name of attr to read
   * @param value place to put output result.
   * @returns 0 on success, negative error code on failure.
   */
  virtual int getattr(coll_t cid, const ghobject_t& oid, const char *name, bufferptr& value) = 0;

  /**
   * getattr -- get an xattr of an object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param name name of attr to read
   * @param value place to put output result.
   * @returns 0 on success, negative error code on failure.
   */
  int getattr(coll_t cid, const ghobject_t& oid, const char *name, bufferlist& value) {
    bufferptr bp;
    int r = getattr(cid, oid, name, bp);
    if (bp.length())
      value.push_back(bp);
    return r;
  }
  int getattr(
    coll_t cid, const ghobject_t& oid,
    const string& name, bufferlist& value) {
    bufferptr bp;
    int r = getattr(cid, oid, name.c_str(), bp);
    value.push_back(bp);
    return r;
  }

  /**
   * getattrs -- get all of the xattrs of an object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param aset place to put output result.
   * @returns 0 on success, negative error code on failure.
   */
  virtual int getattrs(coll_t cid, const ghobject_t& oid, map<string,bufferptr>& aset) = 0;

  /**
   * getattrs -- get all of the xattrs of an object
   *
   * @param cid collection for object
   * @param oid oid of object
   * @param aset place to put output result.
   * @returns 0 on success, negative error code on failure.
   */
  int getattrs(coll_t cid, const ghobject_t& oid, map<string,bufferlist>& aset) {
    map<string,bufferptr> bmap;
    int r = getattrs(cid, oid, bmap);
    for (map<string,bufferptr>::iterator i = bmap.begin();
	i != bmap.end();
	++i) {
      aset[i->first].append(i->second);
    }
    return r;
  }


  // collections

  /**
   * list_collections -- get all of the collections known to this ObjectStore
   *
   * @param ls list of the collections in sorted order.
   * @returns 0 on success, negative error code on failure.
   */
  virtual int list_collections(vector<coll_t>& ls) = 0;

  virtual int collection_version_current(coll_t c, uint32_t *version) {
    *version = 0;
    return 1;
  }
  /**
   * does a collection exist?
   *
   * @param c collection
   * @returns true if it exists, false otherwise
   */
  virtual bool collection_exists(coll_t c) = 0;
  /**
   * collection_getattr - get an xattr of a collection
   *
   * @param cid collection name
   * @param name xattr name
   * @param value pointer of buffer to receive value
   * @param size size of buffer to receive value
   * @returns 0 on success, negative error code on failure
   */
  virtual int collection_getattr(coll_t cid, const char *name,
	                         void *value, size_t size)
    __attribute__ ((deprecated)) {
    return -EOPNOTSUPP;
  }

  /**
   * collection_getattr - get an xattr of a collection
   *
   * @param cid collection name
   * @param name xattr name
   * @param bl buffer to receive value
   * @returns 0 on success, negative error code on failure
   */
  virtual int collection_getattr(coll_t cid, const char *name, bufferlist& bl)
    __attribute__ ((deprecated)) {
    return -EOPNOTSUPP;
  }

  /**
   * collection_getattrs - get all xattrs of a collection
   *
   * @param cid collection name
   * @param asert map of keys and buffers that contain the values
   * @returns 0 on success, negative error code on failure
   */
  virtual int collection_getattrs(coll_t cid, map<string,bufferptr> &aset)
    __attribute__ ((deprecated)) {
    return -EOPNOTSUPP;
  }

  /**
   * is a collection empty?
   *
   * @param c collection
   * @returns true if empty, false otherwise
   */
  virtual bool collection_empty(coll_t c) = 0;

  /**
   * collection_list - get all objects of a collection in sorted order
   *
   * @param c collection name
   * @param o [out] list of objects
   * @returns 0 on success, negative error code on failure
   */
  virtual int collection_list(coll_t c, vector<ghobject_t>& o) = 0;

  /**
   * list partial contents of collection relative to a hash offset/position
   *
   * @param c collection
   * @param start list objects that sort >= this value
   * @param min return at least this many results, unless we reach the end
   * @param max return no more than this many results
   * @param snapid return no objects with snap < snapid
   * @param ls [out] result
   * @param next [out] next item sorts >= this value
   * @return zero on success, or negative error
   */
  virtual int collection_list_partial(coll_t c, ghobject_t start,
				      int min, int max, snapid_t snap,
				      vector<ghobject_t> *ls, ghobject_t *next) = 0;

  /**
   * list contents of a collection that fall in the range [start, end)
   *
   * @param c collection
   * @param start list object that sort >= this value
   * @param end list objects that sort < this value
   * @param snapid return no objects with snap < snapid
   * @param ls [out] result
   * @return zero on success, or negative error
   */
  virtual int collection_list_range(coll_t c, ghobject_t start, ghobject_t end,
	                            snapid_t seq, vector<ghobject_t> *ls) = 0;

  //TODO: Remove
  int collection_list(coll_t c, vector<hobject_t>& o);

  int collection_list_partial(coll_t c, hobject_t start,
				      int min, int max, snapid_t snap,
				      vector<hobject_t> *ls, hobject_t *next);

  int collection_list_range(coll_t c, hobject_t start, hobject_t end,
	                            snapid_t seq, vector<hobject_t> *ls);

  /// OMAP
  /// Get omap contents
  virtual int omap_get(
    coll_t c,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    map<string, bufferlist> *out /// < [out] Key to value map
    ) = 0;

  /// Get omap header
  virtual int omap_get_header(
    coll_t c,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    bool allow_eio = false ///< [in] don't assert on eio
    ) = 0;

  /// Get keys defined on oid
  virtual int omap_get_keys(
    coll_t c,              ///< [in] Collection containing oid
    const ghobject_t &oid, ///< [in] Object containing omap
    set<string> *keys      ///< [out] Keys defined on oid
    ) = 0;

  /// Get key values
  virtual int omap_get_values(
    coll_t c,                    ///< [in] Collection containing oid
    const ghobject_t &oid,       ///< [in] Object containing omap
    const set<string> &keys,     ///< [in] Keys to get
    map<string, bufferlist> *out ///< [out] Returned keys and values
    ) = 0;

  /// Filters keys into out which are defined on oid
  virtual int omap_check_keys(
    coll_t c,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    const set<string> &keys, ///< [in] Keys to check
    set<string> *out         ///< [out] Subset of keys defined on oid
    ) = 0;

  /**
   * Returns an object map iterator
   *
   * Warning!  The returned iterator is an implicit lock on filestore
   * operations in c.  Do not use filestore methods on c while the returned
   * iterator is live.  (Filling in a transaction is no problem).
   *
   * @return iterator, null on error
   */
  virtual ObjectMap::ObjectMapIterator get_omap_iterator(
    coll_t c,              ///< [in] collection
    const ghobject_t &oid  ///< [in] object
    ) = 0;

  virtual void sync(Context *onsync) {}
  virtual void sync() {}
  virtual void flush() {}
  virtual void sync_and_flush() {}

  virtual int dump_journal(ostream& out) { return -EOPNOTSUPP; }

  virtual int snapshot(const string& name) { return -EOPNOTSUPP; }

  /**
   * Set and get internal fsid for this instance. No external data is modified
   */
  virtual void set_fsid(uuid_d u) = 0;
  virtual uuid_d get_fsid() = 0;

  // DEBUG
  virtual void inject_data_error(const ghobject_t &oid) {}
  virtual void inject_mdata_error(const ghobject_t &oid) {}
};
WRITE_CLASS_ENCODER(ObjectStore::Transaction)
WRITE_CLASS_ENCODER(ObjectStore::Transaction::TransactionData)

ostream& operator<<(ostream& out, const ObjectStore::Sequencer& s);

#endif
