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

#include "include/types.h"
#include "include/buffer.h"
#include "osd/osd_types.h"
#include <errno.h>

#include "HashIndex.h"

#include "common/debug.h"
#define dout_subsys ceph_subsys_filestore

const string HashIndex::SUBDIR_ATTR = "contents";
const string HashIndex::IN_PROGRESS_OP_TAG = "in_progress_op";

int HashIndex::cleanup() {
  bufferlist bl;
  int r = get_attr_path(vector<string>(), IN_PROGRESS_OP_TAG, bl);
  if (r < 0) {
    // No in progress operations!
    return 0;
  }
  bufferlist::iterator i = bl.begin();
  InProgressOp in_progress(i);
  subdir_info_s info;
  r = get_info(in_progress.path, &info);
  if (r == -ENOENT) {
    return end_split_or_merge(in_progress.path);
  } else if (r < 0) {
    return r;
  }

  if (in_progress.is_split())
    return complete_split(in_progress.path, info);
  else if (in_progress.is_merge())
    return complete_merge(in_progress.path, info);
  else if (in_progress.is_col_split()) {
    for (vector<string>::iterator i = in_progress.path.begin();
	 i != in_progress.path.end();
	 ++i) {
      vector<string> path(in_progress.path.begin(), i);
      int r = reset_attr(path);
      if (r < 0)
	return r;
    }
    return 0;
  }
  else
    return -EINVAL;
}

int HashIndex::reset_attr(
  const vector<string> &path)
{
  int exists = 0;
  int r = path_exists(path, &exists);
  if (r < 0)
    return r;
  if (!exists)
    return 0;
  map<string, ghobject_t> objects;
  set<string> subdirs;
  r = list_objects(path, 0, 0, &objects);
  if (r < 0)
    return r;
  r = list_subdirs(path, &subdirs);
  if (r < 0)
    return r;

  subdir_info_s info;
  info.hash_level = path.size();
  info.objs = objects.size();
  info.subdirs = subdirs.size();
  return set_info(path, info);
}

int HashIndex::col_split_level(
  HashIndex &from,
  HashIndex &to,
  const vector<string> &path,
  uint32_t inbits,
  uint32_t match,
  unsigned *mkdirred)
{
  /* For each subdir, move, recurse, or ignore based on comparing the low order
   * bits of the hash represented by the subdir path with inbits, match passed
   * in.
   */
  set<string> subdirs;
  int r = from.list_subdirs(path, &subdirs);
  if (r < 0)
    return r;
  map<string, ghobject_t> objects;
  r = from.list_objects(path, 0, 0, &objects);
  if (r < 0)
    return r;

  set<string> to_move;
  for (set<string>::iterator i = subdirs.begin();
       i != subdirs.end();
       ++i) {
    uint32_t bits = 0;
    uint32_t hash = 0;
    vector<string> sub_path(path.begin(), path.end());
    sub_path.push_back(*i);
    path_to_hobject_hash_prefix(sub_path, &bits, &hash);
    if (bits < inbits) {
      if (hobject_t::match_hash(hash, bits, match)) {
	r = col_split_level(
	  from,
	  to,
	  sub_path,
	  inbits,
	  match,
	  mkdirred);
	if (r < 0)
	  return r;
	if (*mkdirred > path.size())
	  *mkdirred = path.size();
      } // else, skip, doesn't need to be moved or recursed into
    } else {
      if (hobject_t::match_hash(hash, inbits, match)) {
	to_move.insert(*i);
      }
    } // else, skip, doesn't need to be moved or recursed into
  }

  /* Then, do the same for each object */
  map<string, ghobject_t> objs_to_move;
  for (map<string, ghobject_t>::iterator i = objects.begin();
       i != objects.end();
       ++i) {
    if (i->second.match(inbits, match)) {
      objs_to_move.insert(*i);
    }
  }

  if (objs_to_move.empty() && to_move.empty())
    return 0;

  // Make parent directories as needed
  while (*mkdirred < path.size()) {
    ++*mkdirred;
    int exists = 0;
    vector<string> creating_path(path.begin(), path.begin()+*mkdirred);
    r = to.path_exists(creating_path, &exists);
    if (r < 0)
      return r;
    if (exists)
      continue;
    subdir_info_s info;
    info.objs = 0;
    info.subdirs = 0;
    info.hash_level = creating_path.size();
    if (*mkdirred < path.size() - 1)
      info.subdirs = 1;
    r = to.start_col_split(creating_path);
    if (r < 0)
      return r;
    r = to.create_path(creating_path);
    if (r < 0)
      return r;
    r = to.set_info(creating_path, info);
    if (r < 0)
      return r;
    r = to.end_split_or_merge(creating_path);
    if (r < 0)
      return r;
  }

  subdir_info_s from_info;
  subdir_info_s to_info;
  r = from.get_info(path, &from_info);
  if (r < 0)
    return r;
  r = to.get_info(path, &to_info);
  if (r < 0)
    return r;

  from.start_col_split(path);
  to.start_col_split(path);

  // Do subdir moves
  for (set<string>::iterator i = to_move.begin();
       i != to_move.end();
       ++i) {
    from_info.subdirs--;
    to_info.subdirs++;
    r = move_subdir(from, to, path, *i);
    if (r < 0)
      return r;
  }

  for (map<string, ghobject_t>::iterator i = objs_to_move.begin();
       i != objs_to_move.end();
       ++i) {
    from_info.objs--;
    to_info.objs++;
    r = move_object(from, to, path, *i);
    if (r < 0)
      return r;
  }
       

  r = to.set_info(path, to_info);
  if (r < 0)
    return r;
  r = from.set_info(path, from_info);
  if (r < 0)
    return r;
  from.end_split_or_merge(path);
  to.end_split_or_merge(path);
  return 0;
}

int HashIndex::_split(
  uint32_t match,
  uint32_t bits,
  CollectionIndex* dest) {
  assert(collection_version() == dest->collection_version());
  unsigned mkdirred = 0;
  return col_split_level(
    *this,
    *static_cast<HashIndex*>(dest),
    vector<string>(),
    bits,
    match,
    &mkdirred);
}

int HashIndex::_init() {
  subdir_info_s info;
  vector<string> path;
  return set_info(path, info);
}

/* LFNIndex virtual method implementations */
int HashIndex::_created(const vector<string> &path,
			const ghobject_t &oid,
			const string &mangled_name) {
  subdir_info_s info;
  int r;
  r = get_info(path, &info);
  if (r < 0)
    return r;
  info.objs++;
  r = set_info(path, info);
  if (r < 0)
    return r;

  if (must_split(info)) {
    int r = initiate_split(path, info);
    if (r < 0)
      return r;
    return complete_split(path, info);
  } else {
    return 0;
  }
}

int HashIndex::_remove(const vector<string> &path,
		       const ghobject_t &oid,
		       const string &mangled_name) {
  int r;
  r = remove_object(path, oid);
  if (r < 0)
    return r;
  subdir_info_s info;
  r = get_info(path, &info);
  if (r < 0)
    return r;
  info.objs--;
  r = set_info(path, info);
  if (r < 0)
    return r;
  if (must_merge(info)) {
    r = initiate_merge(path, info);
    if (r < 0)
      return r;
    return complete_merge(path, info);
  } else {
    return 0;
  }
}

int HashIndex::_lookup(const ghobject_t &oid,
		       vector<string> *path,
		       string *mangled_name,
		       int *exists_out) {
  vector<string> path_comp;
  get_path_components(oid, &path_comp);
  vector<string>::iterator next = path_comp.begin();
  int exists;
  while (1) {
    int r = path_exists(*path, &exists);
    if (r < 0)
      return r;
    if (!exists) {
      if (path->empty())
	return -ENOENT;
      path->pop_back();
      break;
    }
    if (next == path_comp.end())
      break;
    path->push_back(*(next++));
  }
  return get_mangled_name(*path, oid, mangled_name, exists_out);
}

int HashIndex::_collection_list(vector<ghobject_t> *ls) {
  vector<string> path;
  return list_by_hash(path, 0, 0, 0, 0, ls);
}

int HashIndex::_collection_list_partial(const ghobject_t &start,
					int min_count,
					int max_count,
					snapid_t seq,
					vector<ghobject_t> *ls,
					ghobject_t *next) {
  vector<string> path;
  ghobject_t _next;
  if (!next)
    next = &_next;
  *next = start;
  dout(20) << "_collection_list_partial " << start << " " << min_count << "-" << max_count << " ls.size " << ls->size() << dendl;
  return list_by_hash(path, min_count, max_count, seq, next, ls);
}

int HashIndex::prep_delete() {
  return recursive_remove(vector<string>());
}

int HashIndex::_pre_hash_collection(uint32_t pg_num, uint64_t expected_num_objs) {
  int ret;
  vector<string> path;
  subdir_info_s root_info;
  // Make sure there is neither objects nor sub-folders
  // in this collection
  ret = get_info(path, &root_info);
  if (ret < 0)
    return ret;

  // Do the folder splitting first
  ret = pre_split_folder(pg_num, expected_num_objs);
  if (ret < 0)
    return ret;
  // Initialize the folder info starting from root
  return init_split_folder(path, 0);
}

int HashIndex::pre_split_folder(uint32_t pg_num, uint64_t expected_num_objs)
{
  // If folder merging is enabled (by setting the threshold positive),
  // no need to split
  if (merge_threshold > 0)
    return 0;
  const coll_t c = coll();
  // Do not split if the expected number of objects in this collection is zero (by default)
  if (expected_num_objs == 0)
    return 0;

  // Calculate the number of leaf folders (which actually store files)
  // need to be created
  const uint64_t objs_per_folder = (uint64_t)(abs(merge_threshold)) * (uint64_t)split_multiplier * 16;
  uint64_t leavies = expected_num_objs / objs_per_folder ;
  // No need to split
  if (leavies == 0 || expected_num_objs == objs_per_folder)
    return 0;

  spg_t spgid;
  if (!c.is_pg_prefix(spgid))
    return -EINVAL;
  const ps_t ps = spgid.pgid.ps();

  // the most significant bits of pg_num
  const int pg_num_bits = calc_num_bits(pg_num - 1);
  ps_t tmp_id = ps;
  // calculate the number of levels we only create one sub folder
  int num = pg_num_bits / 4;
  // pg num's hex value is like 1xxx,xxxx,xxxx but not 1111,1111,1111,
  // so that splitting starts at level 3
  if (pg_num_bits % 4 == 0 && pg_num < ((uint32_t)1 << pg_num_bits)) {
    --num;
  }

  int ret;
  // Start with creation that only has one subfolder
  vector<string> paths;
  int dump_num = num;
  while (num-- > 0) {
    ps_t v = tmp_id & 0x0000000f;
    paths.push_back(to_hex(v));
    ret = create_path(paths);
    if (ret < 0 && ret != -EEXIST)
      return ret;
    tmp_id = tmp_id >> 4;
  }

  // Starting from here, we can split by creating multiple subfolders
  const int left_bits = pg_num_bits - dump_num * 4;
  // this variable denotes how many bits (for this level) that can be
  // used for sub folder splitting
  int split_bits = 4 - left_bits;
  // the below logic is inspired by rados.h#ceph_stable_mod,
  // it basically determines how many sub-folders should we
  // create for splitting
  if (((1 << (pg_num_bits - 1)) | ps) >= pg_num) {
    ++split_bits;
  }
  const uint32_t subs = (1 << split_bits);
  // Calculate how many levels we create starting from here
  int level  = 0;
  leavies /= subs;
  while (leavies > 1) {
    ++level;
    leavies = leavies >> 4;
  }
  for (uint32_t i = 0; i < subs; ++i) {
    int v = tmp_id | (i << ((4 - split_bits) % 4));
    paths.push_back(to_hex(v));
    ret = create_path(paths);
    if (ret < 0 && ret != -EEXIST)
      return ret;
    ret = recursive_create_path(paths, level);
    if (ret < 0)
      return ret;
    paths.pop_back();
  }
  return 0;
}

int HashIndex::init_split_folder(vector<string> &path, uint32_t hash_level)
{
  // Get the number of sub directories for the current path
  set<string> subdirs;
  int ret = list_subdirs(path, &subdirs);
  if (ret < 0)
    return ret;
  subdir_info_s info;
  info.subdirs = subdirs.size();
  info.hash_level = hash_level;
  ret = set_info(path, info);
  if (ret < 0)
    return ret;
  ret = fsync_dir(path);
  if (ret < 0)
    return ret;

  // Do the same for subdirs
  set<string>::const_iterator iter;
  for (iter = subdirs.begin(); iter != subdirs.end(); ++iter) {
    path.push_back(*iter);
    ret = init_split_folder(path, hash_level + 1);
    if (ret < 0)
      return ret;
    path.pop_back();
  }
  return 0;
}

int HashIndex::recursive_create_path(vector<string>& path, int level)
{
  if (level == 0)
    return 0;
  for (int i = 0; i < 16; ++i) {
    path.push_back(to_hex(i));
    int ret = create_path(path);
    if (ret < 0 && ret != -EEXIST)
      return ret;
    ret = recursive_create_path(path, level - 1);
    if (ret < 0)
      return ret;
    path.pop_back();
  }
  return 0;
}

int HashIndex::recursive_remove(const vector<string> &path) {
  set<string> subdirs;
  int r = list_subdirs(path, &subdirs);
  if (r < 0)
    return r;
  map<string, ghobject_t> objects;
  r = list_objects(path, 0, 0, &objects);
  if (r < 0)
    return r;
  if (!objects.empty())
    return -ENOTEMPTY;
  vector<string> subdir(path);
  for (set<string>::iterator i = subdirs.begin();
       i != subdirs.end();
       ++i) {
    subdir.push_back(*i);
    r = recursive_remove(subdir);
    if (r < 0)
      return r;
    subdir.pop_back();
  }
  return remove_path(path);
}

int HashIndex::start_col_split(const vector<string> &path) {
  bufferlist bl;
  InProgressOp op_tag(InProgressOp::COL_SPLIT, path);
  op_tag.encode(bl);
  int r = add_attr_path(vector<string>(), IN_PROGRESS_OP_TAG, bl);
  if (r < 0)
    return r;
  return fsync_dir(vector<string>());
}

int HashIndex::start_split(const vector<string> &path) {
  bufferlist bl;
  InProgressOp op_tag(InProgressOp::SPLIT, path);
  op_tag.encode(bl);
  int r = add_attr_path(vector<string>(), IN_PROGRESS_OP_TAG, bl);
  if (r < 0)
    return r;
  return fsync_dir(vector<string>());
}

int HashIndex::start_merge(const vector<string> &path) {
  bufferlist bl;
  InProgressOp op_tag(InProgressOp::MERGE, path);
  op_tag.encode(bl);
  int r = add_attr_path(vector<string>(), IN_PROGRESS_OP_TAG, bl);
  if (r < 0)
    return r;
  return fsync_dir(vector<string>());
}

int HashIndex::end_split_or_merge(const vector<string> &path) {
  return remove_attr_path(vector<string>(), IN_PROGRESS_OP_TAG);
}

int HashIndex::get_info(const vector<string> &path, subdir_info_s *info) {
  bufferlist buf;
  int r = get_attr_path(path, SUBDIR_ATTR, buf);
  if (r < 0)
    return r;
  bufferlist::iterator bufiter = buf.begin();
  info->decode(bufiter);
  assert(path.size() == (unsigned)info->hash_level);
  return 0;
}

int HashIndex::set_info(const vector<string> &path, const subdir_info_s &info) {
  bufferlist buf;
  assert(path.size() == (unsigned)info.hash_level);
  info.encode(buf);
  return add_attr_path(path, SUBDIR_ATTR, buf);
}

bool HashIndex::must_merge(const subdir_info_s &info) {
  return (info.hash_level > 0 &&
          merge_threshold > 0 &&
	  info.objs < (unsigned)merge_threshold &&
	  info.subdirs == 0);
}

bool HashIndex::must_split(const subdir_info_s &info) {
  return (info.hash_level < (unsigned)MAX_HASH_LEVEL &&
	  info.objs > ((unsigned)(abs(merge_threshold)) * 16 * split_multiplier));
			    
}

int HashIndex::initiate_merge(const vector<string> &path, subdir_info_s info) {
  return start_merge(path);
}

int HashIndex::complete_merge(const vector<string> &path, subdir_info_s info) {
  vector<string> dst = path;
  dst.pop_back();
  subdir_info_s dstinfo;
  int r, exists;
  r = path_exists(path, &exists);
  if (r < 0)
    return r;
  r = get_info(dst, &dstinfo);
  if (r < 0)
    return r;
  if (exists) {
    r = move_objects(path, dst);
    if (r < 0)
      return r;
    r = reset_attr(dst);
    if (r < 0)
      return r;
    r = remove_path(path);
    if (r < 0)
      return r;
  }
  if (must_merge(dstinfo)) {
    r = initiate_merge(dst, dstinfo);
    if (r < 0)
      return r;
    r = fsync_dir(dst);
    if (r < 0)
      return r;
    return complete_merge(dst, dstinfo);
  }
  r = fsync_dir(dst);
  if (r < 0)
    return r;
  return end_split_or_merge(path);
}

int HashIndex::initiate_split(const vector<string> &path, subdir_info_s info) {
  return start_split(path);
}

int HashIndex::complete_split(const vector<string> &path, subdir_info_s info) {
  int level = info.hash_level;
  map<string, ghobject_t> objects;
  vector<string> dst = path;
  int r;
  dst.push_back("");
  r = list_objects(path, 0, 0, &objects);
  if (r < 0)
    return r;
  set<string> subdirs;
  r = list_subdirs(path, &subdirs);
  if (r < 0)
    return r;
  map<string, map<string, ghobject_t> > mapped;
  map<string, ghobject_t> moved;
  int num_moved = 0;
  for (map<string, ghobject_t>::iterator i = objects.begin();
       i != objects.end();
       ++i) {
    vector<string> new_path;
    get_path_components(i->second, &new_path);
    mapped[new_path[level]][i->first] = i->second;
  }
  for (map<string, map<string, ghobject_t> >::iterator i = mapped.begin();
       i != mapped.end();
       ) {
    dst[level] = i->first;
    /* If the info already exists, it must be correct,
     * we may be picking up a partially finished split */
    subdir_info_s temp;
    // subdir has already been fully copied
    if (subdirs.count(i->first) && !get_info(dst, &temp)) {
      for (map<string, ghobject_t>::iterator j = i->second.begin();
	   j != i->second.end();
	   ++j) {
	moved[j->first] = j->second;
	num_moved++;
	objects.erase(j->first);
      }
      ++i;
      continue;
    }

    subdir_info_s info_new;
    info_new.objs = i->second.size();
    info_new.subdirs = 0;
    info_new.hash_level = level + 1;
    if (must_merge(info_new) && !subdirs.count(i->first)) {
      mapped.erase(i++);
      continue;
    }

    // Subdir doesn't yet exist
    if (!subdirs.count(i->first)) {
      info.subdirs += 1;
      r = create_path(dst);
      if (r < 0)
	return r;
    } // else subdir has been created but only partially copied

    for (map<string, ghobject_t>::iterator j = i->second.begin();
	 j != i->second.end();
	 ++j) {
      moved[j->first] = j->second;
      num_moved++;
      objects.erase(j->first);
      r = link_object(path, dst, j->second, j->first);
      // May be a partially finished split
      if (r < 0 && r != -EEXIST) {
	return r;
      }
    }

    r = fsync_dir(dst);
    if (r < 0)
      return r;

    // Presence of info must imply that all objects have been copied
    r = set_info(dst, info_new);
    if (r < 0)
      return r;

    r = fsync_dir(dst);
    if (r < 0)
      return r;

    ++i;
  }
  r = remove_objects(path, moved, &objects);
  if (r < 0)
    return r;
  info.objs = objects.size();
  r = reset_attr(path);
  if (r < 0)
    return r;
  r = fsync_dir(path);
  if (r < 0)
    return r;
  return end_split_or_merge(path);
}

void HashIndex::get_path_components(const ghobject_t &oid,
				    vector<string> *path) {
  char buf[MAX_HASH_LEVEL + 1];
  snprintf(buf, sizeof(buf), "%.*X", MAX_HASH_LEVEL, (uint32_t)oid.hobj.get_filestore_key());

  // Path components are the hex characters of oid.hobj.hash, least
  // significant first
  for (int i = 0; i < MAX_HASH_LEVEL; ++i) {
    path->push_back(string(&buf[i], 1));
  }
}

string HashIndex::get_hash_str(uint32_t hash) {
  char buf[MAX_HASH_LEVEL + 1];
  snprintf(buf, sizeof(buf), "%.*X", MAX_HASH_LEVEL, hash);
  string retval;
  for (int i = 0; i < MAX_HASH_LEVEL; ++i) {
    retval.push_back(buf[MAX_HASH_LEVEL - 1 - i]);
  }
  return retval;
}

string HashIndex::get_path_str(const ghobject_t &oid) {
  assert(!oid.is_max());
  return get_hash_str(oid.hobj.get_hash());
}

uint32_t HashIndex::hash_prefix_to_hash(string prefix) {
  while (prefix.size() < sizeof(uint32_t) * 2) {
    prefix.push_back('0');
  }
  uint32_t hash;
  sscanf(prefix.c_str(), "%x", &hash);
  // nibble reverse
  hash = ((hash & 0x0f0f0f0f) << 4) | ((hash & 0xf0f0f0f0) >> 4);
  hash = ((hash & 0x00ff00ff) << 8) | ((hash & 0xff00ff00) >> 8);
  hash = ((hash & 0x0000ffff) << 16) | ((hash & 0xffff0000) >> 16);
  return hash;
}

int HashIndex::get_path_contents_by_hash(const vector<string> &path,
					 const string *lower_bound,
					 const ghobject_t *next_object,
					 const snapid_t *seq,
					 set<string> *hash_prefixes,
					 set<pair<string, ghobject_t> > *objects) {
  set<string> subdirs;
  map<string, ghobject_t> rev_objects;
  int r;
  string cur_prefix;
  for (vector<string>::const_iterator i = path.begin();
       i != path.end();
       ++i) {
    cur_prefix.append(*i);
  }
  r = list_objects(path, 0, 0, &rev_objects);
  if (r < 0)
    return r;
  for (map<string, ghobject_t>::iterator i = rev_objects.begin();
       i != rev_objects.end();
       ++i) {
    string hash_prefix = get_path_str(i->second);
    if (lower_bound && hash_prefix < *lower_bound)
      continue;
    if (next_object && i->second < *next_object)
      continue;
    if (seq && i->second.hobj.snap < *seq)
      continue;
    hash_prefixes->insert(hash_prefix);
    objects->insert(pair<string, ghobject_t>(hash_prefix, i->second));
  }
  r = list_subdirs(path, &subdirs);
  if (r < 0)
    return r;
  for (set<string>::iterator i = subdirs.begin();
       i != subdirs.end();
       ++i) {
    string candidate = cur_prefix + *i;
    if (lower_bound && candidate < lower_bound->substr(0, candidate.size()))
      continue;
    if (next_object &&
        (next_object->is_max() ||
	 candidate < get_path_str(*next_object).substr(0, candidate.size())))
      continue;
    hash_prefixes->insert(cur_prefix + *i);
  }
  return 0;
}

int HashIndex::list_by_hash(const vector<string> &path,
			    int min_count,
			    int max_count,
			    snapid_t seq,
			    ghobject_t *next,
			    vector<ghobject_t> *out) {
  assert(out);
  vector<string> next_path = path;
  next_path.push_back("");
  set<string> hash_prefixes;
  set<pair<string, ghobject_t> > objects;
  int r = get_path_contents_by_hash(path,
				    NULL,
				    next,
				    &seq,
				    &hash_prefixes,
				    &objects);
  if (r < 0)
    return r;
  dout(20) << " prefixes " << hash_prefixes << dendl;
  for (set<string>::iterator i = hash_prefixes.begin();
       i != hash_prefixes.end();
       ++i) {
    set<pair<string, ghobject_t> >::iterator j = objects.lower_bound(
      make_pair(*i, ghobject_t()));
    if (j == objects.end() || j->first != *i) {
      if (min_count > 0 && out->size() > (unsigned)min_count) {
	if (next)
	  *next = ghobject_t(hobject_t("", "", CEPH_NOSNAP, hash_prefix_to_hash(*i), -1, ""));
	return 0;
      }
      *(next_path.rbegin()) = *(i->rbegin());
      ghobject_t next_recurse;
      if (next)
	next_recurse = *next;
      r = list_by_hash(next_path,
		       min_count,
		       max_count,
		       seq,
		       &next_recurse,
		       out);

      if (r < 0)
	return r;
      if (!next_recurse.is_max()) {
	if (next)
	  *next = next_recurse;
	return 0;
      }
    } else {
      while (j != objects.end() && j->first == *i) {
	if (max_count > 0 && out->size() == (unsigned)max_count) {
	  if (next)
	    *next = j->second;
	  return 0;
	}
	if (!next || j->second >= *next) {
	  out->push_back(j->second);
	}
	++j;
      }
    }
  }
  if (next)
    *next = ghobject_t(hobject_t::get_max());
  return 0;
}
