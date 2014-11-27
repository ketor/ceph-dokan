#include "include/cephfs/libcephfs.h"
#include <errno.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "common/Clock.h"

#define dout_subsys ceph_subsys_objecter
#undef dout_prefix
#define dout_prefix *_dout << messenger->get_myname() << ".test_cephfs "

#define BUFFER_SIZE 32*1024*1024

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
    
    int ret = ceph_rename(cmount, "3.xls", "2.xls");
    printf("ceph_rename %d\n", ret);
    
//    char c_dir[256];
//    sprintf(c_dir, "/");
//    struct ceph_dir_result *dirp;
//    ceph_opendir(cmount, c_dir, &dirp);
//    
//    while(1)
//    {
//	    struct dirent *result = ceph_readdir(cmount, dirp);
//	    if(result != NULL)
//	    {
//	    	printf("sizeof(dirent) is %d\n", sizeof(struct dirent));
//	    	printf("d_name is [%s]\n", result->d_name);
//	    }else break;
//	    
//		char dir_item_name[MAX_PATH];
//		sprintf(dir_item_name, "/%s", result->d_name);
//		struct stat_ceph stbuf;
//		int ret = ceph_stat(cmount, dir_item_name, &stbuf);
//		if(ret){
//			fprintf(stderr, "ceph_stat error [%s]\n", dir_item_name);
//			continue;
//		}
//		
//		if(S_ISDIR(stbuf.st_mode))
//			printf("This is a Directory.............\n");
//		if(S_ISREG(stbuf.st_mode))
//			printf("This is a Regular File,,,,,,,,,,\n");
//		
//		printf("st_uid=%d\n", stbuf.st_uid);
//		printf("st_gid=%d\n", stbuf.st_gid);
//		printf("st_size=%d\n", stbuf.st_size);
//		
//		printf("sizeof(stbuf.st_atim) is [%d][%d][%d]\n",
//		 sizeof(stbuf.st_atim), sizeof(stbuf.st_atim.tv_sec),
//		 sizeof(stbuf.st_atim.tv_nsec));
//		
//		struct tm *tm_time;
//		tm_time = localtime(&stbuf.st_atim.tv_sec);
//		printf("atime = %04d-%02d-%02d %02d:%02d:%02d\n", 
//			tm_time->tm_year+1900, tm_time->tm_mon, tm_time->tm_mday,
//			tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec);
//
//		tm_time = localtime(&stbuf.st_mtim.tv_sec);
//		printf("mtime = %04d-%02d-%02d %02d:%02d:%02d\n", 
//			tm_time->tm_year+1900, tm_time->tm_mon, tm_time->tm_mday,
//			tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec);
//			
//		tm_time = localtime(&stbuf.st_ctim.tv_sec);
//		printf("ctime = %04d-%02d-%02d %02d:%02d:%02d\n", 
//			tm_time->tm_year+1900, tm_time->tm_mon, tm_time->tm_mday,
//			tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec);
//	}
//    
//    int fd = ceph_open(cmount, "a.txt", O_RDWR, 0755);
//    if(fd<0){
//    	printf("ceph_open error \n");
//    	return -1;
//    }
//    
//    char buf[1024];
//    int ret = ceph_read(cmount, fd, buf, 1024, 0);
//    printf("[%s]\n", buf);

//	ceph_close(cmount, fd);
	
//    char c_dir[256];
//    sprintf(c_dir, "/readdir_r_cb_tests_%d", getpid());
//    struct ceph_dir_result *dirp;
//    ceph_mkdirs(cmount, c_dir, 0777);
//    ceph_opendir(cmount, c_dir, &dirp);
//
//    end = ceph_clock_now(NULL);
//    printf("ceph_mkdirs time is [%d]\n", end.sec()-start.sec());
//    start = ceph_clock_now(NULL);
//
//    // dir is empty, check that it only contains . and ..
//    int buflen = 100;
//    char *buf = new char[buflen];
//    // . is 2, .. is 3 (for null terminators)
//    ceph_getdnames(cmount, dirp, buf, buflen);
//    ceph_closedir(cmount, dirp);
//
//    
//    end = ceph_clock_now(NULL);
//    printf("ceph_getdnames time is [%d]\n", end.sec()-start.sec());
//    start = ceph_clock_now(NULL);
//
//    char c_file[256];
//    sprintf(c_file, "/readdir_r_cb_tests_%d/foo", getpid());
//    int fd = ceph_open(cmount, c_file, O_CREAT|O_RDWR, 0777);
//    printf("ceph_write [fd=%d]\n", fd);
//    
//    char *buffer;
//    buffer = (char*)malloc(BUFFER_SIZE);
//    if(buffer==NULL)
//    {
//        printf("malloc errno\n");
//        return -1;
//    }
//
//    end = ceph_clock_now(NULL);
//    printf("malloc time is [%d]\n", end.sec()-start.sec());
//    start = ceph_clock_now(NULL);
//
//    memset(buffer, 0, BUFFER_SIZE);
//    uint64_t offset = 0;
//    for(int i=0;i<32;i++)
//    {
//        int ret = 0;
//        while(ret < BUFFER_SIZE)
//        {
//            ret = ceph_write(cmount, fd, buffer, BUFFER_SIZE, offset);
//            if(ret<0)
//            {
//                printf("ceph_write error [%d %10d][ret:%d]\n", i/1024, i%1024, ret);
//                exit(ret);
//            }
//        }
//        offset+=BUFFER_SIZE;
//        //ceph_fsync(cmount, fd, 0);
//        printf("ceph_write [%d][%10d][offset:%lld][ret:%d]\n", i/1024, i%1024, offset, ret);
//    }
//    
//    end = ceph_clock_now(NULL);
//    printf("ceph_write time is [%d]\n", end.sec()-start.sec());
//    printf("Write Speed is [%d] MB/s\n", offset/1024/1024/(end.sec()-start.sec()));
//    start = ceph_clock_now(NULL);
//    
//    printf("offset=%ld\n", offset);
//    ceph_close(cmount, fd);
//
//    end = ceph_clock_now(NULL);
//    printf("ceph_close time is [%d]\n", end.sec()-start.sec());
//    start = ceph_clock_now(NULL);
//    
//    printf("ceph_unmount\n");
//    ceph_unmount(cmount);
//    
//    end = ceph_clock_now(NULL);
//    printf("ceph_unmount time is [%d]\n", end.sec()-start.sec());
//    start = ceph_clock_now(NULL);
//    
//    printf("ceph_release\n");
//    ceph_release(cmount);
    
    WSACleanup();
    
    return 0;
}
