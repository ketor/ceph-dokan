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

#include "common/debug.h"
#include "common/errno.h"
#include "common/safe_io.h"
#include "FileJournal.h"
#include "include/color.h"
#include "common/perf_counters.h"
#include "os/FileStore.h"

#include "include/compat.h"

#include <fcntl.h>
#include <limits.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include "common/blkdev.h"
#include "common/linux_version.h"

#define dout_subsys ceph_subsys_journal
#undef dout_prefix
#define dout_prefix *_dout << "journal "

const static int64_t ONE_MEG(1 << 20);

int FileJournal::_open(bool forwrite, bool create)
{
  int flags, ret;

  if (aio && !directio) {
    derr << "FileJournal::_open: aio not supported without directio; disabling aio" << dendl;
    aio = false;
  }
#ifndef HAVE_LIBAIO
  if (aio) {
    derr << "FileJournal::_open: libaio not compiled in; disabling aio" << dendl;
    aio = false;
  }
#endif

  if (forwrite) {
    flags = O_RDWR;
    if (directio)
      flags |= O_DIRECT | O_DSYNC;
  } else {
    flags = O_RDONLY;
  }
  if (create)
    flags |= O_CREAT;
  
  if (fd >= 0) {
    if (TEMP_FAILURE_RETRY(::close(fd))) {
      int err = errno;
      derr << "FileJournal::_open: error closing old fd: "
	   << cpp_strerror(err) << dendl;
    }
  }
  fd = TEMP_FAILURE_RETRY(::open(fn.c_str(), flags, 0644));
  if (fd < 0) {
    int err = errno;
    dout(2) << "FileJournal::_open unable to open journal "
	    << fn << ": " << cpp_strerror(err) << dendl;
    return -err;
  }

  struct stat st;
  ret = ::fstat(fd, &st);
  if (ret) {
    ret = errno;
    derr << "FileJournal::_open: unable to fstat journal: " << cpp_strerror(ret) << dendl;
    ret = -ret;
    goto out_fd;
  }

  if (S_ISBLK(st.st_mode)) {
    ret = _open_block_device();
  } else {
    if (aio && !force_aio) {
      derr << "FileJournal::_open: disabling aio for non-block journal.  Use "
	   << "journal_force_aio to force use of aio anyway" << dendl;
      aio = false;
    }
    ret = _open_file(st.st_size, st.st_blksize, create);
  }

  if (ret)
    goto out_fd;

#ifdef HAVE_LIBAIO
  if (aio) {
    aio_ctx = 0;
    ret = io_setup(128, &aio_ctx);
    if (ret < 0) {
      ret = errno;
      derr << "FileJournal::_open: unable to setup io_context " << cpp_strerror(ret) << dendl;
      ret = -ret;
      goto out_fd;
    }
  }
#endif

  /* We really want max_size to be a multiple of block_size. */
  max_size -= max_size % block_size;

  dout(1) << "_open " << fn << " fd " << fd
	  << ": " << max_size 
	  << " bytes, block size " << block_size
	  << " bytes, directio = " << directio
	  << ", aio = " << aio
	  << dendl;
  return 0;

 out_fd:
  VOID_TEMP_FAILURE_RETRY(::close(fd));
  return ret;
}

int FileJournal::_open_block_device()
{
  int64_t bdev_sz = 0;
  int ret = get_block_device_size(fd, &bdev_sz);
  if (ret) {
    dout(0) << __func__ << ": failed to read block device size." << dendl;
    return -EIO;
  }

  /* Check for bdev_sz too small */
  if (bdev_sz < ONE_MEG) {
    dout(0) << __func__ << ": your block device must be at least "
      << ONE_MEG << " bytes to be used for a Ceph journal." << dendl;
    return -EINVAL;
  }

  dout(10) << __func__ << ": ignoring osd journal size. "
	   << "We'll use the entire block device (size: " << bdev_sz << ")"
	   << dendl;
  max_size = bdev_sz;

  /* block devices have to write in blocks of CEPH_PAGE_SIZE */
  block_size = CEPH_PAGE_SIZE;

  if (g_conf->journal_discard) {
    discard = block_device_support_discard(fn.c_str());
    dout(10) << fn << " support discard: " << (int)discard << dendl;
  }
  _check_disk_write_cache();
  return 0;
}

void FileJournal::_check_disk_write_cache() const
{
  ostringstream hdparm_cmd;
  FILE *fp = NULL;

  if (geteuid() != 0) {
    dout(10) << "_check_disk_write_cache: not root, NOT checking disk write "
      << "cache on raw block device " << fn << dendl;
    goto done;
  }

  hdparm_cmd << "/sbin/hdparm -W " << fn;
  fp = popen(hdparm_cmd.str().c_str(), "r");
  if (!fp) {
    dout(10) << "_check_disk_write_cache: failed to run /sbin/hdparm: NOT "
      << "checking disk write cache on raw block device " << fn << dendl;
    goto done;
  }

  while (true) {
    char buf[256];
    memset(buf, 0, sizeof(buf));
    char *line = fgets(buf, sizeof(buf) - 1, fp);
    if (!line) {
      if (ferror(fp)) {
	int ret = -errno;
	derr << "_check_disk_write_cache: fgets error: " << cpp_strerror(ret)
	     << dendl;
	goto close_f;
      }
      else {
	// EOF.
	break;
      }
    }

    int on;
    if (sscanf(line, " write-caching =  %d", &on) != 1)
      continue;
    if (!on) {
      dout(10) << "_check_disk_write_cache: disk write cache is off (good) on "
	       << fn << dendl;
      break;
    }

    // is our kernel new enough?
    int ver = get_linux_version();
    if (ver == 0) {
      dout(10) << "_check_disk_write_cache: get_linux_version failed" << dendl;
    } else if (ver >= KERNEL_VERSION(2, 6, 33)) {
      dout(20) << "_check_disk_write_cache: disk write cache is on, but your "
	       << "kernel is new enough to handle it correctly. (fn:"
	       << fn << ")" << dendl;
      break;
    }
    derr << TEXT_RED
	 << " ** WARNING: disk write cache is ON on " << fn << ".\n"
	 << "    Journaling will not be reliable on kernels prior to 2.6.33\n"
	 << "    (recent kernels are safe).  You can disable the write cache with\n"
	 << "    'hdparm -W 0 " << fn << "'"
	 << TEXT_NORMAL
	 << dendl;
    break;
  }

close_f:
  if (pclose(fp)) {
    int ret = -errno;
    derr << "_check_disk_write_cache: pclose failed: " << cpp_strerror(ret)
	 << dendl;
  }
done:
  ;
}

int FileJournal::_open_file(int64_t oldsize, blksize_t blksize,
			    bool create)
{
  int ret;
  int64_t conf_journal_sz(g_conf->osd_journal_size);
  conf_journal_sz <<= 20;

  if ((g_conf->osd_journal_size == 0) && (oldsize < ONE_MEG)) {
    derr << "I'm sorry, I don't know how large of a journal to create."
	 << "Please specify a block device to use as the journal OR "
	 << "set osd_journal_size in your ceph.conf" << dendl;
    return -EINVAL;
  }

  if (create && (oldsize < conf_journal_sz)) {
    uint64_t newsize(g_conf->osd_journal_size);
    newsize <<= 20;
    dout(10) << "_open extending to " << newsize << " bytes" << dendl;
    ret = ::ftruncate(fd, newsize);
    if (ret < 0) {
      int err = errno;
      derr << "FileJournal::_open_file : unable to extend journal to "
	   << newsize << " bytes: " << cpp_strerror(err) << dendl;
      return -err;
    }
#ifdef HAVE_POSIX_FALLOCATE
    ret = ::posix_fallocate(fd, 0, newsize);
    if (ret) {
      derr << "FileJournal::_open_file : unable to preallocation journal to "
	   << newsize << " bytes: " << cpp_strerror(ret) << dendl;
      return -ret;
    }
    max_size = newsize;
#elif defined(__APPLE__)
    fstore_t store;
    store.fst_flags = F_ALLOCATECONTIG;
    store.fst_posmode = F_PEOFPOSMODE;
    store.fst_offset = 0;
    store.fst_length = newsize;

    ret = ::fcntl(fd, F_PREALLOCATE, &store);
    if (ret == -1) {
      ret = -errno;
      derr << "FileJournal::_open_file : unable to preallocation journal to "
	   << newsize << " bytes: " << cpp_strerror(ret) << dendl;
      return ret;
    }
    max_size = newsize;
#else
# error "Journal pre-allocation not supported on platform."
#endif
  }
  else {
    max_size = oldsize;
  }
  block_size = MAX(blksize, (blksize_t)CEPH_PAGE_SIZE);

  if (create && g_conf->journal_zero_on_create) {
    derr << "FileJournal::_open_file : zeroing journal" << dendl;
    uint64_t write_size = 1 << 20;
    char *buf;
    ret = ::posix_memalign((void **)&buf, block_size, write_size);
    if (ret != 0) {
      return ret;
    }
    memset(static_cast<void*>(buf), 0, write_size);
    uint64_t i = 0;
    for (; (i + write_size) <= (unsigned)max_size; i += write_size) {
      ret = ::pwrite(fd, static_cast<void*>(buf), write_size, i);
      if (ret < 0) {
	free(buf);
	return -errno;
      }
    }
    if (i < (unsigned)max_size) {
      ret = ::pwrite(fd, static_cast<void*>(buf), max_size - i, i);
      if (ret < 0) {
	free(buf);
	return -errno;
      }
    }
    free(buf);
  }
      

  dout(10) << "_open journal is not a block device, NOT checking disk "
           << "write cache on '" << fn << "'" << dendl;

  return 0;
}

int FileJournal::check()
{
  int ret;

  ret = _open(false, false);
  if (ret)
    goto done;

  ret = read_header();
  if (ret < 0)
    goto done;

  if (header.fsid != fsid) {
    derr << "check: ondisk fsid " << header.fsid << " doesn't match expected " << fsid
	 << ", invalid (someone else's?) journal" << dendl;
    ret = -EINVAL;
    goto done;
  }

  dout(1) << "check: header looks ok" << dendl;
  ret = 0;

 done:
  VOID_TEMP_FAILURE_RETRY(::close(fd));
  fd = -1;
  return ret;
}


int FileJournal::create()
{
  void *buf = 0;
  int64_t needed_space;
  int ret;
  buffer::ptr bp;
  dout(2) << "create " << fn << " fsid " << fsid << dendl;

  ret = _open(true, true);
  if (ret)
    goto done;

  // write empty header
  header = header_t();
  header.flags = header_t::FLAG_CRC;  // enable crcs on any new journal.
  header.fsid = fsid;
  header.max_size = max_size;
  header.block_size = block_size;
  if (g_conf->journal_block_align || directio)
    header.alignment = block_size;
  else
    header.alignment = 16;  // at least stay word aligned on 64bit machines...

  header.start = get_top();
  header.start_seq = 0;

  print_header();

  // static zeroed buffer for alignment padding
  delete [] zero_buf;
  zero_buf = new char[header.alignment];
  memset(zero_buf, 0, header.alignment);

  bp = prepare_header();
  if (TEMP_FAILURE_RETRY(::pwrite(fd, bp.c_str(), bp.length(), 0)) < 0) {
    ret = errno;
    derr << "FileJournal::create : create write header error "
         << cpp_strerror(ret) << dendl;
    goto close_fd;
  }

  // zero first little bit, too.
  ret = posix_memalign(&buf, block_size, block_size);
  if (ret) {
    derr << "FileJournal::create: failed to allocate " << block_size
	 << " bytes of memory: " << cpp_strerror(ret) << dendl;
    goto close_fd;
  }
  memset(buf, 0, block_size);
  if (TEMP_FAILURE_RETRY(::pwrite(fd, buf, block_size, get_top())) < 0) {
    ret = errno;
    derr << "FileJournal::create: error zeroing first " << block_size
	 << " bytes " << cpp_strerror(ret) << dendl;
    goto free_buf;
  }

  needed_space = ((int64_t)g_conf->osd_max_write_size) << 20;
  needed_space += (2 * sizeof(entry_header_t)) + get_top();
  if (header.max_size - header.start < needed_space) {
    derr << "FileJournal::create: OSD journal is not large enough to hold "
	 << "osd_max_write_size bytes!" << dendl;
    ret = -ENOSPC;
    goto free_buf;
  }

  dout(2) << "create done" << dendl;
  ret = 0;

free_buf:
  free(buf);
  buf = 0;
close_fd:
  if (TEMP_FAILURE_RETRY(::close(fd)) < 0) {
    ret = errno;
    derr << "FileJournal::create: error closing fd: " << cpp_strerror(ret)
	 << dendl;
    goto done;
  }
done:
  fd = -1;
  return ret;
}

int FileJournal::peek_fsid(uuid_d& fsid)
{
  int r = _open(false, false);
  if (r)
    return r;
  r = read_header();
  if (r < 0)
    return r;
  fsid = header.fsid;
  return 0;
}

int FileJournal::open(uint64_t fs_op_seq)
{
  dout(2) << "open " << fn << " fsid " << fsid << " fs_op_seq " << fs_op_seq << dendl;

  uint64_t next_seq = fs_op_seq + 1;

  int err = _open(false);
  if (err)
    return err;

  // assume writeable, unless...
  read_pos = 0;
  write_pos = get_top();

  // read header?
  err = read_header();
  if (err < 0)
    return err;

  // static zeroed buffer for alignment padding
  delete [] zero_buf;
  zero_buf = new char[header.alignment];
  memset(zero_buf, 0, header.alignment);

  dout(10) << "open header.fsid = " << header.fsid 
    //<< " vs expected fsid = " << fsid 
	   << dendl;
  if (header.fsid != fsid) {
    derr << "FileJournal::open: ondisk fsid " << header.fsid << " doesn't match expected " << fsid
         << ", invalid (someone else's?) journal" << dendl;
    return -EINVAL;
  }
  if (header.max_size > max_size) {
    dout(2) << "open journal size " << header.max_size << " > current " << max_size << dendl;
    return -EINVAL;
  }
  if (header.block_size != block_size) {
    dout(2) << "open journal block size " << header.block_size << " != current " << block_size << dendl;
    return -EINVAL;
  }
  if (header.max_size % header.block_size) {
    dout(2) << "open journal max size " << header.max_size
	    << " not a multiple of block size " << header.block_size << dendl;
    return -EINVAL;
  }
  if (header.alignment != block_size && directio) {
    dout(0) << "open journal alignment " << header.alignment << " does not match block size " 
	    << block_size << " (required for direct_io journal mode)" << dendl;
    return -EINVAL;
  }
  if ((header.alignment % CEPH_PAGE_SIZE) && directio) {
    dout(0) << "open journal alignment " << header.alignment << " is not multiple of page size " << CEPH_PAGE_SIZE
	    << " (required for direct_io journal mode)" << dendl;
    return -EINVAL;
  }

  // looks like a valid header.
  write_pos = 0;  // not writeable yet

  journaled_seq = header.committed_up_to;

  // find next entry
  read_pos = header.start;
  uint64_t seq = header.start_seq;

  // last_committed_seq is 1 before the start of the journal or
  // 0 if the start is 0
  last_committed_seq = seq > 0 ? seq - 1 : seq;
  if (last_committed_seq < fs_op_seq) {
    dout(2) << "open advancing committed_seq " << last_committed_seq
	    << " to fs op_seq " << fs_op_seq << dendl;
    last_committed_seq = fs_op_seq;
  }

  while (1) {
    bufferlist bl;
    off64_t old_pos = read_pos;
    if (!read_entry(bl, seq)) {
      dout(10) << "open reached end of journal." << dendl;
      break;
    }
    if (seq > next_seq) {
      dout(10) << "open entry " << seq << " len " << bl.length() << " > next_seq " << next_seq
	       << ", ignoring journal contents"
	       << dendl;
      read_pos = -1;
      last_committed_seq = 0;
      seq = 0;
      return 0;
    }
    if (seq == next_seq) {
      dout(10) << "open reached seq " << seq << dendl;
      read_pos = old_pos;
      break;
    }
    seq++;  // next event should follow.
  }

  return 0;
}

void FileJournal::close()
{
  dout(1) << "close " << fn << dendl;

  // stop writer thread
  stop_writer();

  // close
  assert(writeq_empty());
  assert(!must_write_header);
  assert(fd >= 0);
  VOID_TEMP_FAILURE_RETRY(::close(fd));
  fd = -1;
}


int FileJournal::dump(ostream& out)
{
  int err = 0;

  dout(10) << "dump" << dendl;
  err = _open(false, false);
  if (err)
    return err;

  err = read_header();
  if (err < 0)
    return err;

  read_pos = header.start;

  JSONFormatter f(true);

  f.open_array_section("journal");
  uint64_t seq = 0;
  while (1) {
    bufferlist bl;
    uint64_t pos = read_pos;
    if (!read_entry(bl, seq)) {
      dout(3) << "journal_replay: end of journal, done." << dendl;
      break;
    }

    f.open_object_section("entry");
    f.dump_unsigned("offset", pos);
    f.dump_unsigned("seq", seq);
    f.open_array_section("transactions");
    bufferlist::iterator p = bl.begin();
    int trans_num = 0;
    while (!p.end()) {
      ObjectStore::Transaction *t = new ObjectStore::Transaction(p);
      f.open_object_section("transaction");
      f.dump_unsigned("trans_num", trans_num);
      t->dump(&f);
      f.close_section();
      delete t;
      trans_num++;
    }
    f.close_section();
    f.close_section();
    f.flush(cout);
  }

  f.close_section();
  dout(10) << "dump finish" << dendl;
  return 0;
}


void FileJournal::start_writer()
{
  write_stop = false;
  aio_stop = false;
  write_thread.create();
#ifdef HAVE_LIBAIO
  if (aio)
    write_finish_thread.create();
#endif
}

void FileJournal::stop_writer()
{
  {
    Mutex::Locker l(write_lock);
    Mutex::Locker p(writeq_lock);
    write_stop = true;
    writeq_cond.Signal();
  }
  write_thread.join();

#ifdef HAVE_LIBAIO
  // stop aio completeion thread *after* writer thread has stopped
  // and has submitted all of its io
  if (aio) {
    aio_lock.Lock();
    aio_stop = true;
    aio_cond.Signal();
    write_finish_cond.Signal();
    aio_lock.Unlock();
    write_finish_thread.join();
  }
#endif
}



void FileJournal::print_header()
{
  dout(10) << "header: block_size " << header.block_size
	   << " alignment " << header.alignment
	   << " max_size " << header.max_size
	   << dendl;
  dout(10) << "header: start " << header.start << dendl;
  dout(10) << " write_pos " << write_pos << dendl;
}

int FileJournal::read_header()
{
  dout(10) << "read_header" << dendl;
  bufferlist bl;

  buffer::ptr bp = buffer::create_page_aligned(block_size);
  bp.zero();
  int r = ::pread(fd, bp.c_str(), bp.length(), 0);

  if (r < 0) {
    int err = errno;
    dout(0) << "read_header got " << cpp_strerror(err) << dendl;
    return -err;
  }

  bl.push_back(bp);

  try {
    bufferlist::iterator p = bl.begin();
    ::decode(header, p);
  }
  catch (buffer::error& e) {
    derr << "read_header error decoding journal header" << dendl;
    return -EINVAL;
  }

  
  /*
   * Unfortunately we weren't initializing the flags field for new
   * journals!  Aie.  This is safe(ish) now that we have only one
   * flag.  Probably around when we add the next flag we need to
   * remove this or else this (eventually old) code will clobber newer
   * code's flags.
   */
  if (header.flags > 3) {
    derr << "read_header appears to have gibberish flags; assuming 0" << dendl;
    header.flags = 0;
  }

  print_header();

  return 0;
}

bufferptr FileJournal::prepare_header()
{
  bufferlist bl;
  {
    Mutex::Locker l(finisher_lock);
    header.committed_up_to = journaled_seq;
  }
  ::encode(header, bl);
  bufferptr bp = buffer::create_page_aligned(get_top());
  bp.zero();
  memcpy(bp.c_str(), bl.c_str(), bl.length());
  return bp;
}



int FileJournal::check_for_full(uint64_t seq, off64_t pos, off64_t size)
{
  // already full?
  if (full_state != FULL_NOTFULL)
    return -ENOSPC;

  // take 1 byte off so that we only get pos == header.start on EMPTY, never on FULL.
  off64_t room;
  if (pos >= header.start)
    room = (header.max_size - pos) + (header.start - get_top()) - 1;
  else
    room = header.start - pos - 1;
  dout(10) << "room " << room << " max_size " << max_size << " pos " << pos << " header.start " << header.start
	   << " top " << get_top() << dendl;

  if (do_sync_cond) {
    if (room < (header.max_size >> 1) &&
	room + size > (header.max_size >> 1)) {
      dout(10) << " passing half full mark, triggering commit" << dendl;
      do_sync_cond->SloppySignal();  // initiate a real commit so we can trim
    }
  }

  if (room >= size) {
    dout(10) << "check_for_full at " << pos << " : " << size << " < " << room << dendl;
    if (pos + size > header.max_size)
      must_write_header = true;
    return 0;
  }

  // full
  dout(1) << "check_for_full at " << pos << " : JOURNAL FULL "
	  << pos << " >= " << room
	  << " (max_size " << header.max_size << " start " << header.start << ")"
	  << dendl;

  off64_t max = header.max_size - get_top();
  if (size > max)
    dout(0) << "JOURNAL TOO SMALL: continuing, but slow: item " << size << " > journal " << max << " (usable)" << dendl;

  return -ENOSPC;
}

int FileJournal::prepare_multi_write(bufferlist& bl, uint64_t& orig_ops, uint64_t& orig_bytes)
{
  // gather queued writes
  off64_t queue_pos = write_pos;

  int eleft = g_conf->journal_max_write_entries;
  unsigned bmax = g_conf->journal_max_write_bytes;

  if (full_state != FULL_NOTFULL)
    return -ENOSPC;
  
  while (!writeq_empty()) {
    int r = prepare_single_write(bl, queue_pos, orig_ops, orig_bytes);
    if (r == -ENOSPC) {
      if (orig_ops)
	break;         // commit what we have

      if (logger)
	logger->inc(l_os_j_full);

      if (wait_on_full) {
	dout(20) << "prepare_multi_write full on first entry, need to wait" << dendl;
      } else {
	dout(20) << "prepare_multi_write full on first entry, restarting journal" << dendl;

	// throw out what we have so far
	full_state = FULL_FULL;
	while (!writeq_empty()) {
	  put_throttle(1, peek_write().bl.length());
	  pop_write();
	}  
	print_header();
      }

      return -ENOSPC;  // hrm, full on first op
    }

    if (eleft) {
      if (--eleft == 0) {
	dout(20) << "prepare_multi_write hit max events per write " << g_conf->journal_max_write_entries << dendl;
	break;
      }
    }
    if (bmax) {
      if (bl.length() >= bmax) {
	dout(20) << "prepare_multi_write hit max write size " << g_conf->journal_max_write_bytes << dendl;
	break;
      }
    }
  }

  dout(20) << "prepare_multi_write queue_pos now " << queue_pos << dendl;
  //assert(write_pos + bl.length() == queue_pos);
  return 0;
}

/*
void FileJournal::queue_write_fin(uint64_t seq, Context *fin)
{
  writing_seq.push_back(seq);
  if (!waiting_for_notfull.empty()) {
    // make sure previously unjournaled stuff waiting for UNFULL triggers
    // _before_ newly journaled stuff does
    dout(10) << "queue_write_fin will defer seq " << seq << " callback " << fin
	     << " until after UNFULL" << dendl;
    C_Gather *g = new C_Gather(writeq.front().fin);
    writing_fin.push_back(g->new_sub());
    waiting_for_notfull.push_back(g->new_sub());
  } else {
    writing_fin.push_back(writeq.front().fin);
    dout(20) << "queue_write_fin seq " << seq << " callback " << fin << dendl;
  }
}
*/

void FileJournal::queue_completions_thru(uint64_t seq)
{
  assert(finisher_lock.is_locked());
  utime_t now = ceph_clock_now(g_ceph_context);
  while (!completions_empty()) {
    completion_item next = completion_peek_front();
    if (next.seq > seq)
      break;
    completion_pop_front();
    utime_t lat = now;
    lat -= next.start;
    dout(10) << "queue_completions_thru seq " << seq
	     << " queueing seq " << next.seq
	     << " " << next.finish
	     << " lat " << lat << dendl;
    if (logger) {
      logger->tinc(l_os_j_lat, lat);
    }
    if (next.finish)
      finisher->queue(next.finish);
    if (next.tracked_op)
      next.tracked_op->mark_event("journaled_completion_queued");
  }
  finisher_cond.Signal();
}

int FileJournal::prepare_single_write(bufferlist& bl, off64_t& queue_pos, uint64_t& orig_ops, uint64_t& orig_bytes)
{
  // grab next item
  write_item &next_write = peek_write();
  uint64_t seq = next_write.seq;
  bufferlist &ebl = next_write.bl;
  unsigned head_size = sizeof(entry_header_t);
  off64_t base_size = 2*head_size + ebl.length();

  int alignment = next_write.alignment; // we want to start ebl with this alignment
  unsigned pre_pad = 0;
  if (alignment >= 0)
    pre_pad = ((unsigned int)alignment - (unsigned int)head_size) & ~CEPH_PAGE_MASK;
  off64_t size = ROUND_UP_TO(base_size + pre_pad, header.alignment);
  unsigned post_pad = size - base_size - pre_pad;

  int r = check_for_full(seq, queue_pos, size);
  if (r < 0)
    return r;   // ENOSPC or EAGAIN

  orig_bytes += ebl.length();
  orig_ops++;

  // add to write buffer
  dout(15) << "prepare_single_write " << orig_ops << " will write " << queue_pos << " : seq " << seq
	   << " len " << ebl.length() << " -> " << size
	   << " (head " << head_size << " pre_pad " << pre_pad
	   << " ebl " << ebl.length() << " post_pad " << post_pad << " tail " << head_size << ")"
	   << " (ebl alignment " << alignment << ")"
	   << dendl;
    
  // add it this entry
  entry_header_t h;
  memset(&h, 0, sizeof(h));
  h.seq = seq;
  h.pre_pad = pre_pad;
  h.len = ebl.length();
  h.post_pad = post_pad;
  h.make_magic(queue_pos, header.get_fsid64());
  h.crc32c = ebl.crc32c(0);

  bl.append((const char*)&h, sizeof(h));
  if (pre_pad) {
    bufferptr bp = buffer::create_static(pre_pad, zero_buf);
    bl.push_back(bp);
  }
  bl.claim_append(ebl, buffer::list::CLAIM_ALLOW_NONSHAREABLE); // potential zero-copy

  if (h.post_pad) {
    bufferptr bp = buffer::create_static(post_pad, zero_buf);
    bl.push_back(bp);
  }
  bl.append((const char*)&h, sizeof(h));

  if (next_write.tracked_op)
    next_write.tracked_op->mark_event("write_thread_in_journal_buffer");

  // pop from writeq
  pop_write();
  journalq.push_back(pair<uint64_t,off64_t>(seq, queue_pos));
  writing_seq = seq;

  queue_pos += size;
  if (queue_pos >= header.max_size)
    queue_pos = queue_pos + get_top() - header.max_size;

  return 0;
}

void FileJournal::align_bl(off64_t pos, bufferlist& bl)
{
  // make sure list segments are page aligned
  if (directio && (!bl.is_page_aligned() ||
		   !bl.is_n_page_sized())) {
    bl.rebuild_page_aligned();
    dout(10) << __func__ << " total memcopy: " << bl.get_memcopy_count() << dendl;
    if ((bl.length() & ~CEPH_PAGE_MASK) != 0 ||
	(pos & ~CEPH_PAGE_MASK) != 0)
      dout(0) << "rebuild_page_aligned failed, " << bl << dendl;
    assert((bl.length() & ~CEPH_PAGE_MASK) == 0);
    assert((pos & ~CEPH_PAGE_MASK) == 0);
  }
}

int FileJournal::write_bl(off64_t& pos, bufferlist& bl)
{
  int ret;

  off64_t spos = ::lseek64(fd, pos, SEEK_SET);
  if (spos < 0) {
    ret = -errno;
    derr << "FileJournal::write_bl : lseek64 failed " << cpp_strerror(ret) << dendl;
    return ret;
  }
  ret = bl.write_fd(fd);
  if (ret) {
    derr << "FileJournal::write_bl : write_fd failed: " << cpp_strerror(ret) << dendl;
    return ret;
  }
  pos += bl.length();
  if (pos == header.max_size)
    pos = get_top();
  return 0;
}

void FileJournal::do_write(bufferlist& bl)
{
  // nothing to do?
  if (bl.length() == 0 && !must_write_header) 
    return;

  buffer::ptr hbp;
  if (g_conf->journal_write_header_frequency &&
      (((++journaled_since_start) %
	g_conf->journal_write_header_frequency) == 0)) {
    must_write_header = true;
  }

  if (must_write_header) {
    must_write_header = false;
    hbp = prepare_header();
  }

  dout(15) << "do_write writing " << write_pos << "~" << bl.length() 
	   << (hbp.length() ? " + header":"")
	   << dendl;
  
  utime_t from = ceph_clock_now(g_ceph_context);

  // entry
  off64_t pos = write_pos;

  // Adjust write_pos
  align_bl(pos, bl);
  write_pos += bl.length();
  if (write_pos >= header.max_size)
    write_pos = write_pos - header.max_size + get_top();

  write_lock.Unlock();

  // split?
  off64_t split = 0;
  if (pos + bl.length() > header.max_size) {
    bufferlist first, second;
    split = header.max_size - pos;
    first.substr_of(bl, 0, split);
    second.substr_of(bl, split, bl.length() - split);
    assert(first.length() + second.length() == bl.length());
    dout(10) << "do_write wrapping, first bit at " << pos << " len " << first.length()
	     << " second bit len " << second.length() << " (orig len " << bl.length() << ")" << dendl;

    if (write_bl(pos, first)) {
      derr << "FileJournal::do_write: write_bl(pos=" << pos
	   << ") failed" << dendl;
      ceph_abort();
    }
    assert(pos == get_top());
    if (hbp.length()) {
      // be sneaky: include the header in the second fragment
      second.push_front(hbp);
      pos = 0;          // we included the header
    }
    if (write_bl(pos, second)) {
      derr << "FileJournal::do_write: write_bl(pos=" << pos
	   << ") failed" << dendl;
      ceph_abort();
    }
  } else {
    // header too?
    if (hbp.length()) {
      if (TEMP_FAILURE_RETRY(::pwrite(fd, hbp.c_str(), hbp.length(), 0)) < 0) {
	int err = errno;
	derr << "FileJournal::do_write: pwrite(fd=" << fd
	     << ", hbp.length=" << hbp.length() << ") failed :"
	     << cpp_strerror(err) << dendl;
	ceph_abort();
      }
    }

    if (write_bl(pos, bl)) {
      derr << "FileJournal::do_write: write_bl(pos=" << pos
	   << ") failed" << dendl;
      ceph_abort();
    }
  }

  if (!directio) {
    dout(20) << "do_write fsync" << dendl;

    /*
     * We'd really love to have a fsync_range or fdatasync_range and do a:
     *
     *  if (split) {
     *    ::fsync_range(fd, header.max_size - split, split)l
     *    ::fsync_range(fd, get_top(), bl.length() - split);
     *  else
     *    ::fsync_range(fd, write_pos, bl.length())
     *
     * NetBSD and AIX apparently have it, and adding it to Linux wouldn't be
     * too hard given all the underlying infrastructure already exist.
     *
     * NOTE: using sync_file_range here would not be safe as it does not
     * flush disk caches or commits any sort of metadata.
     */
    int ret = 0;
#if defined(DARWIN) || defined(__FreeBSD__)
    ret = ::fsync(fd);
#else
    ret = ::fdatasync(fd);
#endif
    if (ret < 0) {
      derr << __func__ << " fsync/fdatasync failed: " << cpp_strerror(errno) << dendl;
      ceph_abort();
    }
#ifdef HAVE_POSIX_FADVISE
    if (g_conf->filestore_fadvise)
      posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
  }

  utime_t lat = ceph_clock_now(g_ceph_context) - from;    
  dout(20) << "do_write latency " << lat << dendl;

  write_lock.Lock();    

  assert(write_pos == pos);
  assert(write_pos % header.alignment == 0);

  {
    Mutex::Locker locker(finisher_lock);
    journaled_seq = writing_seq;

    // kick finisher?
    //  only if we haven't filled up recently!
    if (full_state != FULL_NOTFULL) {
      dout(10) << "do_write NOT queueing finisher seq " << journaled_seq
	       << ", full_commit_seq|full_restart_seq" << dendl;
    } else {
      if (plug_journal_completions) {
	dout(20) << "do_write NOT queueing finishers through seq " << journaled_seq
		 << " due to completion plug" << dendl;
      } else {
	dout(20) << "do_write queueing finishers through seq " << journaled_seq << dendl;
	queue_completions_thru(journaled_seq);
      }
    }
  }
}

void FileJournal::flush()
{
  dout(10) << "waiting for completions to empty" << dendl;
  {
    Mutex::Locker l(finisher_lock);
    while (!completions_empty())
      finisher_cond.Wait(finisher_lock);
  }
  dout(10) << "flush waiting for finisher" << dendl;
  finisher->wait_for_empty();
  dout(10) << "flush done" << dendl;
}


void FileJournal::write_thread_entry()
{
  dout(10) << "write_thread_entry start" << dendl;
  while (1) {
    {
      Mutex::Locker locker(writeq_lock);
      if (writeq.empty() && !must_write_header) {
	if (write_stop)
	  break;
	dout(20) << "write_thread_entry going to sleep" << dendl;
	writeq_cond.Wait(writeq_lock);
	dout(20) << "write_thread_entry woke up" << dendl;
	continue;
      }
    }
    
#ifdef HAVE_LIBAIO
    if (aio) {
      Mutex::Locker locker(aio_lock);
      // should we back off to limit aios in flight?  try to do this
      // adaptively so that we submit larger aios once we have lots of
      // them in flight.
      //
      // NOTE: our condition here is based on aio_num (protected by
      // aio_lock) and throttle_bytes (part of the write queue).  when
      // we sleep, we *only* wait for aio_num to change, and do not
      // wake when more data is queued.  this is not strictly correct,
      // but should be fine given that we will have plenty of aios in
      // flight if we hit this limit to ensure we keep the device
      // saturated.
      while (aio_num > 0) {
	int exp = MIN(aio_num * 2, 24);
	long unsigned min_new = 1ull << exp;
	long unsigned cur = throttle_bytes.get_current();
	dout(20) << "write_thread_entry aio throttle: aio num " << aio_num << " bytes " << aio_bytes
		 << " ... exp " << exp << " min_new " << min_new
		 << " ... pending " << cur << dendl;
	if (cur >= min_new)
	  break;
	dout(20) << "write_thread_entry deferring until more aios complete: "
		 << aio_num << " aios with " << aio_bytes << " bytes needs " << min_new
		 << " bytes to start a new aio (currently " << cur << " pending)" << dendl;
	aio_cond.Wait(aio_lock);
	dout(20) << "write_thread_entry woke up" << dendl;
      }
    }
#endif

    Mutex::Locker locker(write_lock);
    uint64_t orig_ops = 0;
    uint64_t orig_bytes = 0;

    bufferlist bl;
    int r = prepare_multi_write(bl, orig_ops, orig_bytes);
    if (r == -ENOSPC) {
      dout(20) << "write_thread_entry full, going to sleep (waiting for commit)" << dendl;
      commit_cond.Wait(write_lock);
      dout(20) << "write_thread_entry woke up" << dendl;
      continue;
    }
    assert(r == 0);

    if (logger) {
      logger->inc(l_os_j_wr);
      logger->inc(l_os_j_wr_bytes, bl.length());
    }

#ifdef HAVE_LIBAIO
    if (aio)
      do_aio_write(bl);
    else
      do_write(bl);
#else
    do_write(bl);
#endif
    put_throttle(orig_ops, orig_bytes);
  }

  dout(10) << "write_thread_entry finish" << dendl;
}

#ifdef HAVE_LIBAIO
void FileJournal::do_aio_write(bufferlist& bl)
{

  if (g_conf->journal_write_header_frequency &&
      (((++journaled_since_start) %
	g_conf->journal_write_header_frequency) == 0)) {
    must_write_header = true;
  }

  // nothing to do?
  if (bl.length() == 0 && !must_write_header) 
    return;

  buffer::ptr hbp;
  if (must_write_header) {
    must_write_header = false;
    hbp = prepare_header();
  }

  // entry
  off64_t pos = write_pos;

  dout(15) << "do_aio_write writing " << pos << "~" << bl.length() 
	   << (hbp.length() ? " + header":"")
	   << dendl;
  
  // split?
  off64_t split = 0;
  if (pos + bl.length() > header.max_size) {
    bufferlist first, second;
    split = header.max_size - pos;
    first.substr_of(bl, 0, split);
    second.substr_of(bl, split, bl.length() - split);
    assert(first.length() + second.length() == bl.length());
    dout(10) << "do_aio_write wrapping, first bit at " << pos << "~" << first.length() << dendl;

    if (write_aio_bl(pos, first, 0)) {
      derr << "FileJournal::do_aio_write: write_aio_bl(pos=" << pos
	   << ") failed" << dendl;
      ceph_abort();
    }
    assert(pos == header.max_size);
    if (hbp.length()) {
      // be sneaky: include the header in the second fragment
      second.push_front(hbp);
      pos = 0;          // we included the header
    } else
      pos = get_top();  // no header, start after that
    if (write_aio_bl(pos, second, writing_seq)) {
      derr << "FileJournal::do_aio_write: write_aio_bl(pos=" << pos
	   << ") failed" << dendl;
      ceph_abort();
    }
  } else {
    // header too?
    if (hbp.length()) {
      bufferlist hbl;
      hbl.push_back(hbp);
      loff_t pos = 0;
      if (write_aio_bl(pos, hbl, 0)) {
	derr << "FileJournal::do_aio_write: write_aio_bl(header) failed" << dendl;
	ceph_abort();
      }
    }

    if (write_aio_bl(pos, bl, writing_seq)) {
      derr << "FileJournal::do_aio_write: write_aio_bl(pos=" << pos
	   << ") failed" << dendl;
      ceph_abort();
    }
  }

  write_pos = pos;
  if (write_pos == header.max_size)
    write_pos = get_top();
  assert(write_pos % header.alignment == 0);
}

/**
 * write a buffer using aio
 *
 * @param seq seq to trigger when this aio completes.  if 0, do not update any state
 * on completion.
 */
int FileJournal::write_aio_bl(off64_t& pos, bufferlist& bl, uint64_t seq)
{
  Mutex::Locker locker(aio_lock);
  align_bl(pos, bl);

  dout(20) << "write_aio_bl " << pos << "~" << bl.length() << " seq " << seq << dendl;
  
  while (bl.length() > 0) {
    int max = MIN(bl.buffers().size(), IOV_MAX-1);
    iovec *iov = new iovec[max];
    int n = 0;
    unsigned len = 0;
    for (std::list<buffer::ptr>::const_iterator p = bl.buffers().begin();
	 n < max;
	 ++p, ++n) {
      assert(p != bl.buffers().end());
      iov[n].iov_base = (void *)p->c_str();
      iov[n].iov_len = p->length();
      len += p->length();
    }

    bufferlist tbl;
    bl.splice(0, len, &tbl);  // move bytes from bl -> tbl

    aio_queue.push_back(aio_info(tbl, pos, bl.length() > 0 ? 0 : seq));
    aio_info& aio = aio_queue.back();
    aio.iov = iov;

    io_prep_pwritev(&aio.iocb, fd, aio.iov, n, pos);

    dout(20) << "write_aio_bl .. " << aio.off << "~" << aio.len
	     << " in " << n << dendl;

    aio_num++;
    aio_bytes += aio.len;

    iocb *piocb = &aio.iocb;
    int attempts = 10;
    do {
      int r = io_submit(aio_ctx, 1, &piocb);
      if (r < 0) {
	derr << "io_submit to " << aio.off << "~" << aio.len
	     << " got " << cpp_strerror(r) << dendl;
	if (r == -EAGAIN && attempts-- > 0) {
	  usleep(500);
	  continue;
	}
	assert(0 == "io_submit got unexpected error");
      }
    } while (false);
    pos += aio.len;
  }
  write_finish_cond.Signal();
  return 0;
}
#endif

void FileJournal::write_finish_thread_entry()
{
#ifdef HAVE_LIBAIO
  dout(10) << "write_finish_thread_entry enter" << dendl;
  while (true) {
    {
      Mutex::Locker locker(aio_lock);
      if (aio_queue.empty()) {
	if (aio_stop)
	  break;
	dout(20) << "write_finish_thread_entry sleeping" << dendl;
	write_finish_cond.Wait(aio_lock);
	continue;
      }
    }
    
    dout(20) << "write_finish_thread_entry waiting for aio(s)" << dendl;
    io_event event[16];
    int r = io_getevents(aio_ctx, 1, 16, event, NULL);
    if (r < 0) {
      if (r == -EINTR) {
	dout(0) << "io_getevents got " << cpp_strerror(r) << dendl;
	continue;
      }
      derr << "io_getevents got " << cpp_strerror(r) << dendl;
      assert(0 == "got unexpected error from io_getevents");
    }
    
    {
      Mutex::Locker locker(aio_lock);
      for (int i=0; i<r; i++) {
	aio_info *ai = (aio_info *)event[i].obj;
	if (event[i].res != ai->len) {
	  derr << "aio to " << ai->off << "~" << ai->len
	       << " wrote " << event[i].res << dendl;
	  assert(0 == "unexpected aio error");
	}
	dout(10) << "write_finish_thread_entry aio " << ai->off
		 << "~" << ai->len << " done" << dendl;
	ai->done = true;
      }
      check_aio_completion();
    }
  }
  dout(10) << "write_finish_thread_entry exit" << dendl;
#endif
}

#ifdef HAVE_LIBAIO
/**
 * check aio_wait for completed aio, and update state appropriately.
 */
void FileJournal::check_aio_completion()
{
  assert(aio_lock.is_locked());
  dout(20) << "check_aio_completion" << dendl;

  bool completed_something = false, signal = false;
  uint64_t new_journaled_seq = 0;

  list<aio_info>::iterator p = aio_queue.begin();
  while (p != aio_queue.end() && p->done) {
    dout(20) << "check_aio_completion completed seq " << p->seq << " "
	     << p->off << "~" << p->len << dendl;
    if (p->seq) {
      new_journaled_seq = p->seq;
      completed_something = true;
    }
    aio_num--;
    aio_bytes -= p->len;
    aio_queue.erase(p++);
    signal = true;
  }

  if (completed_something) {
    // kick finisher?  
    //  only if we haven't filled up recently!
    Mutex::Locker locker(finisher_lock);
    journaled_seq = new_journaled_seq;
    if (full_state != FULL_NOTFULL) {
      dout(10) << "check_aio_completion NOT queueing finisher seq " << journaled_seq
	       << ", full_commit_seq|full_restart_seq" << dendl;
    } else {
      if (plug_journal_completions) {
	dout(20) << "check_aio_completion NOT queueing finishers through seq " << journaled_seq
		 << " due to completion plug" << dendl;
      } else {
	dout(20) << "check_aio_completion queueing finishers through seq " << journaled_seq << dendl;
	queue_completions_thru(journaled_seq);
      }
    }
  }
  if (signal) {
    // maybe write queue was waiting for aio count to drop?
    aio_cond.Signal();
  }
}
#endif

void FileJournal::submit_entry(uint64_t seq, bufferlist& e, int alignment,
			       Context *oncommit, TrackedOpRef osd_op)
{
  // dump on queue
  dout(5) << "submit_entry seq " << seq
	  << " len " << e.length()
	  << " (" << oncommit << ")" << dendl;
  assert(e.length() > 0);

  throttle_ops.take(1);
  throttle_bytes.take(e.length());
  if (osd_op)
    osd_op->mark_event("commit_queued_for_journal_write");
  if (logger) {
    logger->set(l_os_jq_max_ops, throttle_ops.get_max());
    logger->set(l_os_jq_max_bytes, throttle_bytes.get_max());
    logger->set(l_os_jq_ops, throttle_ops.get_current());
    logger->set(l_os_jq_bytes, throttle_bytes.get_current());
  }

  {
    Mutex::Locker l1(writeq_lock);  // ** lock **
    Mutex::Locker l2(completions_lock);  // ** lock **
    completions.push_back(
      completion_item(
	seq, oncommit, ceph_clock_now(g_ceph_context), osd_op));
    if (writeq.empty())
      writeq_cond.Signal();
    writeq.push_back(write_item(seq, e, alignment, osd_op));
  }
}

bool FileJournal::writeq_empty()
{
  Mutex::Locker locker(writeq_lock);
  return writeq.empty();
}

FileJournal::write_item &FileJournal::peek_write()
{
  assert(write_lock.is_locked());
  Mutex::Locker locker(writeq_lock);
  return writeq.front();
}

void FileJournal::pop_write()
{
  assert(write_lock.is_locked());
  Mutex::Locker locker(writeq_lock);
  writeq.pop_front();
}

void FileJournal::commit_start(uint64_t seq)
{
  dout(10) << "commit_start" << dendl;

  // was full?
  switch (full_state) {
  case FULL_NOTFULL:
    break; // all good

  case FULL_FULL:
    if (seq >= journaled_seq) {
      dout(1) << " FULL_FULL -> FULL_WAIT.  commit_start on seq "
	      << seq << " > journaled_seq " << journaled_seq
	      << ", moving to FULL_WAIT."
	      << dendl;
      full_state = FULL_WAIT;
    } else {
      dout(1) << "FULL_FULL commit_start on seq "
	      << seq << " < journaled_seq " << journaled_seq
	      << ", remaining in FULL_FULL"
	      << dendl;
    }
    break;

  case FULL_WAIT:
    dout(1) << " FULL_WAIT -> FULL_NOTFULL.  journal now active, setting completion plug." << dendl;
    full_state = FULL_NOTFULL;
    plug_journal_completions = true;
    break;
  }
}

/*
 *send discard command to joural block deivce
 */
void FileJournal::do_discard(int64_t offset, int64_t end)
{
  dout(10) << __func__ << "trim(" << offset << ", " << end << dendl;

  offset = ROUND_UP_TO(offset, block_size);
  if (offset >= end)
    return;
  end = ROUND_UP_TO(end - block_size, block_size);
  assert(end >= offset);
  if (offset < end)
    if (block_device_discard(fd, offset, end - offset) < 0)
	dout(1) << __func__ << "ioctl(BLKDISCARD) error:" << cpp_strerror(errno) << dendl;
}

void FileJournal::committed_thru(uint64_t seq)
{
  Mutex::Locker locker(write_lock);

  if (seq < last_committed_seq) {
    dout(5) << "committed_thru " << seq << " < last_committed_seq " << last_committed_seq << dendl;
    assert(seq >= last_committed_seq);
    return;
  }
  if (seq == last_committed_seq) {
    dout(5) << "committed_thru " << seq << " == last_committed_seq " << last_committed_seq << dendl;
    return;
  }

  dout(5) << "committed_thru " << seq << " (last_committed_seq " << last_committed_seq << ")" << dendl;
  last_committed_seq = seq;

  // completions!
  {
    Mutex::Locker locker(finisher_lock);
    queue_completions_thru(seq);
    if (plug_journal_completions && seq >= header.start_seq) {
      dout(10) << " removing completion plug, queuing completions thru journaled_seq " << journaled_seq << dendl;
      plug_journal_completions = false;
      queue_completions_thru(journaled_seq);
    }
  }

  // adjust start pointer
  while (!journalq.empty() && journalq.front().first <= seq) {
    journalq.pop_front();
  }

  int64_t old_start = header.start;
  if (!journalq.empty()) {
    header.start = journalq.front().second;
    header.start_seq = journalq.front().first;
  } else {
    header.start = write_pos;
    header.start_seq = seq + 1;
  }

  if (discard) {
    dout(10) << __func__  << " will trim (" << old_start << ", " << header.start << ")" << dendl;
    if (old_start < header.start)
      do_discard(old_start, header.start - 1);
    else {
      do_discard(old_start, header.max_size - 1);
      do_discard(get_top(), header.start - 1);
    }
  }

  must_write_header = true;
  print_header();

  // committed but unjournaled items
  while (!writeq_empty() && peek_write().seq <= seq) {
    dout(15) << " dropping committed but unwritten seq " << peek_write().seq 
	     << " len " << peek_write().bl.length()
	     << dendl;
    put_throttle(1, peek_write().bl.length());
    pop_write();
  }
  
  commit_cond.Signal();

  dout(10) << "committed_thru done" << dendl;
}


void FileJournal::put_throttle(uint64_t ops, uint64_t bytes)
{
  uint64_t new_ops = throttle_ops.put(ops);
  uint64_t new_bytes = throttle_bytes.put(bytes);
  dout(5) << "put_throttle finished " << ops << " ops and " 
	   << bytes << " bytes, now "
	   << new_ops << " ops and " << new_bytes << " bytes"
	   << dendl;

  if (logger) {
    logger->inc(l_os_j_ops, ops);
    logger->inc(l_os_j_bytes, bytes);
    logger->set(l_os_jq_ops, new_ops);
    logger->set(l_os_jq_bytes, new_bytes);
    logger->set(l_os_jq_max_ops, throttle_ops.get_max());
    logger->set(l_os_jq_max_bytes, throttle_bytes.get_max());
  }
}

int FileJournal::make_writeable()
{
  dout(10) << __func__ << dendl;
  int r = _open(true);
  if (r < 0)
    return r;

  if (read_pos > 0)
    write_pos = read_pos;
  else
    write_pos = get_top();
  read_pos = 0;

  must_write_header = true;
  start_writer();
  return 0;
}

void FileJournal::wrap_read_bl(
  off64_t pos,
  int64_t olen,
  bufferlist* bl,
  off64_t *out_pos
  )
{
  while (olen > 0) {
    while (pos >= header.max_size)
      pos = pos + get_top() - header.max_size;

    int64_t len;
    if (pos + olen > header.max_size)
      len = header.max_size - pos;        // partial
    else
      len = olen;                         // rest
    
    int64_t actual = ::lseek64(fd, pos, SEEK_SET);
    assert(actual == pos);
    
    bufferptr bp = buffer::create(len);
    int r = safe_read_exact(fd, bp.c_str(), len);
    if (r) {
      derr << "FileJournal::wrap_read_bl: safe_read_exact " << pos << "~" << len << " returned "
	   << r << dendl;
      ceph_abort();
    }
    bl->push_back(bp);
    pos += len;
    olen -= len;
  }
  if (pos >= header.max_size)
    pos = pos + get_top() - header.max_size;
  if (out_pos)
    *out_pos = pos;
}

bool FileJournal::read_entry(
  bufferlist &bl,
  uint64_t &next_seq,
  bool *corrupt)
{
  if (corrupt)
    *corrupt = false;
  uint64_t seq = next_seq;

  if (!read_pos) {
    dout(2) << "read_entry -- not readable" << dendl;
    return false;
  }

  off64_t pos = read_pos;
  off64_t next_pos = pos;
  stringstream ss;
  read_entry_result result = do_read_entry(
    pos,
    &next_pos,
    &bl,
    &seq,
    &ss);
  if (result == SUCCESS) {
    if (next_seq > seq) {
      return false;
    } else {
      read_pos = next_pos;
      next_seq = seq;
      if (seq > journaled_seq)
        journaled_seq = seq;
      return true;
    }
  }

  stringstream errss;
  if (seq < header.committed_up_to) {
    derr << "Unable to read past sequence " << seq
	 << " but header indicates the journal has committed up through "
	 << header.committed_up_to << ", journal is corrupt" << dendl;
    if (g_conf->journal_ignore_corruption) {
      if (corrupt)
	*corrupt = true;
      return false;
    } else {
      assert(0);
    }
  }

  dout(25) << errss.str() << dendl;
  dout(2) << "No further valid entries found, journal is most likely valid"
	  << dendl;
  return false;
}

FileJournal::read_entry_result FileJournal::do_read_entry(
  off64_t pos,
  off64_t *next_pos,
  bufferlist *bl,
  uint64_t *seq,
  ostream *ss,
  entry_header_t *_h)
{
  bufferlist _bl;
  if (!bl)
    bl = &_bl;

  // header
  entry_header_t *h;
  bufferlist hbl;
  off64_t _next_pos;
  wrap_read_bl(pos, sizeof(*h), &hbl, &_next_pos);
  h = reinterpret_cast<entry_header_t *>(hbl.c_str());

  if (!h->check_magic(pos, header.get_fsid64())) {
    dout(25) << "read_entry " << pos
	     << " : bad header magic, end of journal" << dendl;
    if (ss)
      *ss << "bad header magic";
    if (next_pos)
      *next_pos = pos + (4<<10); // check 4k ahead
    return MAYBE_CORRUPT;
  }
  pos = _next_pos;

  // pad + body + pad
  if (h->pre_pad)
    pos += h->pre_pad;

  bl->clear();
  wrap_read_bl(pos, h->len, bl, &pos);

  if (h->post_pad)
    pos += h->post_pad;

  // footer
  entry_header_t *f;
  bufferlist fbl;
  wrap_read_bl(pos, sizeof(*f), &fbl, &pos);
  f = reinterpret_cast<entry_header_t *>(fbl.c_str());
  if (memcmp(f, h, sizeof(*f))) {
    if (ss)
      *ss << "bad footer magic, partial entry";
    if (next_pos)
      *next_pos = pos;
    return MAYBE_CORRUPT;
  }

  if ((header.flags & header_t::FLAG_CRC) ||   // if explicitly enabled (new journal)
      h->crc32c != 0) {                        // newer entry in old journal
    uint32_t actual_crc = bl->crc32c(0);
    if (actual_crc != h->crc32c) {
      if (ss)
	*ss << "header crc (" << h->crc32c
	    << ") doesn't match body crc (" << actual_crc << ")";
      if (next_pos)
	*next_pos = pos;
      return MAYBE_CORRUPT;
    }
  }

  // yay!
  dout(2) << "read_entry " << pos << " : seq " << h->seq
	  << " " << h->len << " bytes"
	  << dendl;

  // ok!
  if (seq)
    *seq = h->seq;

  // works around an apparent GCC 4.8(?) compiler bug about unaligned
  // bind by reference to (packed) h->seq
  journalq.push_back(
    pair<uint64_t,off64_t>(static_cast<uint64_t>(h->seq),
			   static_cast<off64_t>(pos)));

  if (next_pos)
    *next_pos = pos;

  if (_h)
    *_h = *h;

  assert(pos % header.alignment == 0);
  return SUCCESS;
}

void FileJournal::throttle()
{
  if (throttle_ops.wait(g_conf->journal_queue_max_ops))
    dout(2) << "throttle: waited for ops" << dendl;
  if (throttle_bytes.wait(g_conf->journal_queue_max_bytes))
    dout(2) << "throttle: waited for bytes" << dendl;
}

void FileJournal::get_header(
  uint64_t wanted_seq,
  off64_t *_pos,
  entry_header_t *h)
{
  off64_t pos = header.start;
  off64_t next_pos = pos;
  bufferlist bl;
  uint64_t seq = 0;
  while (1) {
    bl.clear();
    pos = next_pos;
    read_entry_result result = do_read_entry(
      pos,
      &next_pos,
      &bl,
      &seq,
      0,
      h);
    if (result == FAILURE || result == MAYBE_CORRUPT)
      assert(0);
    if (seq == wanted_seq) {
      if (_pos)
	*_pos = pos;
      return;
    }
  }
  assert(0); // not reachable
}

void FileJournal::corrupt(
  int wfd,
  off64_t corrupt_at)
{
  if (corrupt_at >= header.max_size)
    corrupt_at = corrupt_at + get_top() - header.max_size;

  int64_t actual = ::lseek64(fd, corrupt_at, SEEK_SET);
  assert(actual == corrupt_at);

  char buf[10];
  int r = safe_read_exact(fd, buf, 1);
  assert(r == 0);

  actual = ::lseek64(wfd, corrupt_at, SEEK_SET);
  assert(actual == corrupt_at);

  buf[0]++;
  r = safe_write(wfd, buf, 1);
  assert(r == 0);
}

void FileJournal::corrupt_payload(
  int wfd,
  uint64_t seq)
{
  off64_t pos = 0;
  entry_header_t h;
  get_header(seq, &pos, &h);
  off64_t corrupt_at =
    pos + sizeof(entry_header_t) + h.pre_pad;
  corrupt(wfd, corrupt_at);
}


void FileJournal::corrupt_footer_magic(
  int wfd,
  uint64_t seq)
{
  off64_t pos = 0;
  entry_header_t h;
  get_header(seq, &pos, &h);
  off64_t corrupt_at =
    pos + sizeof(entry_header_t) + h.pre_pad +
    h.len + h.post_pad +
    (reinterpret_cast<char*>(&h.magic2) - reinterpret_cast<char*>(&h));
  corrupt(wfd, corrupt_at);
}


void FileJournal::corrupt_header_magic(
  int wfd,
  uint64_t seq)
{
  off64_t pos = 0;
  entry_header_t h;
  get_header(seq, &pos, &h);
  off64_t corrupt_at =
    pos +
    (reinterpret_cast<char*>(&h.magic2) - reinterpret_cast<char*>(&h));
  corrupt(wfd, corrupt_at);
}
