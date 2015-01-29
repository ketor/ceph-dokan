// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef __CEPH_LOG_LOG_H
#define __CEPH_LOG_LOG_H

#include "common/Thread.h"

#include <pthread.h>

#include "Entry.h"
#include "EntryQueue.h"
#include "SubsystemMap.h"

namespace ceph {
namespace log {

class Log : private Thread
{
  Log **m_indirect_this;

  SubsystemMap *m_subs;
  
  pthread_mutex_t m_queue_mutex;
  pthread_mutex_t m_flush_mutex;
  pthread_cond_t m_cond_loggers;
  pthread_cond_t m_cond_flusher;

  pthread_t m_queue_mutex_holder;
  pthread_t m_flush_mutex_holder;

  EntryQueue m_new;    ///< new entries
  EntryQueue m_recent; ///< recent (less new) entries we've already written at low detail

  std::string m_log_file;
  int m_fd;

  int m_syslog_log, m_syslog_crash;
  int m_stderr_log, m_stderr_crash;

  bool m_stop;

  int m_max_new, m_max_recent;

  bool m_inject_segv;

  void *entry();

  void _flush(EntryQueue *q, EntryQueue *requeue, bool crash);

  void _log_message(const char *s, bool crash);

public:
  Log(SubsystemMap *s);
  virtual ~Log();

  void set_flush_on_exit();

  void set_max_new(int n);
  void set_max_recent(int n);
  void set_log_file(std::string fn);
  void reopen_log_file();

  void flush(); 

  void dump_recent();

  void set_syslog_level(int log, int crash);
  void set_stderr_level(int log, int crash);

  Entry *create_entry(int level, int subsys);
  void submit_entry(Entry *e);

  void start();
  void stop();

  /// true if the log lock is held by our thread
  bool is_inside_log_lock();

  /// induce a segv on the next log event
  void inject_segv();
};

}
}

#endif
