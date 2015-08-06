#include <errno.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "dokan/libcephfs.h"
#include "common/Clock.h"

static int findfiles(struct ceph_mount_info *cmount, char* file_name)
{
    struct ceph_dir_result *dirp;
    int ret = ceph_opendir(cmount, file_name, &dirp);
    if(ret != 0){
        fprintf(stderr, "ceph_opendir error : %s [%d]\n", file_name, ret);
        return -1;
    }
    
    fprintf(stderr, "ceph_opendir OK: %s\n", file_name);
    
    int count = 0;
    while(1)
    {
        struct dirent result;
        struct stat stbuf;
        int stmask;
        
        ret = ceph_readdirplus_r(cmount, dirp, &result, &stbuf, &stmask);
        if(ret==0)
            break;
        if(ret<0){
            fprintf(stderr, "ceph_readdirplus_r error [%s][ret=%d]\n", file_name, ret);
            return ret;
        }
        fprintf(stderr, "====ceph_readdir [%d][%s]\n", ++count, result.d_name);
		
		if(S_ISDIR(stbuf.st_mode))
			printf("This is a Directory.............\n");
		if(S_ISREG(stbuf.st_mode))
			printf("This is a Regular File,,,,,,,,,,\n");
		
		printf("st_uid=%d\n", stbuf.st_uid);
		printf("st_gid=%d\n", stbuf.st_gid);
		printf("st_size=%d\n", stbuf.st_size);
		
		//printf("sizeof(stbuf.st_atim) is [%d][%d][%d]\n",
		// sizeof(stbuf.st_atim), sizeof(stbuf.st_atim.tv_sec),
		// sizeof(stbuf.st_atim.tv_nsec));
		//
		//struct tm *tm_time;
		//tm_time = localtime(&stbuf.st_atim.tv_sec);
		//printf("atime = %04d-%02d-%02d %02d:%02d:%02d\n", 
		//	tm_time->tm_year+1900, tm_time->tm_mon, tm_time->tm_mday,
		//	tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec);
        //
		//tm_time = localtime(&stbuf.st_mtim.tv_sec);
		//printf("mtime = %04d-%02d-%02d %02d:%02d:%02d\n", 
		//	tm_time->tm_year+1900, tm_time->tm_mon, tm_time->tm_mday,
		//	tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec);
		//	
		//tm_time = localtime(&stbuf.st_ctim.tv_sec);
		//printf("ctime = %04d-%02d-%02d %02d:%02d:%02d\n", 
		//	tm_time->tm_year+1900, tm_time->tm_mon, tm_time->tm_mday,
		//	tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec);
    }
    
    fprintf(stderr, "ceph_readdir END[%d]\n", count);
    
    ret = ceph_closedir(cmount, dirp);
}

int main(int argc, char* argv[])
{
    if(argc != 2)
    {
    	printf("Usage:%s [ceph.conf]\n", argv[0]);
    	return -1;
    }
    
    WORD VerNum = MAKEWORD(2, 2);
    WSADATA VerData;
    if (WSAStartup(VerNum, &VerData) != 0) {
      fprintf(stderr, "init winsock failed!\n");
      return -1;
    }
    
    utime_t start;
    utime_t end;
    
    start = ceph_clock_now(NULL);
    
    struct ceph_mount_info *cmount;
    ceph_create(&cmount, NULL);
    ceph_conf_read_file(cmount, argv[1]);
    ceph_mount(cmount, "/");
    
    end = ceph_clock_now(NULL);
    printf("ceph_mount time is [%d]\n", end.sec()-start.sec());
    start = ceph_clock_now(NULL);
    
    char path[4096];
    sprintf(path, "/");
    findfiles(cmount, path);
    
    ceph_unmount(cmount);
    
    WSACleanup();
    
    return 0;
}
