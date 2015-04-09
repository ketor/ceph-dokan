#Makefile by Ketor 
CC        = gcc -D__USE_FILE_OFFSET64 -DHAVE_CONFIG_H -I. -D__CEPH__ -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE -D__STDC_FORMAT_MACROS -D_GNU_SOURCE -fno-strict-aliasing -fsigned-char -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -g -DPIC 
CPP       = g++ -D__USE_FILE_OFFSET64 -DHAVE_CONFIG_H -I. -D__CEPH__ -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE -D__STDC_FORMAT_MACROS -D_GNU_SOURCE -fno-strict-aliasing -fsigned-char -Wno-invalid-offsetof -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -g -DPIC 

CEPH_INCLUDE = -I./ -I./global
CFLAGS   = $(CEPH_INCLUDE)
CLIBS    = 

%.o:%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

auth/%.o:auth/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

auth/cephx/%.o:auth/cephx/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

auth/none/%.o:auth/none/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

include/%.o:include/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

common/%.o:common/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

crush/%.o:crush/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

mon/%.o:mon/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

osdc/%.o:osdc/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

common/%.o:common/%.c
	$(CC) -c $(CFLAGS) $^ -o $@

crush/%.o:crush/%.c
	$(CC) -c $(CFLAGS) $^ -o $@

mds/%.o:mds/%.c
	$(CC) -c $(CFLAGS) $^ -o $@

dokan/%.o:dokan/%.c
	$(CC) -w -c $(CFLAGS) $^ -o $@

ALL:ceph-dokan.exe

OBJECTS=libcephfs.o global/global_context.o global/global_init.o global/pidfile.o global/signal_handler.o common/types.o common/TextTable.o common/io_priority.o common/hobject.o common/ceph_frag.o common/Readahead.o common/histogram.o include/uuid.o common/ceph_fs.o common/bloom_filter.o common/ceph_hash.o common/ceph_strings.o common/addr_parsing.o common/assert.o  common/BackTrace.o  common/buffer.o  common/ceph_argparse.o  common/ceph_context.o common/lockdep.o common/Clock.o common/ceph_crypto.o  common/code_environment.o  common/common_init.o  common/ConfUtils.o  common/DecayCounter.o  common/dout.o  common/entity_name.o  common/environment.o  common/errno.o  common/fd.o  common/Finisher.o  common/Formatter.o  common/hex.o common/LogEntry.o  common/Mutex.o  common/page.o  common/perf_counters.o  common/PrebufferedStreambuf.o  common/RefCountedObj.o    common/signal.o  common/simple_spin.o  common/snap_types.o  common/str_list.o  common/strtol.o  common/Thread.o  common/Throttle.o  common/Timer.o  common/util.o  common/config.o  common/armor.o common/crc32c.o common/crc32c-intel.o common/TrackedOp.o common/escape.o common/mime.o common/safe_io.o common/sctp_crc32.o common/secret.o common/utf8.o common/LogClient.o common/version.o log/Log.o  log/SubsystemMap.o log/Log.o  log/SubsystemMap.o auth/AuthAuthorizeHandler.o auth/AuthClientHandler.o auth/AuthMethodList.o auth/AuthServiceHandler.o auth/AuthSessionHandler.o auth/Crypto.o auth/KeyRing.o auth/RotatingKeyRing.o auth/none/AuthNoneAuthorizeHandler.o auth/cephx/CephxSessionHandler.o auth/cephx/CephxAuthorizeHandler.o auth/cephx/CephxProtocol.o   auth/cephx/CephxClientHandler.o auth/cephx/CephxServiceHandler.o auth/cephx/CephxKeyServer.o  crush/CrushCompiler.o crush/CrushWrapper.o crush/builder.o crush/crush.o crush/hash.o crush/mapper.o common/hobject.o msg/simple/Accepter.o msg/simple/PipeConnection.o msg/simple/DispatchQueue.o msg/Message.o msg/Messenger.o msg/msg_types.o msg/simple/Pipe.o msg/simple/SimpleMessenger.o osd/HitSet.o osd/OSDMap.o osd/OpRequest.o osd/osd_types.o mon/MonClient.o mon/MonMap.o mon/MonCap.o mds/flock.o mds/MDSMap.o mds/mdstypes.o mds/inode_backtrace.o osdc/Filer.o osdc/Journaler.o osdc/ObjectCacher.o osdc/Objecter.o osdc/Striper.o client/Client.o client/ClientSnapRealm.o client/Dentry.o client/Inode.o client/MetaRequest.o client/MetaSession.o client/Trace.o #include/uuid.o

libcephfs.dll:$(OBJECTS)
	$(CPP) $(CFLAGS) $(CLIBS) -shared -o $@ $^ -lws2_32 -lpthread
	@echo "**************************************************************"
	@echo "MAKE "$@" FINISH"
	@echo "**************************************************************"

test-cephfs.exe:test_cephfs.o libcephfs.dll
	$(CPP) $(CFLAGS) $(CLIBS) -o $@ $^ -lws2_32 -lpthread -unicode -static-libgcc -static-libstdc++
	@echo "**************************************************************"
	@echo "MAKE "$@" FINISH"
	@echo "**************************************************************"

ceph-dokan.exe:dokan/ceph_dokan.o dokan/posix_acl.o dokan/dokan.lib libcephfs.dll
	$(CPP) $(CFLAGS) $(CLIBS) -o $@ $^ -lws2_32 -lpthread -unicode 
	@echo "**************************************************************"
	@echo "MAKE "$@" FINISH"
	@echo "**************************************************************"

clean:
	rm -f $(OBJECTS) dokan/*.o *.o libcephfs.dll ceph-dokan.exe test-cephfs.exe

