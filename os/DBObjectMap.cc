// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-

#include "include/int_types.h"
#include "include/buffer.h"

#include <iostream>
#include <set>
#include <map>
#include <string>
#include "include/memory.h"
#include <vector>

#include "ObjectMap.h"
#include "KeyValueDB.h"
#include "DBObjectMap.h"
#include <errno.h>

#include "common/debug.h"
#include "common/config.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_filestore
#undef dout_prefix
#define dout_prefix *_dout << "filestore "

const string DBObjectMap::USER_PREFIX = "_USER_";
const string DBObjectMap::XATTR_PREFIX = "_AXATTR_";
const string DBObjectMap::SYS_PREFIX = "_SYS_";
const string DBObjectMap::COMPLETE_PREFIX = "_COMPLETE_";
const string DBObjectMap::HEADER_KEY = "HEADER";
const string DBObjectMap::USER_HEADER_KEY = "USER_HEADER";
const string DBObjectMap::GLOBAL_STATE_KEY = "HEADER";
const string DBObjectMap::HOBJECT_TO_SEQ = "_HOBJTOSEQ_";

// Legacy
const string DBObjectMap::LEAF_PREFIX = "_LEAF_";
const string DBObjectMap::REVERSE_LEAF_PREFIX = "_REVLEAF_";

static void append_escaped(const string &in, string *out)
{
  for (string::const_iterator i = in.begin(); i != in.end(); ++i) {
    if (*i == '%') {
      out->push_back('%');
      out->push_back('p');
    } else if (*i == '.') {
      out->push_back('%');
      out->push_back('e');
    } else if (*i == '_') {
      out->push_back('%');
      out->push_back('u');
    } else {
      out->push_back(*i);
    }
  }
}

bool DBObjectMap::check(std::ostream &out)
{
  bool retval = true;
  map<uint64_t, uint64_t> parent_to_num_children;
  map<uint64_t, uint64_t> parent_to_actual_num_children;
  KeyValueDB::Iterator iter = db->get_iterator(HOBJECT_TO_SEQ);
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    _Header header;
    assert(header.num_children == 1);
    header.num_children = 0; // Hack for leaf node
    bufferlist bl = iter->value();
    while (true) {
      bufferlist::iterator bliter = bl.begin();
      header.decode(bliter);
      if (header.seq != 0)
	parent_to_actual_num_children[header.seq] = header.num_children;
      if (header.parent == 0)
	break;

      if (!parent_to_num_children.count(header.parent))
	parent_to_num_children[header.parent] = 0;
      parent_to_num_children[header.parent]++;
      if (parent_to_actual_num_children.count(header.parent))
	break;

      set<string> to_get;
      map<string, bufferlist> got;
      to_get.insert(HEADER_KEY);
      db->get(sys_parent_prefix(header), to_get, &got);
      if (got.empty()) {
	out << "Missing: seq " << header.parent << std::endl;
	retval = false;
	break;
      } else {
	bl = got.begin()->second;
      }
    }
  }

  for (map<uint64_t, uint64_t>::iterator i = parent_to_num_children.begin();
       i != parent_to_num_children.end();
       parent_to_num_children.erase(i++)) {
    if (!parent_to_actual_num_children.count(i->first))
      continue;
    if (parent_to_actual_num_children[i->first] != i->second) {
      out << "Invalid: seq " << i->first << " recorded children: "
	  << parent_to_actual_num_children[i->first] << " found: "
	  << i->second << std::endl;
      retval = false;
    }
    parent_to_actual_num_children.erase(i->first);
  }
  return retval;
}

string DBObjectMap::ghobject_key(const ghobject_t &oid)
{
  string out;
  append_escaped(oid.hobj.oid.name, &out);
  out.push_back('.');
  append_escaped(oid.hobj.get_key(), &out);
  out.push_back('.');
  append_escaped(oid.hobj.nspace, &out);
  out.push_back('.');

  char snap_with_hash[1000];
  char *t = snap_with_hash;
  char *end = t + sizeof(snap_with_hash);
  if (oid.hobj.snap == CEPH_NOSNAP)
    t += snprintf(t, end - t, "head");
  else if (oid.hobj.snap == CEPH_SNAPDIR)
    t += snprintf(t, end - t, "snapdir");
  else
    t += snprintf(t, end - t, "%llx", (long long unsigned)oid.hobj.snap);

  if (oid.hobj.pool == -1)
    t += snprintf(t, end - t, ".none");
  else
    t += snprintf(t, end - t, ".%llx", (long long unsigned)oid.hobj.pool);
  t += snprintf(t, end - t, ".%.*X", (int)(sizeof(uint32_t)*2), oid.hobj.get_hash());

  if (oid.generation != ghobject_t::NO_GEN ||
      oid.shard_id != shard_id_t::NO_SHARD) {
    t += snprintf(t, end - t, ".%llx", (long long unsigned)oid.generation);
    t += snprintf(t, end - t, ".%x", (int)oid.shard_id);
  }
  out += string(snap_with_hash);
  return out;
}

//    ok: pglog%u3%efs1...0.none.0017B237
//   bad: plana8923501-10...4c.3.ffffffffffffffff.2
// fixed: plana8923501-10...4c.3.CB767F2D.ffffffffffffffff.2
// returns 0 for false, 1 for true, negative for error
int DBObjectMap::is_buggy_ghobject_key_v1(const string &in)
{
  int dots = 5;  // skip 5 .'s
  const char *s = in.c_str();
  do {
    while (*s && *s != '.')
      ++s;
    if (!*s) {
      derr << "unexpected null at " << (int)(s-in.c_str()) << dendl;
      return -EINVAL;
    }
    ++s;
  } while (*s && --dots);
  if (!*s) {
    derr << "unexpected null at " << (int)(s-in.c_str()) << dendl;
    return -EINVAL;
  }
  // we are now either at a hash value (32 bits, 8 chars) or a generation
  // value (64 bits) '.' and shard id.  count the dots!
  int len = 0;
  while (*s && *s != '.') {
    ++s;
    ++len;
  }
  if (*s == '\0') {
    if (len != 8) {
      derr << "hash value is not 8 chars" << dendl;
      return -EINVAL;  // the hash value is always 8 chars.
    }
    return 0;
  }
  if (*s != '.') { // the shard follows.
    derr << "missing final . and shard id at " << (int)(s-in.c_str()) << dendl;
    return -EINVAL;
  }
  return 1;
}


string DBObjectMap::map_header_key(const ghobject_t &oid)
{
  return ghobject_key(oid);
}

string DBObjectMap::header_key(uint64_t seq)
{
  char buf[100];
  snprintf(buf, sizeof(buf), "%.*" PRId64, (int)(2*sizeof(seq)), seq);
  return string(buf);
}

string DBObjectMap::complete_prefix(Header header)
{
  return USER_PREFIX + header_key(header->seq) + COMPLETE_PREFIX;
}

string DBObjectMap::user_prefix(Header header)
{
  return USER_PREFIX + header_key(header->seq) + USER_PREFIX;
}

string DBObjectMap::sys_prefix(Header header)
{
  return USER_PREFIX + header_key(header->seq) + SYS_PREFIX;
}

string DBObjectMap::xattr_prefix(Header header)
{
  return USER_PREFIX + header_key(header->seq) + XATTR_PREFIX;
}

string DBObjectMap::sys_parent_prefix(_Header header)
{
  return USER_PREFIX + header_key(header.parent) + SYS_PREFIX;
}

int DBObjectMap::DBObjectMapIteratorImpl::init()
{
  invalid = false;
  if (ready) {
    return 0;
  }
  assert(!parent_iter);
  if (header->parent) {
    Header parent = map->lookup_parent(header);
    if (!parent) {
      assert(0);
      return -EINVAL;
    }
    parent_iter.reset(new DBObjectMapIteratorImpl(map, parent));
  }
  key_iter = map->db->get_iterator(map->user_prefix(header));
  assert(key_iter);
  complete_iter = map->db->get_iterator(map->complete_prefix(header));
  assert(complete_iter);
  cur_iter = key_iter;
  assert(cur_iter);
  ready = true;
  return 0;
}

ObjectMap::ObjectMapIterator DBObjectMap::get_iterator(
  const ghobject_t &oid)
{
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header)
    return ObjectMapIterator(new EmptyIteratorImpl());
  DBObjectMapIterator iter = _get_iterator(header);
  iter->hlock.swap(hl);
  return iter;
}

int DBObjectMap::DBObjectMapIteratorImpl::seek_to_first()
{
  init();
  r = 0;
  if (parent_iter) {
    r = parent_iter->seek_to_first();
    if (r < 0)
      return r;
  }
  r = key_iter->seek_to_first();
  if (r < 0)
    return r;
  return adjust();
}

int DBObjectMap::DBObjectMapIteratorImpl::seek_to_last()
{
  init();
  r = 0;
  if (parent_iter) {
    r = parent_iter->seek_to_last();
    if (r < 0)
      return r;
    if (parent_iter->valid())
      r = parent_iter->next();
    if (r < 0)
      return r;
  }
  r = key_iter->seek_to_last();
  if (r < 0)
    return r;
  if (key_iter->valid())
    r = key_iter->next();
  if (r < 0)
    return r;
  return adjust();
}

int DBObjectMap::DBObjectMapIteratorImpl::lower_bound(const string &to)
{
  init();
  r = 0;
  if (parent_iter) {
    r = parent_iter->lower_bound(to);
    if (r < 0)
      return r;
  }
  r = key_iter->lower_bound(to);
  if (r < 0)
    return r;
  return adjust();
}

int DBObjectMap::DBObjectMapIteratorImpl::upper_bound(const string &after)
{
  init();
  r = 0;
  if (parent_iter) {
    r = parent_iter->upper_bound(after);
    if (r < 0)
      return r;
  }
  r = key_iter->upper_bound(after);
  if (r < 0)
    return r;
  return adjust();
}

bool DBObjectMap::DBObjectMapIteratorImpl::valid()
{
  bool valid = !invalid && ready;
  assert(!valid || cur_iter->valid());
  return valid;
}

bool DBObjectMap::DBObjectMapIteratorImpl::valid_parent()
{
  if (parent_iter && parent_iter->valid() &&
      (!key_iter->valid() || key_iter->key() > parent_iter->key()))
    return true;
  return false;
}

int DBObjectMap::DBObjectMapIteratorImpl::next()
{
  assert(cur_iter->valid());
  assert(valid());
  cur_iter->next();
  return adjust();
}

int DBObjectMap::DBObjectMapIteratorImpl::next_parent()
{
  if (!parent_iter || !parent_iter->valid()) {
    invalid = true;
    return 0;
  }
  r = next();
  if (r < 0)
    return r;
  if (!valid() || on_parent() || !parent_iter->valid())
    return 0;

  return lower_bound(parent_iter->key());
}

int DBObjectMap::DBObjectMapIteratorImpl::in_complete_region(const string &to_test,
							     string *begin,
							     string *end)
{
  complete_iter->upper_bound(to_test);
  if (complete_iter->valid())
    complete_iter->prev();
  else
    complete_iter->seek_to_last();

  if (!complete_iter->valid())
    return false;

  string _end;
  if (begin)
    *begin = complete_iter->key();
  _end = string(complete_iter->value().c_str());
  if (end)
    *end = _end;
  return (to_test >= complete_iter->key()) && (!_end.size() || _end > to_test);
}

/**
 * Moves parent_iter to the next position both out of the complete_region and 
 * not equal to key_iter.  Then, we set cur_iter to parent_iter if valid and
 * less than key_iter and key_iter otherwise.
 */
int DBObjectMap::DBObjectMapIteratorImpl::adjust()
{
  string begin, end;
  while (parent_iter && parent_iter->valid()) {
    if (in_complete_region(parent_iter->key(), &begin, &end)) {
      if (end.size() == 0) {
	parent_iter->seek_to_last();
	if (parent_iter->valid())
	  parent_iter->next();
      } else
	parent_iter->lower_bound(end);
    } else if (key_iter->valid() && key_iter->key() == parent_iter->key()) {
      parent_iter->next();
    } else {
      break;
    }
  }
  if (valid_parent()) {
    cur_iter = parent_iter;
  } else if (key_iter->valid()) {
    cur_iter = key_iter;
  } else {
    invalid = true;
  }
  assert(invalid || cur_iter->valid());
  return 0;
}


string DBObjectMap::DBObjectMapIteratorImpl::key()
{
  return cur_iter->key();
}

bufferlist DBObjectMap::DBObjectMapIteratorImpl::value()
{
  return cur_iter->value();
}

int DBObjectMap::DBObjectMapIteratorImpl::status()
{
  return r;
}

int DBObjectMap::set_keys(const ghobject_t &oid,
			  const map<string, bufferlist> &set,
			  const SequencerPosition *spos)
{
  KeyValueDB::Transaction t = db->get_transaction();
  MapHeaderLock hl(this, oid);
  Header header = lookup_create_map_header(hl, oid, t);
  if (!header)
    return -EINVAL;
  if (check_spos(oid, header, spos))
    return 0;

  t->set(user_prefix(header), set);

  return db->submit_transaction(t);
}

int DBObjectMap::set_header(const ghobject_t &oid,
			    const bufferlist &bl,
			    const SequencerPosition *spos)
{
  KeyValueDB::Transaction t = db->get_transaction();
  MapHeaderLock hl(this, oid);
  Header header = lookup_create_map_header(hl, oid, t);
  if (!header)
    return -EINVAL;
  if (check_spos(oid, header, spos))
    return 0;
  _set_header(header, bl, t);
  return db->submit_transaction(t);
}

void DBObjectMap::_set_header(Header header, const bufferlist &bl,
			      KeyValueDB::Transaction t)
{
  map<string, bufferlist> to_set;
  to_set[USER_HEADER_KEY] = bl;
  t->set(sys_prefix(header), to_set);
}

int DBObjectMap::get_header(const ghobject_t &oid,
			    bufferlist *bl)
{
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header) {
    return 0;
  }
  return _get_header(header, bl);
}

int DBObjectMap::_get_header(Header header,
			     bufferlist *bl)
{
  map<string, bufferlist> out;
  while (true) {
    out.clear();
    set<string> to_get;
    to_get.insert(USER_HEADER_KEY);
    int r = db->get(sys_prefix(header), to_get, &out);
    if (r == 0 && !out.empty())
      break;
    if (r < 0)
      return r;
    Header current(header);
    if (!current->parent)
      break;
    header = lookup_parent(current);
  }

  if (!out.empty())
    bl->swap(out.begin()->second);
  return 0;
}

int DBObjectMap::clear(const ghobject_t &oid,
		       const SequencerPosition *spos)
{
  KeyValueDB::Transaction t = db->get_transaction();
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header)
    return -ENOENT;
  if (check_spos(oid, header, spos))
    return 0;
  remove_map_header(hl, oid, header, t);
  assert(header->num_children > 0);
  header->num_children--;
  int r = _clear(header, t);
  if (r < 0)
    return r;
  return db->submit_transaction(t);
}

int DBObjectMap::_clear(Header header,
			KeyValueDB::Transaction t)
{
  while (1) {
    if (header->num_children) {
      set_header(header, t);
      break;
    }
    clear_header(header, t);
    if (!header->parent)
      break;
    Header parent = lookup_parent(header);
    if (!parent) {
      return -EINVAL;
    }
    assert(parent->num_children > 0);
    parent->num_children--;
    header.swap(parent);
  }
  return 0;
}

int DBObjectMap::merge_new_complete(Header header,
				    const map<string, string> &new_complete,
				    DBObjectMapIterator iter,
				    KeyValueDB::Transaction t)
{
  KeyValueDB::Iterator complete_iter = db->get_iterator(
    complete_prefix(header)
    );
  map<string, string>::const_iterator i = new_complete.begin();
  set<string> to_remove;
  map<string, bufferlist> to_add;

  string begin, end;
  while (i != new_complete.end()) {
    string new_begin = i->first;
    string new_end = i->second;
    int r = iter->in_complete_region(new_begin, &begin, &end);
    if (r < 0)
      return r;
    if (r) {
      to_remove.insert(begin);
      new_begin = begin;
    }
    ++i;
    while (i != new_complete.end()) {
      if (!new_end.size() || i->first <= new_end) {
	if (!new_end.size() && i->second > new_end) {
	  new_end = i->second;
	}
	++i;
	continue;
      }

      r = iter->in_complete_region(new_end, &begin, &end);
      if (r < 0)
	return r;
      if (r) {
	to_remove.insert(begin);
	new_end = end;
	continue;
      }
      break;
    }
    bufferlist bl;
    bl.append(bufferptr(new_end.c_str(), new_end.size() + 1));
    to_add.insert(make_pair(new_begin, bl));
  }
  t->rmkeys(complete_prefix(header), to_remove);
  t->set(complete_prefix(header), to_add);
  return 0;
}

int DBObjectMap::copy_up_header(Header header,
				KeyValueDB::Transaction t)
{
  bufferlist bl;
  int r = _get_header(header, &bl);
  if (r < 0)
    return r;

  _set_header(header, bl, t);
  return 0;
}

int DBObjectMap::need_parent(DBObjectMapIterator iter)
{
  int r = iter->seek_to_first();
  if (r < 0)
    return r;

  if (!iter->valid())
    return 0;

  string begin, end;
  if (iter->in_complete_region(iter->key(), &begin, &end) && end == "") {
    return 0;
  }
  return 1;
}

int DBObjectMap::rm_keys(const ghobject_t &oid,
			 const set<string> &to_clear,
			 const SequencerPosition *spos)
{
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header)
    return -ENOENT;
  KeyValueDB::Transaction t = db->get_transaction();
  if (check_spos(oid, header, spos))
    return 0;
  t->rmkeys(user_prefix(header), to_clear);
  if (!header->parent) {
    return db->submit_transaction(t);
  }

  // Copy up keys from parent around to_clear
  int keep_parent;
  {
    DBObjectMapIterator iter = _get_iterator(header);
    iter->seek_to_first();
    map<string, string> new_complete;
    map<string, bufferlist> to_write;
    for(set<string>::const_iterator i = to_clear.begin();
	i != to_clear.end();
      ) {
      unsigned copied = 0;
      iter->lower_bound(*i);
      ++i;
      if (!iter->valid())
	break;
      string begin = iter->key();
      if (!iter->on_parent())
	iter->next_parent();
      if (new_complete.size() && new_complete.rbegin()->second == begin) {
	begin = new_complete.rbegin()->first;
      }
      while (iter->valid() && copied < 20) {
	if (!to_clear.count(iter->key()))
	  to_write[iter->key()].append(iter->value());
	if (i != to_clear.end() && *i <= iter->key()) {
	  ++i;
	  copied = 0;
	}

	iter->next_parent();
	copied++;
      }
      if (iter->valid()) {
	new_complete[begin] = iter->key();
      } else {
	new_complete[begin] = "";
	break;
      }
    }
    t->set(user_prefix(header), to_write);
    merge_new_complete(header, new_complete, iter, t);
    keep_parent = need_parent(iter);
    if (keep_parent < 0)
      return keep_parent;
  }
  if (!keep_parent) {
    copy_up_header(header, t);
    Header parent = lookup_parent(header);
    if (!parent)
      return -EINVAL;
    parent->num_children--;
    _clear(parent, t);
    header->parent = 0;
    set_map_header(hl, oid, *header, t);
    t->rmkeys_by_prefix(complete_prefix(header));
  }
  return db->submit_transaction(t);
}

int DBObjectMap::clear_keys_header(const ghobject_t &oid,
				   const SequencerPosition *spos)
{
  KeyValueDB::Transaction t = db->get_transaction();
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header)
    return -ENOENT;
  if (check_spos(oid, header, spos))
    return 0;

  // save old attrs
  KeyValueDB::Iterator iter = db->get_iterator(xattr_prefix(header));
  if (!iter)
    return -EINVAL;
  map<string, bufferlist> attrs;
  for (iter->seek_to_first(); !iter->status() && iter->valid(); iter->next())
    attrs.insert(make_pair(iter->key(), iter->value()));
  if (iter->status())
    return iter->status();

  // remove current header
  remove_map_header(hl, oid, header, t);
  assert(header->num_children > 0);
  header->num_children--;
  int r = _clear(header, t);
  if (r < 0)
    return r;

  // create new header
  Header newheader = generate_new_header(oid, Header());
  set_map_header(hl, oid, *newheader, t);
  if (!attrs.empty())
    t->set(xattr_prefix(newheader), attrs);
  return db->submit_transaction(t);
}

int DBObjectMap::get(const ghobject_t &oid,
		     bufferlist *_header,
		     map<string, bufferlist> *out)
{
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header)
    return -ENOENT;
  _get_header(header, _header);
  ObjectMapIterator iter = _get_iterator(header);
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    if (iter->status())
      return iter->status();
    out->insert(make_pair(iter->key(), iter->value()));
  }
  return 0;
}

int DBObjectMap::get_keys(const ghobject_t &oid,
			  set<string> *keys)
{
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header)
    return -ENOENT;
  ObjectMapIterator iter = get_iterator(oid);
  for (; iter->valid(); iter->next()) {
    if (iter->status())
      return iter->status();
    keys->insert(iter->key());
  }
  return 0;
}

int DBObjectMap::scan(Header header,
		      const set<string> &in_keys,
		      set<string> *out_keys,
		      map<string, bufferlist> *out_values)
{
  ObjectMapIterator db_iter = _get_iterator(header);
  for (set<string>::const_iterator key_iter = in_keys.begin();
       key_iter != in_keys.end();
       ++key_iter) {
    db_iter->lower_bound(*key_iter);
    if (db_iter->status())
      return db_iter->status();
    if (db_iter->valid() && db_iter->key() == *key_iter) {
      if (out_keys)
	out_keys->insert(*key_iter);
      if (out_values)
	out_values->insert(make_pair(db_iter->key(), db_iter->value()));
    }
  }
  return 0;
}

int DBObjectMap::get_values(const ghobject_t &oid,
			    const set<string> &keys,
			    map<string, bufferlist> *out)
{
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header)
    return -ENOENT;
  return scan(header, keys, 0, out);
}

int DBObjectMap::check_keys(const ghobject_t &oid,
			    const set<string> &keys,
			    set<string> *out)
{
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header)
    return -ENOENT;
  return scan(header, keys, out, 0);
}

int DBObjectMap::get_xattrs(const ghobject_t &oid,
			    const set<string> &to_get,
			    map<string, bufferlist> *out)
{
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header)
    return -ENOENT;
  return db->get(xattr_prefix(header), to_get, out);
}

int DBObjectMap::get_all_xattrs(const ghobject_t &oid,
				set<string> *out)
{
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header)
    return -ENOENT;
  KeyValueDB::Iterator iter = db->get_iterator(xattr_prefix(header));
  if (!iter)
    return -EINVAL;
  for (iter->seek_to_first(); !iter->status() && iter->valid(); iter->next())
    out->insert(iter->key());
  return iter->status();
}

int DBObjectMap::set_xattrs(const ghobject_t &oid,
			    const map<string, bufferlist> &to_set,
			    const SequencerPosition *spos)
{
  KeyValueDB::Transaction t = db->get_transaction();
  MapHeaderLock hl(this, oid);
  Header header = lookup_create_map_header(hl, oid, t);
  if (!header)
    return -EINVAL;
  if (check_spos(oid, header, spos))
    return 0;
  t->set(xattr_prefix(header), to_set);
  return db->submit_transaction(t);
}

int DBObjectMap::remove_xattrs(const ghobject_t &oid,
			       const set<string> &to_remove,
			       const SequencerPosition *spos)
{
  KeyValueDB::Transaction t = db->get_transaction();
  MapHeaderLock hl(this, oid);
  Header header = lookup_map_header(hl, oid);
  if (!header)
    return -ENOENT;
  if (check_spos(oid, header, spos))
    return 0;
  t->rmkeys(xattr_prefix(header), to_remove);
  return db->submit_transaction(t);
}

int DBObjectMap::clone(const ghobject_t &oid,
		       const ghobject_t &target,
		       const SequencerPosition *spos)
{
  if (oid == target)
    return 0;

  MapHeaderLock _l1(this, MIN(oid, target));
  MapHeaderLock _l2(this, MAX(oid, target));
  MapHeaderLock *lsource, *ltarget;
  if (oid > target) {
    lsource = &_l2;
    ltarget= &_l1;
  } else {
    lsource = &_l1;
    ltarget= &_l2;
  }

  KeyValueDB::Transaction t = db->get_transaction();
  {
    Header destination = lookup_map_header(*ltarget, target);
    if (destination) {
      remove_map_header(*ltarget, target, destination, t);
      if (check_spos(target, destination, spos))
	return 0;
      destination->num_children--;
      _clear(destination, t);
    }
  }

  Header parent = lookup_map_header(*lsource, oid);
  if (!parent)
    return db->submit_transaction(t);

  Header source = generate_new_header(oid, parent);
  Header destination = generate_new_header(target, parent);
  if (spos)
    destination->spos = *spos;

  parent->num_children = 2;
  set_header(parent, t);
  set_map_header(*lsource, oid, *source, t);
  set_map_header(*ltarget, target, *destination, t);

  map<string, bufferlist> to_set;
  KeyValueDB::Iterator xattr_iter = db->get_iterator(xattr_prefix(parent));
  for (xattr_iter->seek_to_first();
       xattr_iter->valid();
       xattr_iter->next())
    to_set.insert(make_pair(xattr_iter->key(), xattr_iter->value()));
  t->set(xattr_prefix(source), to_set);
  t->set(xattr_prefix(destination), to_set);
  t->rmkeys_by_prefix(xattr_prefix(parent));
  return db->submit_transaction(t);
}

int DBObjectMap::upgrade_to_v2()
{
  dout(1) << __func__ << " start" << dendl;
  KeyValueDB::Iterator iter = db->get_iterator(HOBJECT_TO_SEQ);
  iter->seek_to_first();
  while (iter->valid()) {
    unsigned count = 0;
    KeyValueDB::Transaction t = db->get_transaction();
    set<string> remove;
    map<string, bufferlist> add;
    for (;
        iter->valid() && count < 300;
        iter->next()) {
      dout(20) << __func__ << " key is " << iter->key() << dendl;
      int r = is_buggy_ghobject_key_v1(iter->key());
      if (r < 0) {
	derr << __func__ << " bad key '" << iter->key() << "'" << dendl;
	return r;
      }
      if (!r) {
	dout(20) << __func__ << " " << iter->key() << " ok" << dendl;
	continue;
      }

      // decode header to get oid
      _Header hdr;
      bufferlist bl = iter->value();
      bufferlist::iterator bliter = bl.begin();
      hdr.decode(bliter);

      string newkey(ghobject_key(hdr.oid));
      dout(20) << __func__ << " " << iter->key() << " -> " << newkey << dendl;
      add[newkey] = iter->value();
      remove.insert(iter->key());
      ++count;
    }

    if (!remove.empty()) {
      dout(20) << __func__ << " updating " << remove.size() << " keys" << dendl;
      t->rmkeys(HOBJECT_TO_SEQ, remove);
      t->set(HOBJECT_TO_SEQ, add);
      int r = db->submit_transaction(t);
      if (r < 0)
	return r;
    }
  }

  state.v = 2;
  KeyValueDB::Transaction t = db->get_transaction();
  write_state(t);
  db->submit_transaction_sync(t);
  dout(1) << __func__ << " done" << dendl;
  return 0;
}

int DBObjectMap::init(bool do_upgrade)
{
  map<string, bufferlist> result;
  set<string> to_get;
  to_get.insert(GLOBAL_STATE_KEY);
  int r = db->get(SYS_PREFIX, to_get, &result);
  if (r < 0)
    return r;
  if (!result.empty()) {
    bufferlist::iterator bliter = result.begin()->second.begin();
    state.decode(bliter);
    if (state.v < 1) {
      dout(1) << "DBObjectMap is *very* old; upgrade to an older version first"
	      << dendl;
      return -ENOTSUP;
    }
    if (state.v < 2) { // Needs upgrade
      if (!do_upgrade) {
	dout(1) << "DOBjbectMap requires an upgrade,"
		<< " set filestore_update_to"
		<< dendl;
	return -ENOTSUP;
      } else {
	r = upgrade_to_v2();
	if (r < 0)
	  return r;
      }
    }
  } else {
    // New store
    state.v = 2;
    state.seq = 1;
  }
  dout(20) << "(init)dbobjectmap: seq is " << state.seq << dendl;
  return 0;
}

int DBObjectMap::sync(const ghobject_t *oid,
		      const SequencerPosition *spos) {
  KeyValueDB::Transaction t = db->get_transaction();
  write_state(t);
  if (oid) {
    assert(spos);
    MapHeaderLock hl(this, *oid);
    Header header = lookup_map_header(hl, *oid);
    if (header) {
      dout(10) << "oid: " << *oid << " setting spos to "
	       << *spos << dendl;
      header->spos = *spos;
      set_map_header(hl, *oid, *header, t);
    }
  }
  return db->submit_transaction_sync(t);
}

int DBObjectMap::write_state(KeyValueDB::Transaction _t) {
  dout(20) << "dbobjectmap: seq is " << state.seq << dendl;
  KeyValueDB::Transaction t = _t ? _t : db->get_transaction();
  bufferlist bl;
  state.encode(bl);
  map<string, bufferlist> to_write;
  to_write[GLOBAL_STATE_KEY] = bl;
  t->set(SYS_PREFIX, to_write);
  return _t ? 0 : db->submit_transaction(t);
}


DBObjectMap::Header DBObjectMap::_lookup_map_header(
  const MapHeaderLock &l,
  const ghobject_t &oid)
{
  assert(l.get_locked() == oid);

  _Header *header = new _Header();
  {
    Mutex::Locker l(cache_lock);
    if (caches.lookup(oid, header)) {
      assert(!in_use.count(header->seq));
      in_use.insert(header->seq);
      return Header(header, RemoveOnDelete(this));
    }
  }

  map<string, bufferlist> out;
  set<string> to_get;
  to_get.insert(map_header_key(oid));
  int r = db->get(HOBJECT_TO_SEQ, to_get, &out);
  if (r < 0 || out.empty()) {
    delete header;
    return Header();
  }

  Header ret(header, RemoveOnDelete(this));
  bufferlist::iterator iter = out.begin()->second.begin();
  ret->decode(iter);
  {
    Mutex::Locker l(cache_lock);
    caches.add(oid, *ret);
  }

  assert(!in_use.count(header->seq));
  in_use.insert(header->seq);
  return ret;
}

DBObjectMap::Header DBObjectMap::_generate_new_header(const ghobject_t &oid,
						      Header parent)
{
  Header header = Header(new _Header(), RemoveOnDelete(this));
  header->seq = state.seq++;
  if (parent) {
    header->parent = parent->seq;
    header->spos = parent->spos;
  }
  header->num_children = 1;
  header->oid = oid;
  assert(!in_use.count(header->seq));
  in_use.insert(header->seq);

  write_state();
  return header;
}

DBObjectMap::Header DBObjectMap::lookup_parent(Header input)
{
  Mutex::Locker l(header_lock);
  while (in_use.count(input->parent))
    header_cond.Wait(header_lock);
  map<string, bufferlist> out;
  set<string> keys;
  keys.insert(HEADER_KEY);

  dout(20) << "lookup_parent: parent " << input->parent
       << " for seq " << input->seq << dendl;
  int r = db->get(sys_parent_prefix(input), keys, &out);
  if (r < 0) {
    assert(0);
    return Header();
  }
  if (out.empty()) {
    assert(0);
    return Header();
  }

  Header header = Header(new _Header(), RemoveOnDelete(this));
  header->seq = input->parent;
  bufferlist::iterator iter = out.begin()->second.begin();
  header->decode(iter);
  dout(20) << "lookup_parent: parent seq is " << header->seq << " with parent "
       << header->parent << dendl;
  in_use.insert(header->seq);
  return header;
}

DBObjectMap::Header DBObjectMap::lookup_create_map_header(
  const MapHeaderLock &hl,
  const ghobject_t &oid,
  KeyValueDB::Transaction t)
{
  Mutex::Locker l(header_lock);
  Header header = _lookup_map_header(hl, oid);
  if (!header) {
    header = _generate_new_header(oid, Header());
    set_map_header(hl, oid, *header, t);
  }
  return header;
}

void DBObjectMap::clear_header(Header header, KeyValueDB::Transaction t)
{
  dout(20) << "clear_header: clearing seq " << header->seq << dendl;
  t->rmkeys_by_prefix(user_prefix(header));
  t->rmkeys_by_prefix(sys_prefix(header));
  t->rmkeys_by_prefix(complete_prefix(header));
  t->rmkeys_by_prefix(xattr_prefix(header));
  set<string> keys;
  keys.insert(header_key(header->seq));
  t->rmkeys(USER_PREFIX, keys);
}

void DBObjectMap::set_header(Header header, KeyValueDB::Transaction t)
{
  dout(20) << "set_header: setting seq " << header->seq << dendl;
  map<string, bufferlist> to_write;
  header->encode(to_write[HEADER_KEY]);
  t->set(sys_prefix(header), to_write);
}

void DBObjectMap::remove_map_header(
  const MapHeaderLock &l,
  const ghobject_t &oid,
  Header header,
  KeyValueDB::Transaction t)
{
  assert(l.get_locked() == oid);
  dout(20) << "remove_map_header: removing " << header->seq
	   << " oid " << oid << dendl;
  set<string> to_remove;
  to_remove.insert(map_header_key(oid));
  t->rmkeys(HOBJECT_TO_SEQ, to_remove);
  {
    Mutex::Locker l(cache_lock);
    caches.clear(oid);
  }
}

void DBObjectMap::set_map_header(
  const MapHeaderLock &l,
  const ghobject_t &oid, _Header header,
  KeyValueDB::Transaction t)
{
  assert(l.get_locked() == oid);
  dout(20) << "set_map_header: setting " << header.seq
	   << " oid " << oid << " parent seq "
	   << header.parent << dendl;
  map<string, bufferlist> to_set;
  header.encode(to_set[map_header_key(oid)]);
  t->set(HOBJECT_TO_SEQ, to_set);
  {
    Mutex::Locker l(cache_lock);
    caches.add(oid, header);
  }
}

bool DBObjectMap::check_spos(const ghobject_t &oid,
			     Header header,
			     const SequencerPosition *spos)
{
  if (!spos || *spos > header->spos) {
    stringstream out;
    if (spos)
      dout(10) << "oid: " << oid << " not skipping op, *spos "
	       << *spos << dendl;
    else
      dout(10) << "oid: " << oid << " not skipping op, *spos "
	       << "empty" << dendl;
    dout(10) << " > header.spos " << header->spos << dendl;
    return false;
  } else {
    dout(10) << "oid: " << oid << " skipping op, *spos " << *spos
	     << " <= header.spos " << header->spos << dendl;
    return true;
  }
}

int DBObjectMap::list_objects(vector<ghobject_t> *out)
{
  KeyValueDB::Iterator iter = db->get_iterator(HOBJECT_TO_SEQ);
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    bufferlist bl = iter->value();
    bufferlist::iterator bliter = bl.begin();
    _Header header;
    header.decode(bliter);
    out->push_back(header.oid);
  }
  return 0;
}
