// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2009 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 * Series of functions to test your rados installation. Notice
 * that this code is not terribly robust -- for instance, if you
 * try and bench on a pool you don't have permission to access
 * it will just loop forever.
 */
#include "common/Cond.h"
#include "obj_bencher.h"

#include <iostream>
#include <fstream>

#include <cerrno>

#include <stdlib.h>
#include <time.h>
#include <sstream>
#include <vector>


const std::string BENCH_LASTRUN_METADATA = "benchmark_last_metadata";
const std::string BENCH_PREFIX = "benchmark_data";

static std::string generate_object_prefix(int pid = 0) {
  char hostname[30];
  gethostname(hostname, sizeof(hostname)-1);
  hostname[sizeof(hostname)-1] = 0;

  if (!pid)
    pid = getpid();

  std::ostringstream oss;
  oss << BENCH_PREFIX << "_" << hostname << "_" << pid;
  return oss.str();
}

static std::string generate_object_name(int objnum, int pid = 0)
{
  std::ostringstream oss;
  oss << generate_object_prefix(pid) << "_object" << objnum;
  return oss.str();
}

static void sanitize_object_contents (bench_data *data, int length) {
  memset(data->object_contents, 'z', length);
}

ostream& ObjBencher::out(ostream& os, utime_t& t)
{
  if (show_time)
    return t.localtime(os) << " ";
  else
    return os << " ";
}

ostream& ObjBencher::out(ostream& os)
{
  utime_t cur_time = ceph_clock_now(cct);
  return out(os, cur_time);
}

void *ObjBencher::status_printer(void *_bencher) {
  ObjBencher *bencher = static_cast<ObjBencher *>(_bencher);
  bench_data& data = bencher->data;
  Cond cond;
  int i = 0;
  int previous_writes = 0;
  int cycleSinceChange = 0;
  double bandwidth;
  utime_t ONE_SECOND;
  ONE_SECOND.set_from_double(1.0);
  bencher->lock.Lock();
  while(!data.done) {
    utime_t cur_time = ceph_clock_now(bencher->cct);

    if (i % 20 == 0) {
      if (i > 0)
	cur_time.localtime(cout) << "min lat: " << data.min_latency
	     << " max lat: " << data.max_latency
	     << " avg lat: " << data.avg_latency << std::endl;
      //I'm naughty and don't reset the fill
      bencher->out(cout, cur_time) << setfill(' ')
	   << setw(5) << "sec"
	   << setw(8) << "Cur ops"
	   << setw(10) << "started"
	   << setw(10) << "finished"
	   << setw(10) << "avg MB/s"
	   << setw(10) << "cur MB/s"
	   << setw(10) << "last lat"
	   << setw(10) << "avg lat" << std::endl;
    }
    if (cycleSinceChange)
      bandwidth = (double)(data.finished - previous_writes)
	* (data.trans_size)
	/ (1024*1024)
	/ cycleSinceChange;
    else
      bandwidth = 0;

    if (!isnan(bandwidth)) {
      if (bandwidth > data.idata.max_bandwidth)
        data.idata.max_bandwidth = bandwidth;
      if (bandwidth < data.idata.min_bandwidth)
        data.idata.min_bandwidth = bandwidth;

      data.history.bandwidth.push_back(bandwidth);
    }

    double avg_bandwidth = (double) (data.trans_size) * (data.finished)
      / (double)(cur_time - data.start_time) / (1024*1024);
    if (previous_writes != data.finished) {
      previous_writes = data.finished;
      cycleSinceChange = 0;
      bencher->out(cout, cur_time) << setfill(' ')
	   << setw(5) << i
	   << setw(8) << data.in_flight
	   << setw(10) << data.started
	   << setw(10) << data.finished
	   << setw(10) << avg_bandwidth
	   << setw(10) << bandwidth
	   << setw(10) << (double)data.cur_latency
	   << setw(10) << data.avg_latency << std::endl;
    }
    else {
      bencher->out(cout, cur_time) << setfill(' ')
	   << setw(5) << i
	   << setw(8) << data.in_flight
	   << setw(10) << data.started
	   << setw(10) << data.finished
	   << setw(10) << avg_bandwidth
	   << setw(10) << '0'
	   << setw(10) << '-'
	   << setw(10) << data.avg_latency << std::endl;
    }
    ++i;
    ++cycleSinceChange;
    cond.WaitInterval(bencher->cct, bencher->lock, ONE_SECOND);
  }
  bencher->lock.Unlock();
  return NULL;
}

int ObjBencher::aio_bench(
  int operation, int secondsToRun,
  int maxObjectsToCreate,
  int concurrentios, int op_size, bool cleanup, const char* run_name) {

  if (concurrentios <= 0) 
    return -EINVAL;

  int object_size = op_size;
  int num_objects = 0;
  int r = 0;
  int prevPid = 0;

  // default metadata object is used if user does not specify one
  const std::string run_name_meta = (run_name == NULL ? BENCH_LASTRUN_METADATA : std::string(run_name));

  //get data from previous write run, if available
  if (operation != OP_WRITE) {
    r = fetch_bench_metadata(run_name_meta, &object_size, &num_objects, &prevPid);
    if (r < 0) {
      if (r == -ENOENT)
	cerr << "Must write data before running a read benchmark!" << std::endl;
      return r;
    }
  } else {
    object_size = op_size;
  }

  char* contentsChars = new char[object_size];
  lock.Lock();
  data.done = false;
  data.object_size = object_size;
  data.trans_size = op_size;
  data.in_flight = 0;
  data.started = 0;
  data.finished = num_objects;
  data.min_latency = 9999.0; // this better be higher than initial latency!
  data.max_latency = 0;
  data.avg_latency = 0;
  data.idata.min_bandwidth = 99999999.0;
  data.idata.max_bandwidth = 0;
  data.object_contents = contentsChars;
  lock.Unlock();

  //fill in contentsChars deterministically so we can check returns
  sanitize_object_contents(&data, data.object_size);

  if (OP_WRITE == operation) {
    r = write_bench(secondsToRun, maxObjectsToCreate, concurrentios, run_name_meta);
    if (r != 0) goto out;
  }
  else if (OP_SEQ_READ == operation) {
    r = seq_read_bench(secondsToRun, num_objects, concurrentios, prevPid);
    if (r != 0) goto out;
  }
  else if (OP_RAND_READ == operation) {
    r = rand_read_bench(secondsToRun, num_objects, concurrentios, prevPid);
    if (r != 0) goto out;
  }

  if (OP_WRITE == operation && cleanup) {
    r = fetch_bench_metadata(run_name_meta, &object_size, &num_objects, &prevPid);
    if (r < 0) {
      if (r == -ENOENT)
	cerr << "Should never happen: bench metadata missing for current run!" << std::endl;
      goto out;
    }
 
    r = clean_up(num_objects, prevPid, concurrentios);
    if (r != 0) goto out;

    // lastrun file
    r = sync_remove(run_name_meta);
    if (r != 0) goto out;
  }

 out:
  delete[] contentsChars;
  return r;
}

struct lock_cond {
  lock_cond(Mutex *_lock) : lock(_lock) {}
  Mutex *lock;
  Cond cond;
};

void _aio_cb(void *cb, void *arg) {
  struct lock_cond *lc = (struct lock_cond *)arg;
  lc->lock->Lock();
  lc->cond.Signal();
  lc->lock->Unlock();
}

static double vec_stddev(vector<double>& v)
{
  double mean = 0;

  if (v.size() < 2)
    return 0;

  vector<double>::iterator iter;
  for (iter = v.begin(); iter != v.end(); ++iter) {
    mean += *iter;
  }

  mean /= v.size();

  double stddev = 0;
  for (iter = v.begin(); iter != v.end(); ++iter) {
    double dev = *iter - mean;
    dev *= dev;
    stddev += dev;
  }
  stddev /= (v.size() - 1);
  return sqrt(stddev);
}

int ObjBencher::fetch_bench_metadata(const std::string& metadata_file, int* object_size, int* num_objects, int* prevPid) {
  int r = 0;
  bufferlist object_data;

  r = sync_read(metadata_file, object_data, sizeof(int)*3);
  if (r <= 0) {
    // treat an empty file as a file that does not exist
    if (r == 0) {
      r = -ENOENT;
    }
    return r;
  }
  bufferlist::iterator p = object_data.begin();
  ::decode(*object_size, p);
  ::decode(*num_objects, p);
  ::decode(*prevPid, p);

  return 0;
}

int ObjBencher::write_bench(int secondsToRun, int maxObjectsToCreate,
			    int concurrentios, const string& run_name_meta) {
  if (concurrentios <= 0) 
    return -EINVAL;

  if (maxObjectsToCreate > 0 && concurrentios > maxObjectsToCreate)
    concurrentios = maxObjectsToCreate;
  out(cout) << "Maintaining " << concurrentios << " concurrent writes of "
	    << data.object_size << " bytes for up to "
	    << secondsToRun << " seconds or "
	    << maxObjectsToCreate << " objects"
	    << std::endl;
  bufferlist* newContents = 0;

  std::string prefix = generate_object_prefix();
  out(cout) << "Object prefix: " << prefix << std::endl;

  std::vector<string> name(concurrentios);
  std::string newName;
  bufferlist* contents[concurrentios];
  double total_latency = 0;
  std::vector<utime_t> start_times(concurrentios);
  utime_t stopTime;
  int r = 0;
  bufferlist b_write;
  lock_cond lc(&lock);
  utime_t runtime;
  utime_t timePassed;

  r = completions_init(concurrentios);

  //set up writes so I can start them together
  for (int i = 0; i<concurrentios; ++i) {
    name[i] = generate_object_name(i);
    contents[i] = new bufferlist();
    snprintf(data.object_contents, data.object_size, "I'm the %16dth object!", i);
    contents[i]->append(data.object_contents, data.object_size);
  }

  pthread_t print_thread;

  pthread_create(&print_thread, NULL, ObjBencher::status_printer, (void *)this);
  lock.Lock();
  data.start_time = ceph_clock_now(cct);
  lock.Unlock();
  for (int i = 0; i<concurrentios; ++i) {
    start_times[i] = ceph_clock_now(cct);
    r = create_completion(i, _aio_cb, (void *)&lc);
    if (r < 0)
      goto ERR;
    r = aio_write(name[i], i, *contents[i], data.object_size);
    if (r < 0) { //naughty, doesn't clean up heap
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    lock.Unlock();
  }

  //keep on adding new writes as old ones complete until we've passed minimum time
  int slot;

  //don't need locking for reads because other thread doesn't write

  runtime.set_from_double(secondsToRun);
  stopTime = data.start_time + runtime;
  slot = 0;
  lock.Lock();
  while( ceph_clock_now(cct) < stopTime &&
	 (!maxObjectsToCreate || data.started < maxObjectsToCreate)) {
    bool found = false;
    while (1) {
      int old_slot = slot;
      do {
	if (completion_is_done(slot)) {
          found = true;
	  break;
	}
        slot++;
        if (slot == concurrentios) {
          slot = 0;
        }
      } while (slot != old_slot);
      if (found)
        break;
      lc.cond.Wait(lock);
    }
    lock.Unlock();
    //create new contents and name on the heap, and fill them
    newContents = new bufferlist();
    newName = generate_object_name(data.started);
    snprintf(data.object_contents, data.object_size, "I'm the %16dth object!", data.started);
    newContents->append(data.object_contents, data.object_size);
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r != 0) {
      lock.Unlock();
      goto ERR;
    }
    data.cur_latency = ceph_clock_now(cct) - start_times[slot];
    data.history.latency.push_back(data.cur_latency);
    total_latency += data.cur_latency;
    if( data.cur_latency > data.max_latency) data.max_latency = data.cur_latency;
    if (data.cur_latency < data.min_latency) data.min_latency = data.cur_latency;
    ++data.finished;
    data.avg_latency = total_latency / data.finished;
    --data.in_flight;
    lock.Unlock();
    release_completion(slot);
    timePassed = ceph_clock_now(cct) - data.start_time;

    //write new stuff to backend, then delete old stuff
    //and save locations of new stuff for later deletion
    start_times[slot] = ceph_clock_now(cct);
    r = create_completion(slot, _aio_cb, &lc);
    if (r < 0)
      goto ERR;
    r = aio_write(newName, slot, *newContents, data.object_size);
    if (r < 0) {//naughty; doesn't clean up heap space.
      goto ERR;
    }
    delete contents[slot];
    name[slot] = newName;
    contents[slot] = newContents;
    newContents = 0;
    lock.Lock();
    ++data.started;
    ++data.in_flight;
  }
  lock.Unlock();

  while (data.finished < data.started) {
    slot = data.finished % concurrentios;
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r != 0) {
      lock.Unlock();
      goto ERR;
    }
    data.cur_latency = ceph_clock_now(cct) - start_times[slot];
    data.history.latency.push_back(data.cur_latency);
    total_latency += data.cur_latency;
    if (data.cur_latency > data.max_latency) data.max_latency = data.cur_latency;
    if (data.cur_latency < data.min_latency) data.min_latency = data.cur_latency;
    ++data.finished;
    data.avg_latency = total_latency / data.finished;
    --data.in_flight;
    lock.Unlock();
    release_completion(slot);
    delete contents[slot];
  }

  timePassed = ceph_clock_now(cct) - data.start_time;
  lock.Lock();
  data.done = true;
  lock.Unlock();

  pthread_join(print_thread, NULL);

  double bandwidth;
  bandwidth = ((double)data.finished)*((double)data.object_size)/(double)timePassed;
  bandwidth = bandwidth/(1024*1024); // we want it in MB/sec
  char bw[20];
  snprintf(bw, sizeof(bw), "%.3lf \n", bandwidth);

  out(cout) << "Total time run:         " << timePassed << std::endl
       << "Total writes made:      " << data.finished << std::endl
       << "Write size:             " << data.object_size << std::endl
       << "Bandwidth (MB/sec):     " << bw << std::endl
       << "Stddev Bandwidth:       " << vec_stddev(data.history.bandwidth) << std::endl
       << "Max bandwidth (MB/sec): " << data.idata.max_bandwidth << std::endl
       << "Min bandwidth (MB/sec): " << data.idata.min_bandwidth << std::endl
       << "Average Latency:        " << data.avg_latency << std::endl
       << "Stddev Latency:         " << vec_stddev(data.history.latency) << std::endl
       << "Max latency:            " << data.max_latency << std::endl
       << "Min latency:            " << data.min_latency << std::endl;

  //write object size/number data for read benchmarks
  ::encode(data.object_size, b_write);
  ::encode(data.finished, b_write);
  ::encode(getpid(), b_write);

  // persist meta-data for further cleanup or read
  sync_write(run_name_meta, b_write, sizeof(int)*3);

  completions_done();

  return 0;

 ERR:
  lock.Lock();
  data.done = 1;
  lock.Unlock();
  pthread_join(print_thread, NULL);
  delete newContents;
  return -5;
}

int ObjBencher::seq_read_bench(int seconds_to_run, int num_objects, int concurrentios, int pid) {
  lock_cond lc(&lock);

  if (concurrentios <= 0) 
    return -EINVAL;

  std::vector<string> name(concurrentios);
  std::string newName;
  bufferlist* contents[concurrentios];
  int index[concurrentios];
  int errors = 0;
  utime_t start_time;
  std::vector<utime_t> start_times(concurrentios);
  utime_t time_to_run;
  time_to_run.set_from_double(seconds_to_run);
  double total_latency = 0;
  int r = 0;
  utime_t runtime;
  sanitize_object_contents(&data, data.object_size); //clean it up once; subsequent
  //changes will be safe because string length should remain the same

  r = completions_init(concurrentios);
  if (r < 0)
    return r;

  //set up initial reads
  for (int i = 0; i < concurrentios; ++i) {
    name[i] = generate_object_name(i, pid);
    contents[i] = new bufferlist();
  }

  lock.Lock();
  data.finished = 0;
  data.start_time = ceph_clock_now(cct);
  lock.Unlock();

  pthread_t print_thread;
  pthread_create(&print_thread, NULL, status_printer, (void *)this);

  utime_t finish_time = data.start_time + time_to_run;
  //start initial reads
  for (int i = 0; i < concurrentios; ++i) {
    index[i] = i;
    start_times[i] = ceph_clock_now(cct);
    create_completion(i, _aio_cb, (void *)&lc);
    r = aio_read(name[i], i, contents[i], data.object_size);
    if (r < 0) { //naughty, doesn't clean up heap -- oh, or handle the print thread!
      cerr << "r = " << r << std::endl;
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    lock.Unlock();
  }

  //keep on adding new reads as old ones complete
  int slot;
  bufferlist *cur_contents;

  slot = 0;
  while (seconds_to_run && (ceph_clock_now(cct) < finish_time) &&
      num_objects > data.started) {
    lock.Lock();
    int old_slot = slot;
    bool found = false;
    while (1) {
      do {
	if (completion_is_done(slot)) {
          found = true;
	  break;
	}
        slot++;
        if (slot == concurrentios) {
          slot = 0;
        }
      } while (slot != old_slot);
      if (found) {
	break;
      }
      lc.cond.Wait(lock);
    }
    lock.Unlock();
    newName = generate_object_name(data.started, pid);
    int current_index = index[slot];
    index[slot] = data.started;
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r < 0) {
      cerr << "read got " << r << std::endl;
      lock.Unlock();
      goto ERR;
    }
    data.cur_latency = ceph_clock_now(cct) - start_times[slot];
    total_latency += data.cur_latency;
    if( data.cur_latency > data.max_latency) data.max_latency = data.cur_latency;
    if (data.cur_latency < data.min_latency) data.min_latency = data.cur_latency;
    ++data.finished;
    data.avg_latency = total_latency / data.finished;
    --data.in_flight;
    lock.Unlock();
    release_completion(slot);
    cur_contents = contents[slot];

    //start new read and check data if requested
    start_times[slot] = ceph_clock_now(cct);
    contents[slot] = new bufferlist();
    create_completion(slot, _aio_cb, (void *)&lc);
    r = aio_read(newName, slot, contents[slot], data.object_size);
    if (r < 0) {
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    snprintf(data.object_contents, data.object_size, "I'm the %16dth object!", current_index);
    lock.Unlock();
    if (memcmp(data.object_contents, cur_contents->c_str(), data.object_size) != 0) {
      cerr << name[slot] << " is not correct!" << std::endl;
      ++errors;
    }
    name[slot] = newName;
    delete cur_contents;
  }

  //wait for final reads to complete
  while (data.finished < data.started) {
    slot = data.finished % concurrentios;
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r < 0) {
      cerr << "read got " << r << std::endl;
      lock.Unlock();
      goto ERR;
    }
    data.cur_latency = ceph_clock_now(cct) - start_times[slot];
    total_latency += data.cur_latency;
    if (data.cur_latency > data.max_latency) data.max_latency = data.cur_latency;
    if (data.cur_latency < data.min_latency) data.min_latency = data.cur_latency;
    ++data.finished;
    data.avg_latency = total_latency / data.finished;
    --data.in_flight;
    release_completion(slot);
    snprintf(data.object_contents, data.object_size, "I'm the %16dth object!", index[slot]);
    lock.Unlock();
    if (memcmp(data.object_contents, contents[slot]->c_str(), data.object_size) != 0) {
      cerr << name[slot] << " is not correct!" << std::endl;
      ++errors;
    }
    delete contents[slot];
  }

  runtime = ceph_clock_now(cct) - data.start_time;
  lock.Lock();
  data.done = true;
  lock.Unlock();

  pthread_join(print_thread, NULL);

  double bandwidth;
  bandwidth = ((double)data.finished)*((double)data.object_size)/(double)runtime;
  bandwidth = bandwidth/(1024*1024); // we want it in MB/sec
  char bw[20];
  snprintf(bw, sizeof(bw), "%.3lf \n", bandwidth);

  out(cout) << "Total time run:        " << runtime << std::endl
       << "Total reads made:     " << data.finished << std::endl
       << "Read size:            " << data.object_size << std::endl
       << "Bandwidth (MB/sec):    " << bw << std::endl
       << "Average Latency:       " << data.avg_latency << std::endl
       << "Max latency:           " << data.max_latency << std::endl
       << "Min latency:           " << data.min_latency << std::endl;

  completions_done();

  return 0;

 ERR:
  lock.Lock();
  data.done = 1;
  lock.Unlock();
  pthread_join(print_thread, NULL);
  return -5;
}

int ObjBencher::rand_read_bench(int seconds_to_run, int num_objects, int concurrentios, int pid)
{
  lock_cond lc(&lock);

  if (concurrentios <= 0) 
    return -EINVAL;
 
  std::vector<string> name(concurrentios);
  std::string newName;
  bufferlist* contents[concurrentios];
  int index[concurrentios];
  int errors = 0;
  utime_t start_time;
  std::vector<utime_t> start_times(concurrentios);
  utime_t time_to_run;
  time_to_run.set_from_double(seconds_to_run);
  double total_latency = 0;
  int r = 0;
  utime_t runtime;
  sanitize_object_contents(&data, data.object_size); //clean it up once; subsequent
  //changes will be safe because string length should remain the same

  srand (time(NULL));

  r = completions_init(concurrentios);
  if (r < 0)
    return r;

  //set up initial reads
  for (int i = 0; i < concurrentios; ++i) {
    name[i] = generate_object_name(i, pid);
    contents[i] = new bufferlist();
  }

  lock.Lock();
  data.finished = 0;
  data.start_time = ceph_clock_now(g_ceph_context);
  lock.Unlock();

  pthread_t print_thread;
  pthread_create(&print_thread, NULL, status_printer, (void *)this);

  utime_t finish_time = data.start_time + time_to_run;
  //start initial reads
  for (int i = 0; i < concurrentios; ++i) {
    index[i] = i;
    start_times[i] = ceph_clock_now(g_ceph_context);
    create_completion(i, _aio_cb, (void *)&lc);
    r = aio_read(name[i], i, contents[i], data.object_size);
    if (r < 0) { //naughty, doesn't clean up heap -- oh, or handle the print thread!
      cerr << "r = " << r << std::endl;
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    lock.Unlock();
  }

  //keep on adding new reads as old ones complete
  int slot;
  bufferlist *cur_contents;
  int rand_id;

  slot = 0;
  while (seconds_to_run && (ceph_clock_now(g_ceph_context) < finish_time)) {
    lock.Lock();
    int old_slot = slot;
    bool found = false;
    while (1) {
      do {
        if (completion_is_done(slot)) {
          found = true;
          break;
        }
        slot++;
        if (slot == concurrentios) {
          slot = 0;
        }
      } while (slot != old_slot);
      if (found) {
        break;
      }
      lc.cond.Wait(lock);
    }
    lock.Unlock();
    rand_id = rand() % num_objects;
    newName = generate_object_name(rand_id, pid);
    int current_index = index[slot];
    index[slot] = rand_id;
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r < 0) {
      cerr << "read got " << r << std::endl;
      lock.Unlock();
      goto ERR;
    }
    data.cur_latency = ceph_clock_now(g_ceph_context) - start_times[slot];
    total_latency += data.cur_latency;
    if( data.cur_latency > data.max_latency) data.max_latency = data.cur_latency;
    if (data.cur_latency < data.min_latency) data.min_latency = data.cur_latency;
    ++data.finished;
    data.avg_latency = total_latency / data.finished;
    --data.in_flight;
    lock.Unlock();
    release_completion(slot);
    cur_contents = contents[slot];

    //start new read and check data if requested
    start_times[slot] = ceph_clock_now(g_ceph_context);
    contents[slot] = new bufferlist();
    create_completion(slot, _aio_cb, (void *)&lc);
    r = aio_read(newName, slot, contents[slot], data.object_size);
    if (r < 0) {
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    snprintf(data.object_contents, data.object_size, "I'm the %16dth object!", current_index);
    lock.Unlock();
    if (memcmp(data.object_contents, cur_contents->c_str(), data.object_size) != 0) {
      cerr << name[slot] << " is not correct!" << std::endl;
      ++errors;
    }
    name[slot] = newName;
    delete cur_contents;
  }

  //wait for final reads to complete
  while (data.finished < data.started) {
    slot = data.finished % concurrentios;
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r < 0) {
      cerr << "read got " << r << std::endl;
      lock.Unlock();
      goto ERR;
    }
    data.cur_latency = ceph_clock_now(g_ceph_context) - start_times[slot];
    total_latency += data.cur_latency;
    if (data.cur_latency > data.max_latency) data.max_latency = data.cur_latency;
    if (data.cur_latency < data.min_latency) data.min_latency = data.cur_latency;
    ++data.finished;
    data.avg_latency = total_latency / data.finished;
    --data.in_flight;
    release_completion(slot);
    snprintf(data.object_contents, data.object_size, "I'm the %16dth object!", index[slot]);
    lock.Unlock();
    if (memcmp(data.object_contents, contents[slot]->c_str(), data.object_size) != 0) {
      cerr << name[slot] << " is not correct!" << std::endl;
      ++errors;
    }
    delete contents[slot];
  }

  runtime = ceph_clock_now(g_ceph_context) - data.start_time;
  lock.Lock();
  data.done = true;
  lock.Unlock();

  pthread_join(print_thread, NULL);

  double bandwidth;
  bandwidth = ((double)data.finished)*((double)data.object_size)/(double)runtime;
  bandwidth = bandwidth/(1024*1024); // we want it in MB/sec
  char bw[20];
  snprintf(bw, sizeof(bw), "%.3lf \n", bandwidth);

  out(cout) << "Total time run:        " << runtime << std::endl
       << "Total reads made:     " << data.finished << std::endl
       << "Read size:            " << data.object_size << std::endl
       << "Bandwidth (MB/sec):    " << bw << std::endl
       << "Average Latency:       " << data.avg_latency << std::endl
       << "Max latency:           " << data.max_latency << std::endl
       << "Min latency:           " << data.min_latency << std::endl;

  completions_done();

  return 0;

 ERR:
  lock.Lock();
  data.done = 1;
  lock.Unlock();
  pthread_join(print_thread, NULL);
  return -5;
}

int ObjBencher::clean_up(const char* prefix, int concurrentios, const char* run_name) {
  int r = 0;
  int object_size;
  int num_objects;
  int prevPid;

  // default meta object if user does not specify one
  const std::string run_name_meta = (run_name == NULL ? BENCH_LASTRUN_METADATA : std::string(run_name));

  r = fetch_bench_metadata(run_name_meta, &object_size, &num_objects, &prevPid);
  if (r < 0) {
    // if the metadata file is not found we should try to do a linear search on the prefix
    if (r == -ENOENT && prefix != NULL) {
      return clean_up_slow(prefix, concurrentios);
    }
    else {
      return r;
    }
  }

  r = clean_up(num_objects, prevPid, concurrentios);
  if (r != 0) return r;

  r = sync_remove(run_name_meta);
  if (r != 0) return r;

  return 0;
}

int ObjBencher::clean_up(int num_objects, int prevPid, int concurrentios) {
  lock_cond lc(&lock);
  
  if (concurrentios <= 0) 
    return -EINVAL;

  std::vector<string> name(concurrentios);
  std::string newName;
  int r = 0;
  utime_t runtime;
  int slot = 0;

  lock.Lock();
  data.done = false;
  data.in_flight = 0;
  data.started = 0;
  data.finished = 0;
  lock.Unlock();

  // don't start more completions than files
  if (num_objects < concurrentios) {
    concurrentios = num_objects;
  }

  r = completions_init(concurrentios);
  if (r < 0)
    return r;

  //set up initial removes
  for (int i = 0; i < concurrentios; ++i) {
    name[i] = generate_object_name(i, prevPid);
  }

  //start initial removes
  for (int i = 0; i < concurrentios; ++i) {
    create_completion(i, _aio_cb, (void *)&lc);
    r = aio_remove(name[i], i);
    if (r < 0) { //naughty, doesn't clean up heap
      cerr << "r = " << r << std::endl;
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    lock.Unlock();
  }

  //keep on adding new removes as old ones complete
  while (data.started < num_objects) {
    lock.Lock();
    int old_slot = slot;
    bool found = false;
    while (1) {
      do {
	if (completion_is_done(slot)) {
          found = true;
	  break;
	}
        slot++;
        if (slot == concurrentios) {
          slot = 0;
        }
      } while (slot != old_slot);
      if (found) {
	break;
      }
      lc.cond.Wait(lock);
    }
    lock.Unlock();
    newName = generate_object_name(data.started, prevPid);
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r != 0 && r != -ENOENT) { // file does not exist
      cerr << "remove got " << r << std::endl;
      lock.Unlock();
      goto ERR;
    }
    ++data.finished;
    --data.in_flight;
    lock.Unlock();
    release_completion(slot);

    //start new remove and check data if requested
    create_completion(slot, _aio_cb, (void *)&lc);
    r = aio_remove(newName, slot);
    if (r < 0) {
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    lock.Unlock();
    name[slot] = newName;
  }

  //wait for final removes to complete
  while (data.finished < data.started) {
    slot = data.finished % concurrentios;
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r != 0 && r != -ENOENT) { // file does not exist
      cerr << "remove got " << r << std::endl;
      lock.Unlock();
      goto ERR;
    }
    ++data.finished;
    --data.in_flight;
    release_completion(slot);
    lock.Unlock();
  }

  lock.Lock();
  data.done = true;
  lock.Unlock();

  completions_done();

  return 0;

 ERR:
  lock.Lock();
  data.done = 1;
  lock.Unlock();
  return -5;
}

/**
 * Return objects from the datastore which match a prefix.
 *
 * Clears the list and populates it with any objects which match the
 * prefix. The list is guaranteed to have at least one item when the
 * function returns true.
 *
 * @in
 * @param prefix the prefix to match against
 * @param objects return list of objects
 * @returns true if there are any objects in the store which match
 * the prefix, false if there are no more
 */
bool ObjBencher::more_objects_matching_prefix(const std::string& prefix, std::list<std::string>* objects) {
  std::list<std::string> unfiltered_objects;

  objects->clear();

  while (objects->empty()) {
    bool objects_remain = get_objects(&unfiltered_objects, 20);
    if (!objects_remain)
      return false;

    std::list<std::string>::const_iterator i = unfiltered_objects.begin();
    for ( ; i != unfiltered_objects.end(); ++i) {
      const std::string& next = *i;

      if (next.substr(0, prefix.length()) == prefix) {
        objects->push_back(next);
      }
    }
  }

  return true;
}

int ObjBencher::clean_up_slow(const std::string& prefix, int concurrentios) {
  lock_cond lc(&lock);

  if (concurrentios <= 0) 
    return -EINVAL;

  std::vector<string> name(concurrentios);
  std::string newName;
  int r = 0;
  utime_t runtime;
  int slot = 0;
  std::list<std::string> objects;
  bool objects_remain = true;

  lock.Lock();
  data.done = false;
  data.in_flight = 0;
  data.started = 0;
  data.finished = 0;
  lock.Unlock();

  out(cout) << "Warning: using slow linear search" << std::endl;

  r = completions_init(concurrentios);
  if (r < 0)
    return r;

  //set up initial removes
  for (int i = 0; i < concurrentios; ++i) {
    if (objects.empty()) {
      // if there are fewer objects than concurrent ios, don't generate extras
      bool objects_found = more_objects_matching_prefix(prefix, &objects);
      if (!objects_found) {
        concurrentios = i;
        objects_remain = false;
        break;
      }
    }

    name[i] = objects.front();
    objects.pop_front();
  }

  //start initial removes
  for (int i = 0; i < concurrentios; ++i) {
    create_completion(i, _aio_cb, (void *)&lc);
    r = aio_remove(name[i], i);
    if (r < 0) { //naughty, doesn't clean up heap
      cerr << "r = " << r << std::endl;
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    lock.Unlock();
  }

  //keep on adding new removes as old ones complete
  while (objects_remain) {
    lock.Lock();
    int old_slot = slot;
    bool found = false;
    while (1) {
      do {
	if (completion_is_done(slot)) {
          found = true;
	  break;
	}
        slot++;
        if (slot == concurrentios) {
          slot = 0;
        }
      } while (slot != old_slot);
      if (found) {
	break;
      }
      lc.cond.Wait(lock);
    }
    lock.Unlock();

    // get more objects if necessary
    if (objects.empty()) {
      objects_remain = more_objects_matching_prefix(prefix, &objects);
      // quit if there are no more
      if (!objects_remain) {
        break;
      }
    }

    // get the next object
    newName = objects.front();
    objects.pop_front();

    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r != 0 && r != -ENOENT) { // file does not exist
      cerr << "remove got " << r << std::endl;
      lock.Unlock();
      goto ERR;
    }
    ++data.finished;
    --data.in_flight;
    lock.Unlock();
    release_completion(slot);

    //start new remove and check data if requested
    create_completion(slot, _aio_cb, (void *)&lc);
    r = aio_remove(newName, slot);
    if (r < 0) {
      goto ERR;
    }
    lock.Lock();
    ++data.started;
    ++data.in_flight;
    lock.Unlock();
    name[slot] = newName;
  }

  //wait for final removes to complete
  while (data.finished < data.started) {
    slot = data.finished % concurrentios;
    completion_wait(slot);
    lock.Lock();
    r = completion_ret(slot);
    if (r != 0 && r != -ENOENT) { // file does not exist
      cerr << "remove got " << r << std::endl;
      lock.Unlock();
      goto ERR;
    }
    ++data.finished;
    --data.in_flight;
    release_completion(slot);
    lock.Unlock();
  }

  lock.Lock();
  data.done = true;
  lock.Unlock();

  completions_done();

  out(cout) << "Removed " << data.finished << " object" << (data.finished != 1 ? "s" : "") << std::endl;

  return 0;

 ERR:
  lock.Lock();
  data.done = 1;
  lock.Unlock();
  return -5;
}
