/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include <sys/types.h>
#include <sys/stat.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <malloc.h>
#include <dirent.h>
#include <assert.h>
#include <fcntl.h>


#include <netinet/in.h>
#include <net/net.h>
#include <net/netctl.h>


#include <sysmodule/sysmodule.h>
#include <psl1ght/lv2.h>
#include <psl1ght/lv2/spu.h>
#include <lv2/process.h>

#include <rtc.h>

#include "arch/threads.h"
#include "arch/atomic.h"
#include "arch/arch.h"
#include "main.h"
#include "service.h"
#include "misc/callout.h"
#include "misc/md5.h"
#include "misc/str.h"
#include "text/text.h"
#include "notifications.h"
#include "fileaccess/fileaccess.h"
#include "arch/halloc.h"
#include "ps3.h"

extern int sysUtilGetSystemParamInt(int, int *);

static void replace_gamefile(const char *name);

// #define EMERGENCY_EXIT_THREAD
static uint64_t ticks_per_us;

static callout_t memlogger;


static uint64_t
mftb(void)
{
  uint64_t ret;
  asm volatile ("1: mftb %[tmp];       "
		"   cmpwi 7, %[tmp], 0;"
		"   beq-  7, 1b;       "
		: [tmp] "=r" (ret):: "cr7");
  return ret;
}

static prop_t *sysprop;
static prop_t *memprop;
static prop_t *tempprop;
static prop_t *hddprop;

#define LOW_MEM_LOW_WATER  5 * 1024 * 1024
#define LOW_MEM_HIGH_WATER 15 * 1024 * 1024

extern void vm_stat_log(void);

static void
memlogger_fn(callout_t *co, void *aux)
{
  static int low_mem_warning;

  callout_arm(&memlogger, memlogger_fn, NULL, 1);

  struct {
    uint32_t total;
    uint32_t avail;
  } meminfo;


  Lv2Syscall1(352, (uint64_t) &meminfo);

  prop_set_int(prop_create(memprop, "systotal"), meminfo.total / 1024);
  prop_set_int(prop_create(memprop, "sysfree"), meminfo.avail / 1024);

  struct mallinfo mi = mallinfo();
  prop_set_int(prop_create(memprop, "arena"), (mi.hblks + mi.arena) / 1024);
  prop_set_int(prop_create(memprop, "unusedChunks"), mi.ordblks);
  prop_set_int(prop_create(memprop, "activeMem"), mi.uordblks / 1024);
  prop_set_int(prop_create(memprop, "inactiveMem"), mi.fordblks / 1024);


  if(gconf.enable_mem_debug) {
    vm_stat_log();

    TRACE(TRACE_DEBUG, "MEM",
          "SysTotal: %d kB, "
          "SysFree: %d kB, "
          "Memory Used: %d kB, "
          "Fragments: %d, "
          "Inactive: %d kB",
          meminfo.total / 1024,
          meminfo.avail / 1024,
          mi.uordblks / 1024,
          mi.ordblks,
          mi.fordblks / 1024);
  }

  if(meminfo.avail < LOW_MEM_LOW_WATER && !low_mem_warning) {
    low_mem_warning = 1;
    notify_add(NULL, NOTIFY_ERROR, NULL, 5,
	       _("System is low on memory (%d kB RAM available)"),
	       meminfo.avail / 1024);
  }

  if(meminfo.avail > LOW_MEM_HIGH_WATER)
    low_mem_warning = 0;

  uint32_t temp;
  Lv2Syscall2(383, 0, (uint64_t)&temp); // CPU temp
  prop_set(tempprop, "cpu", PROP_SET_INT, temp >> 24);
  Lv2Syscall2(383, 1, (uint64_t)&temp); // RSX temp
  prop_set(tempprop, "gpu", PROP_SET_INT, temp >> 24);

  uint64_t size, avail;

  int r = Lv2Syscall3(840,
                      (uint64_t)"/dev_hdd0/game/HTSS00003/",
                      (uint64_t)&size,
                      (uint64_t)&avail);

  if(!r) {
    prop_set(hddprop, "avail", PROP_SET_FLOAT, avail / 1000000000.0);
    prop_set(hddprop, "size", PROP_SET_FLOAT,   size / 1000000000.0);
  }

}




/**
 *
 */
int64_t
arch_get_ts(void)
{
  return mftb() / ticks_per_us;
}


/**
 *
 */
typedef struct rootfsnode {
  LIST_ENTRY(rootfsnode) link;
  char *name;
  service_t *service;
  int mark;
} rootfsnode_t;
 
static LIST_HEAD(, rootfsnode) rootfsnodes;

/**
 *
 */
static void
scan_root_fs(callout_t *co, void *aux)
{
  struct dirent *d;
  struct stat st;
  DIR *dir;
  char fname[32];
  char dpyname[32];
  rootfsnode_t *rfn, *next;

  LIST_FOREACH(rfn, &rootfsnodes, link)
    rfn->mark = 1;

  callout_arm(co, scan_root_fs, NULL, 1);

  if((dir = opendir("/")) == NULL)
    return;

  while((d = readdir(dir)) != NULL) {
    if(strncmp(d->d_name, "dev_", strlen("dev_")))
      continue;
    if(!strncmp(d->d_name, "dev_flash", strlen("dev_flash")))
      continue;

    snprintf(fname, sizeof(fname), "/%s", d->d_name);
    if(stat(fname, &st))
      continue;

    if((st.st_mode & S_IFMT) != S_IFDIR)
      continue;

    LIST_FOREACH(rfn, &rootfsnodes, link)
      if(!strcmp(rfn->name, d->d_name))
	break;

    if(rfn == NULL) {
      rfn = malloc(sizeof(rootfsnode_t));
      rfn->name = strdup(d->d_name);

      snprintf(fname, sizeof(fname), "file:///%s", d->d_name);

      const char *name = d->d_name;
      const char *type = "other";
      const char *desc;
      if(!strcmp(name, "dev_hdd0")) {
	name = "PS3 HDD";
	desc = "Internal Harddrive";
      } else if(!strncmp(name, "dev_usb", strlen("dev_usb"))) {
	snprintf(dpyname, sizeof(dpyname), "USB Drive %d",
		 atoi(name + strlen("dev_usb")));
	type = "usb";
	name = dpyname;
	desc = "External Harddrive";
      }
      else if(!strcmp(name, "dev_bdvd") ||
	      !strcmp(name, "dev_ps2disc")) {
	name = "BluRay Drive";
	type = "bluray";
      }

      rfn->service = service_create_managed(name, name, fname, type, NULL, 0, 1,
					    SVC_ORIGIN_MEDIA, name);
      LIST_INSERT_HEAD(&rootfsnodes, rfn, link);
    }
    rfn->mark = 0;
  }
  closedir(dir);
  
  for(rfn = LIST_FIRST(&rootfsnodes); rfn != NULL; rfn = next) {
    next = LIST_NEXT(rfn, link);
    if(!rfn->mark)
      continue;

    LIST_REMOVE(rfn, link);
    service_destroy(rfn->service);
    free(rfn->name);
    free(rfn);
  }

}





static int trace_fd = -1;
static struct sockaddr_in log_server;

void
my_trace(const char *fmt, ...)
{
  char msg[1000];
  va_list ap;

  if(trace_fd == -2)
    return;

  if(trace_fd == -1) {
    int port = 4000;
    char *p;

    log_server.sin_len = sizeof(log_server);
    log_server.sin_family = AF_INET;
    
    snprintf(msg, sizeof(msg), "%s", SHOWTIME_DEFAULT_LOGTARGET);
    p = strchr(msg, ':');
    if(p != NULL) {
      *p++ = 0;
      port = atoi(p);
    }
    log_server.sin_port = htons(port);
    if(inet_pton(AF_INET, msg, &log_server.sin_addr) != 1) {
      trace_fd = -2;
      return;
    }

    trace_fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(trace_fd == -1)
      return;
  }

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  sendto(trace_fd, msg, strlen(msg), 0,
	 (struct sockaddr*)&log_server, sizeof(log_server));
}

/**
 *
 */
void
trace_arch(int level, const char *prefix, const char *str)
{
}


#ifdef EMERGENCY_EXIT_THREAD
/**
 *
 */
static void *
emergency_thread(void *aux)
{
  struct sockaddr_in si = {0};
  int s;
  int one = 1, r;
  struct pollfd fds;

  s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));
  si.sin_family = AF_INET;
  si.sin_port = htons(31337);

  if(bind(s, (struct sockaddr *)&si, sizeof(struct sockaddr_in)) == -1) {
    TRACE(TRACE_ERROR, "ER", "Unable to bind");
    return NULL;
  }

  fds.fd = s;
  fds.events = POLLIN;

  while(1) {
    r = poll(&fds, 1 , 1000);
    if(r > 0 && fds.revents & POLLIN)
      exit(0);
  }
  return NULL;
}
#endif


/**
 *
 */
static void
ps3_early_init(int argc, char **argv)
{
  char buf[PATH_MAX], *x;

  hts_mutex_init(&thread_info_mutex);


  netInitialize();
  netCtlInit();

  extern void *__netMemory;
  ticks_per_us = Lv2Syscall0(147) / 1000000;
  my_trace("Ticks per µs = %ld  netlib memory at:%p\n",
           ticks_per_us, __netMemory);


  if(argc == 0) {
    my_trace(APPNAMEUSER" starting from ???\n");
    return;
  }
  my_trace(APPNAMEUSER" starting from %s\n", argv[0]);
  gconf.binary = strdup(argv[0]);

  snprintf(buf, sizeof(buf), "%s", argv[0]);
  x = strrchr(buf, '/');
  if(x == NULL) {
    my_trace(APPNAMEUSER" starting but argv[0] seems invalid");
    exit(0);
  }
  x++;
  *x = 0;
  gconf.dirname = strdup(buf);
  strcpy(x, "settings");
  gconf.persistent_path = strdup(buf);
  strcpy(x, "cache");
  gconf.cache_path = strdup(buf);
  SysLoadModule(SYSMODULE_RTC);

  thread_info_t *ti = malloc(sizeof(thread_info_t));
  snprintf(ti->name, sizeof(ti->name), "main");
  sys_ppu_thread_get_id(&ti->id);
  hts_mutex_lock(&thread_info_mutex);
  LIST_INSERT_HEAD(&threads, ti, link);
  hts_mutex_unlock(&thread_info_mutex);

#ifdef EMERGENCY_EXIT_THREAD
  r = sys_ppu_thread_create(&tid, (void *)emergency_thread, 0,
				2, 16384, 0, (char *)"emergency");
  if(r) {
    my_trace("Failed to create emergency thread: %x", r);
    exit(0);
  }
#endif

  my_trace("The binary is: %s\n", gconf.binary);
}


/**
 *
 */
uint64_t
arch_get_seed(void)
{
  return mftb();
}



/**
 *
 */
static void
preload_fonts(void)
{
/*
  freetype_load_default_font("file:///dev_flash/data/font/SCE-PS3-VR-R-LATIN2.TTF", 100);
  freetype_load_default_font("file:///dev_flash/data/font/SCE-PS3-NR-R-JPN.TTF", 101);
  freetype_load_default_font("file:///dev_flash/data/font/SCE-PS3-YG-R-KOR.TTF", 102);
  freetype_load_default_font("file:///dev_flash/data/font/SCE-PS3-DH-R-CGB.TTF", 103);
  freetype_load_default_font("file:///dev_flash/data/font/SCE-PS3-CP-R-KANA.TTF",104);
*/
  freetype_load_default_font("file:///dev_hdd0/game/HTSS00003/USRDIR/FONT.TTF",100);
}

const char *
arch_get_system_type(void)
{
  return "PS3";
}




/**
 *
 */
void *
halloc(size_t size)
{
#define ROUND_UP(p, round) ((p + (round) - 1) & ~((round) - 1))

  size_t allocsize = ROUND_UP(size, 64*1024);
  u32 taddr;

  if(Lv2Syscall3(348, allocsize, 0x200, (u64)&taddr))
    panic("halloc(%d) failed", (int)size);

  return (void *)(uint64_t)taddr;
}


/**
 *
 */
void
hfree(void *ptr, size_t size)
{
  Lv2Syscall1(349, (uint64_t)ptr);
}


/**
 *
 */
static void
set_device_id(void)
{
  uint8_t digest[16];

  md5_decl(ctx);
  md5_init(ctx);

  union net_ctl_info info;

  if(!netCtlGetInfo(NET_CTL_INFO_ETHER_ADDR, &info))
    md5_update(ctx, info.ether_addr.data, 6);

  md5_final(ctx, digest);
  bin2hex(gconf.device_id, sizeof(gconf.device_id), digest, sizeof(digest));

  uint8_t platinfo[0x18] = {0};
  int v = Lv2Syscall1(387, (uint64_t)platinfo);
  if(v == 0) {
    snprintf(gconf.os_info, sizeof(gconf.os_info), "PS3 %d.%d%x",
             platinfo[0],
             platinfo[1],
             platinfo[2]);

    strcpy(gconf.device_type, "PS3 ");
    memcpy(gconf.device_type + 4, platinfo+8, 8);
  }

}


/**
 *
 */
void
arch_localtime(const time_t *now, struct tm *tm)
{
  rtc_datetime dt;
  rtc_tick utc, local;

  rtc_convert_time_to_datetime(&dt, *now);
  rtc_convert_datetime_to_tick(&dt, &utc);
  rtc_convert_utc_to_localtime(&utc, &local);
  rtc_convert_tick_to_datetime(&dt, &local);

  memset(tm, 0, sizeof(struct tm));

  tm->tm_year = dt.year - 1900;
  tm->tm_mon  = dt.month - 1;
  tm->tm_mday = dt.day;
  tm->tm_hour = dt.hour;
  tm->tm_min  = dt.minute;
  tm->tm_sec  = dt.second;
}


void
arch_exit(void)
{
  if(gconf.exit_code == APP_EXIT_STANDBY) {
    unlink("/dev_hdd0/tmp/turnoff");
    Lv2Syscall3(379, 0x100, 0, 0 );
  }
  if(gconf.exit_code == APP_EXIT_RESTART)
    sysProcessExitSpawn2(gconf.binary, 0, 0, 0, 0, 1200, 0x70);

  exit(gconf.exit_code);
}


static  sys_event_queue_t crash_event_queue;

static void *
exec_catcher(void *aux)
{
  sys_event_t event;

  while(1) {
    int ret = sys_event_queue_receive(crash_event_queue, &event,
				      100 * 1000 * 1000);

    if(ret)
      continue;
    thread_dump();

    exit(0);
  }
  return NULL;
}

typedef struct sys_event_queue_attr {
  uint32_t attr_protocol;
  int type;
  char name[8];
} sys_event_queue_attribute_t;

/**
 *
 */
int
main(int argc, char **argv)
{
  int v;
  ps3_early_init(argc, argv);

  gconf.concurrency = 2;
  gconf.can_standby = 1;
  gconf.trace_level = TRACE_DEBUG;
  gconf.disable_http_reuse = 1;

  load_syms();

  my_trace("The binary is: %s\n", gconf.binary);
  set_device_id();

  // Time format
  if(!sysUtilGetSystemParamInt(0x115, &v)) {
    gconf.time_format_system = v ? TIME_FORMAT_24 : TIME_FORMAT_12;
  }

  main_init();

  sysprop = prop_create(prop_get_global(), "system");
  memprop = prop_create(sysprop, "mem");
  tempprop = prop_create(sysprop, "temp");
  hddprop = prop_create(sysprop, "hdd");
  callout_arm(&memlogger, memlogger_fn, NULL, 1);

#if ENABLE_PS3_VDEC
  TRACE(TRACE_DEBUG, "SPU", "Initializing SPUs");
  lv2SpuInitialize(6, 0);
#endif

  TRACE(TRACE_DEBUG, "SYSTEM", "System version %s", gconf.os_info);

  sys_event_queue_attribute_t attr = {
    .attr_protocol = 2,
    .type = 1,
  };
  
  int r;
  r = Lv2Syscall4(128, (intptr_t)&crash_event_queue,(intptr_t)&attr, 0, 64);

  r = Lv2Syscall1(944, crash_event_queue);
  sys_ppu_thread_t tid;

  r = sys_ppu_thread_create(&tid, (void *)exec_catcher, 0,
			    2, 16384, 0, (char *)"execcatcher");

  preload_fonts();

  static callout_t co;
  scan_root_fs(&co, NULL);

  replace_gamefile("PARAM.SFO");
  replace_gamefile("ICON0.PNG");

  extern void glw_ps3_start(void);
  glw_ps3_start();

  main_fini();

  arch_exit();


}


int
arch_pipe(int pipefd[2])
{
  int fd;
  struct sockaddr_in si = {0};
  socklen_t addrlen = sizeof(struct sockaddr_in);

  if((fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
    return -1;

  si.sin_family = AF_INET;
  si.sin_addr.s_addr = htonl(0x7f000001);

  if(bind(fd, (struct sockaddr *)&si, sizeof(struct sockaddr_in))) {
    close(fd);
    return -1;
  }

  if(getsockname(fd, (struct sockaddr *)&si, &addrlen)) {
    close(fd);
    return -1;
  }

  listen(fd, 1);

  if((pipefd[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
    close(fd);
    return -1;
  }

  if(connect(pipefd[0], (struct sockaddr *)&si, addrlen) < 0) {
    close(fd);
    close(pipefd[0]);
    return -1;
  }
  
  addrlen = sizeof(struct sockaddr_in);
  pipefd[1] = accept(fd, (struct sockaddr *)&si, &addrlen) ;
  close(fd);
  if(pipefd[1] == -1) {
    close(pipefd[0]);
    return -1;
  }

  return 0;
}


/**
 *
 */
static void
overwrite_gamefile(int fd, const void *buf, int size, const char *name)
{
  if(lseek(fd, 0, SEEK_SET) != 0)
    return;
  if(write(fd, buf, size) != size)
    return;
  ftruncate(fd, size);
  TRACE(TRACE_INFO, "SYSTEM", "%s updated", name);
}


/**
 *
 */
static void
replace_gamefile(const char *name)
{
  char path[256];
  snprintf(path, sizeof(path), "%s/%s", app_dataroot(), name);

  fa_handle_t *fh = fa_open(path, NULL, 0);

  if(fh == NULL) {
    return;
  }
  int size = fa_fsize(fh);
  char *buf = malloc(size);

  int r = fa_read(fh, buf, size);
  fa_close(fh);
  if(r != size) {
    free(buf);
    return;
  }

  snprintf(path, sizeof(path), "%s/../%s", gconf.dirname, name);
  int fd = open(path, O_RDWR);

  if(fd != -1) {
    struct stat st;
    if(fstat(fd, &st) != -1) {
      if(st.st_size == size) {
        char *buf2 = malloc(st.st_size);
        if(buf2 != NULL) {
          int x = read(fd, buf2, st.st_size);
          if(x == st.st_size) {
            if(memcmp(buf, buf2, x)) {
              overwrite_gamefile(fd, buf, size, name);
            }
          }
          free(buf2);
        }
      } else {
        overwrite_gamefile(fd, buf, size, name);
      }
    }
    close(fd);
  }
  free(buf);
}
