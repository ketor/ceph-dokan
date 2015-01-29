// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <time.h>

#include <boost/algorithm/string.hpp>

#include "common/admin_socket.h"
#include "common/perf_counters.h"
#include "common/Thread.h"
#include "common/ceph_context.h"
#include "common/config.h"
#include "common/debug.h"
#include "common/HeartbeatMap.h"
#include "common/errno.h"
#include "common/lockdep.h"
#include "common/Formatter.h"
#include "log/Log.h"
#include "auth/Crypto.h"
#include "include/str_list.h"
#include "common/Mutex.h"
#include "common/Cond.h"

#include <iostream>
#include <pthread.h>

#include "include/Spinlock.h"

using ceph::HeartbeatMap;

class CephContextServiceThread : public Thread
{
public:
  CephContextServiceThread(CephContext *cct)
    : _lock("CephContextServiceThread::_lock"),
      _reopen_logs(false), _exit_thread(false), _cct(cct)
  {
  }

  ~CephContextServiceThread() {}

  void *entry()
  {
    while (1) {
      Mutex::Locker l(_lock);

      if (_cct->_conf->heartbeat_interval) {
        utime_t interval(_cct->_conf->heartbeat_interval, 0);
        _cond.WaitInterval(_cct, _lock, interval);
      } else
        _cond.Wait(_lock);

      if (_exit_thread) {
        break;
      }

      if (_reopen_logs) {
        _cct->_log->reopen_log_file();
        _reopen_logs = false;
      }
      _cct->_heartbeat_map->check_touch_file();
    }
    return NULL;
  }

  void reopen_logs()
  {
    Mutex::Locker l(_lock);
    _reopen_logs = true;
    _cond.Signal();
  }

  void exit_thread()
  {
    Mutex::Locker l(_lock);
    _exit_thread = true;
    _cond.Signal();
  }

private:
  Mutex _lock;
  Cond _cond;
  bool _reopen_logs;
  bool _exit_thread;
  CephContext *_cct;
};


/**
 * observe logging config changes
 *
 * The logging subsystem sits below most of the ceph code, including
 * the config subsystem, to keep it simple and self-contained.  Feed
 * logging-related config changes to the log.
 */
class LogObs : public md_config_obs_t {
  ceph::log::Log *log;

public:
  LogObs(ceph::log::Log *l) : log(l) {}

  const char** get_tracked_conf_keys() const {
    static const char *KEYS[] = {
      "log_file",
      "log_max_new",
      "log_max_recent",
      "log_to_syslog",
      "err_to_syslog",
      "log_to_stderr",
      "err_to_stderr",
      NULL
    };
    return KEYS;
  }

  void handle_conf_change(const md_config_t *conf,
                          const std::set <std::string> &changed) {
    // stderr
    if (changed.count("log_to_stderr") || changed.count("err_to_stderr")) {
      int l = conf->log_to_stderr ? 99 : (conf->err_to_stderr ? -1 : -2);
      log->set_stderr_level(l, l);
    }

    // syslog
    if (changed.count("log_to_syslog")) {
      int l = conf->log_to_syslog ? 99 : (conf->err_to_syslog ? -1 : -2);
      log->set_syslog_level(l, l);
    }

    // file
    if (changed.count("log_file")) {
      log->set_log_file(conf->log_file);
      log->reopen_log_file();
    }

    if (changed.count("log_max_new")) {
      log->set_max_new(conf->log_max_new);
    }

    if (changed.count("log_max_recent")) {
      log->set_max_recent(conf->log_max_recent);
    }
  }
};


// cct config watcher
class CephContextObs : public md_config_obs_t {
  CephContext *cct;

public:
  CephContextObs(CephContext *cct) : cct(cct) {}

  const char** get_tracked_conf_keys() const {
    static const char *KEYS[] = {
      "enable_experimental_unrecoverable_data_corrupting_features",
      NULL
    };
    return KEYS;
  }

  void handle_conf_change(const md_config_t *conf,
                          const std::set <std::string> &changed) {
    ceph_spin_lock(&cct->_feature_lock);
    get_str_set(conf->enable_experimental_unrecoverable_data_corrupting_features,
		cct->_experimental_features);
    ceph_spin_unlock(&cct->_feature_lock);
    if (!cct->_experimental_features.empty())
      lderr(cct) << "WARNING: the following dangerous and experimental features are enabled: "
		 << cct->_experimental_features << dendl;
  }
};

bool CephContext::check_experimental_feature_enabled(std::string feat)
{
  ceph_spin_lock(&_feature_lock);
  bool enabled = _experimental_features.count(feat);
  ceph_spin_unlock(&_feature_lock);

  if (enabled) {
    lderr(this) << "WARNING: experimental feature '" << feat << "' is enabled" << dendl;
    lderr(this) << "Please be aware that this feature is experimental, untested," << dendl;
    lderr(this) << "unsupported, and may result in data corruption, data loss," << dendl;
    lderr(this) << "and/or irreparable damage to your cluster.  Do not use" << dendl;
    lderr(this) << "feature with important data." << dendl;
  } else {
    lderr(this) << "*** experimental feature '" << feat << "' is not enabled ***" << dendl;
    lderr(this) << "This feature is marked as experimental, which means it" << dendl;
    lderr(this) << " - is untested" << dendl;
    lderr(this) << " - is unsupported" << dendl;
    lderr(this) << " - may corrupt your data" << dendl;
    lderr(this) << " - may break your cluster is an unrecoverable fashion" << dendl;
    lderr(this) << "To enable this feature, add this to your ceph.conf:" << dendl;
    lderr(this) << "  enable experimental unrecoverable data corrupting features = " << feat << dendl;
  }
  return enabled;
}

// perfcounter hooks

class CephContextHook : public AdminSocketHook {
  CephContext *m_cct;

public:
  CephContextHook(CephContext *cct) : m_cct(cct) {}

  bool call(std::string command, cmdmap_t& cmdmap, std::string format,
	    bufferlist& out) {
    m_cct->do_command(command, cmdmap, format, &out);
    return true;
  }
};

void CephContext::do_command(std::string command, cmdmap_t& cmdmap,
			     std::string format, bufferlist *out)
{
  Formatter *f = Formatter::create(format, "json-pretty", "json-pretty");
  stringstream ss;
  for (cmdmap_t::iterator it = cmdmap.begin(); it != cmdmap.end(); ++it) {
    if (it->first != "prefix") {
      ss << it->first  << ":" << cmd_vartype_stringify(it->second) << " ";
    }
  }
  lgeneric_dout(this, 1) << "do_command '" << command << "' '"
			 << ss.str() << dendl;
  if (command == "perfcounters_dump" || command == "1" ||
      command == "perf dump") {
    _perf_counters_collection->dump_formatted(f, false);
  }
  else if (command == "perfcounters_schema" || command == "2" ||
    command == "perf schema") {
    _perf_counters_collection->dump_formatted(f, true);
  }
  else if (command == "perf reset") {
    std::string var;
    if (!cmd_getval(this, cmdmap, "var", var)) {
      f->dump_string("error", "syntax error: 'perf reset <var>'");
    } else {
     if(!_perf_counters_collection->reset(var))
        f->dump_stream("error") << "Not find: " << var;
    }
  }
  else {
    string section = command;
    boost::replace_all(section, " ", "_");
    f->open_object_section(section.c_str());
    if (command == "config show") {
      _conf->show_config(f);
    }
    else if (command == "config set") {
      std::string var;
      std::vector<std::string> val;

      if (!(cmd_getval(this, cmdmap, "var", var)) ||
          !(cmd_getval(this, cmdmap, "val", val))) {
        f->dump_string("error", "syntax error: 'config set <var> <value>'");
      } else {
	// val may be multiple words
	string valstr = str_join(val, " ");
        int r = _conf->set_val(var.c_str(), valstr.c_str());
        if (r < 0) {
          f->dump_stream("error") << "error setting '" << var << "' to '" << valstr << "': " << cpp_strerror(r);
        } else {
          ostringstream ss;
          _conf->apply_changes(&ss);
          f->dump_string("success", ss.str());
        }
      }
    } else if (command == "config get") {
      std::string var;
      if (!cmd_getval(this, cmdmap, "var", var)) {
	f->dump_string("error", "syntax error: 'config get <var>'");
      } else {
	char buf[4096];
	memset(buf, 0, sizeof(buf));
	char *tmp = buf;
	int r = _conf->get_val(var.c_str(), &tmp, sizeof(buf));
	if (r < 0) {
	    f->dump_stream("error") << "error getting '" << var << "': " << cpp_strerror(r);
	} else {
	    f->dump_string(var.c_str(), buf);
	}
      }
    } else if (command == "config diff") {
      md_config_t def_conf;
      def_conf.set_val("cluster", _conf->cluster);
      def_conf.name = _conf->name;
      def_conf.set_val("host", _conf->host);
      def_conf.apply_changes(NULL);

      map<string,pair<string,string> > diff;
      set<string> unknown;
      def_conf.diff(_conf, &diff, &unknown);
      f->open_object_section("diff");

      f->open_object_section("current");
      for (map<string,pair<string,string> >::iterator p = diff.begin();
           p != diff.end(); ++p) {
        f->dump_string(p->first.c_str(), p->second.second);
      }
      f->close_section(); // current
      f->open_object_section("defaults");
      for (map<string,pair<string,string> >::iterator p = diff.begin();
           p != diff.end(); ++p) {
        f->dump_string(p->first.c_str(), p->second.first);
      }
      f->close_section(); // defaults
      f->close_section(); // diff

      f->open_array_section("unknown");
      for (set<string>::iterator p = unknown.begin();
           p != unknown.end(); ++p) {
        f->dump_string("option", *p);
      }
      f->close_section(); // unknown
    } else if (command == "log flush") {
      _log->flush();
    }
    else if (command == "log dump") {
      _log->dump_recent();
    }
    else if (command == "log reopen") {
      _log->reopen_log_file();
    }
    else {
      assert(0 == "registered under wrong command?");    
    }
    f->close_section();
  }
  f->flush(*out);
  delete f;
  lgeneric_dout(this, 1) << "do_command '" << command << "' '" << ss.str()
		         << "result is " << out->length() << " bytes" << dendl;
}


CephContext::CephContext(uint32_t module_type_)
  : nref(1),
    _conf(new md_config_t()),
    _log(NULL),
    _module_type(module_type_),
    _service_thread(NULL),
    _log_obs(NULL),
    _admin_socket(NULL),
    _perf_counters_collection(NULL),
    _perf_counters_conf_obs(NULL),
    _heartbeat_map(NULL),
    _crypto_none(NULL),
    _crypto_aes(NULL)
{
  ceph_spin_init(&_service_thread_lock);
  ceph_spin_init(&_associated_objs_lock);
  ceph_spin_init(&_feature_lock);

  _log = new ceph::log::Log(&_conf->subsys);
  _log->start();

  _log_obs = new LogObs(_log);
  _conf->add_observer(_log_obs);

  _cct_obs = new CephContextObs(this);
  _conf->add_observer(_cct_obs);

  _perf_counters_collection = new PerfCountersCollection(this);
  _admin_socket = new AdminSocket(this);
  _heartbeat_map = new HeartbeatMap(this);

  _admin_hook = new CephContextHook(this);
  _admin_socket->register_command("perfcounters_dump", "perfcounters_dump", _admin_hook, "");
  _admin_socket->register_command("1", "1", _admin_hook, "");
  _admin_socket->register_command("perf dump", "perf dump", _admin_hook, "dump perfcounters value");
  _admin_socket->register_command("perfcounters_schema", "perfcounters_schema", _admin_hook, "");
  _admin_socket->register_command("2", "2", _admin_hook, "");
  _admin_socket->register_command("perf schema", "perf schema", _admin_hook, "dump perfcounters schema");
  _admin_socket->register_command("perf reset", "perf reset name=var,type=CephString", _admin_hook, "perf reset <name>: perf reset all or one perfcounter name");
  _admin_socket->register_command("config show", "config show", _admin_hook, "dump current config settings");
  _admin_socket->register_command("config set", "config set name=var,type=CephString name=val,type=CephString,n=N",  _admin_hook, "config set <field> <val> [<val> ...]: set a config variable");
  _admin_socket->register_command("config get", "config get name=var,type=CephString", _admin_hook, "config get <field>: get the config value");
  _admin_socket->register_command("config diff",
      "config diff", _admin_hook,
      "dump diff of current config and default config");
  _admin_socket->register_command("log flush", "log flush", _admin_hook, "flush log entries to log file");
  _admin_socket->register_command("log dump", "log dump", _admin_hook, "dump recent log entries to log file");
  _admin_socket->register_command("log reopen", "log reopen", _admin_hook, "reopen log file");

  _crypto_none = new CryptoNone;
  _crypto_aes = new CryptoAES;
}

CephContext::~CephContext()
{
  join_service_thread();

  for (map<string, AssociatedSingletonObject*>::iterator it = _associated_objs.begin();
       it != _associated_objs.end(); ++it)
    delete it->second;

  if (_conf->lockdep) {
    lockdep_unregister_ceph_context(this);
  }

  _admin_socket->unregister_command("perfcounters_dump");
  _admin_socket->unregister_command("perf dump");
  _admin_socket->unregister_command("1");
  _admin_socket->unregister_command("perfcounters_schema");
  _admin_socket->unregister_command("perf schema");
  _admin_socket->unregister_command("2");
  _admin_socket->unregister_command("perf reset");
  _admin_socket->unregister_command("config show");
  _admin_socket->unregister_command("config set");
  _admin_socket->unregister_command("config get");
  _admin_socket->unregister_command("config diff");
  _admin_socket->unregister_command("log flush");
  _admin_socket->unregister_command("log dump");
  _admin_socket->unregister_command("log reopen");
  delete _admin_hook;
  delete _admin_socket;

  delete _heartbeat_map;

  delete _perf_counters_collection;
  _perf_counters_collection = NULL;

  delete _perf_counters_conf_obs;
  _perf_counters_conf_obs = NULL;

  _conf->remove_observer(_log_obs);
  delete _log_obs;
  _log_obs = NULL;

  _conf->remove_observer(_cct_obs);
  delete _cct_obs;
  _cct_obs = NULL;

  _log->stop();
  delete _log;
  _log = NULL;

  delete _conf;
  ceph_spin_destroy(&_service_thread_lock);
  ceph_spin_destroy(&_associated_objs_lock);
  ceph_spin_destroy(&_feature_lock);

  delete _crypto_none;
  delete _crypto_aes;
}

void CephContext::start_service_thread()
{
  ceph_spin_lock(&_service_thread_lock);
  if (_service_thread) {
    ceph_spin_unlock(&_service_thread_lock);
    return;
  }
  _service_thread = new CephContextServiceThread(this);
  _service_thread->create();
  ceph_spin_unlock(&_service_thread_lock);

  // make logs flush on_exit()
  if (_conf->log_flush_on_exit)
    _log->set_flush_on_exit();

  // Trigger callbacks on any config observers that were waiting for
  // it to become safe to start threads.
  _conf->set_val("internal_safe_to_start_threads", "true");
  _conf->call_all_observers();

  // start admin socket
  if (_conf->admin_socket.length())
    _admin_socket->init(_conf->admin_socket);
}

void CephContext::reopen_logs()
{
  ceph_spin_lock(&_service_thread_lock);
  if (_service_thread)
    _service_thread->reopen_logs();
  ceph_spin_unlock(&_service_thread_lock);
}

void CephContext::join_service_thread()
{
  ceph_spin_lock(&_service_thread_lock);
  CephContextServiceThread *thread = _service_thread;
  if (!thread) {
    ceph_spin_unlock(&_service_thread_lock);
    return;
  }
  _service_thread = NULL;
  ceph_spin_unlock(&_service_thread_lock);

  thread->exit_thread();
  thread->join();
  delete thread;
}

uint32_t CephContext::get_module_type() const
{
  return _module_type;
}

PerfCountersCollection *CephContext::get_perfcounters_collection()
{
  return _perf_counters_collection;
}

AdminSocket *CephContext::get_admin_socket()
{
  return _admin_socket;
}

CryptoHandler *CephContext::get_crypto_handler(int type)
{
  switch (type) {
  case CEPH_CRYPTO_NONE:
    return _crypto_none;
  case CEPH_CRYPTO_AES:
    return _crypto_aes;
  default:
    return NULL;
  }
}
