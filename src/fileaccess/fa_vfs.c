/*
 *  Copyright (C) 2013 Andreas Öman
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
 */

#include "showtime.h"
#include "fileaccess.h"
#include "misc/fs.h"

#include "fa_proto.h"
#include "fa_vfs.h"

LIST_HEAD(vfs_mapping_list, vfs_mapping);

static struct vfs_mapping_list vfs_mappings;
static hts_mutex_t vfs_mutex;

static const char *READMETXT =
  "This is Showtime's virtual file system\n"
  "\n"
  "To add stuff here go to Settings -> Bookmarks in Showtime's UI\n"
  "and mark items as 'Publised in Virtual File System'\n";

/**
 *
 */
typedef struct vfs_mapping {
  LIST_ENTRY(vfs_mapping) vm_link;
  char *vm_vdir;
  int vm_vdirlen;
  char *vm_prefix;
} vfs_mapping_t;


/**
 *
 */
static int
vm_compar(const vfs_mapping_t *a, const vfs_mapping_t *b)
{
  return strcmp(a->vm_vdir, b->vm_vdir);
}


/**
 *
 */
void
vfs_add_mapping(const char *vdir, const char *prefix)
{
  vfs_mapping_t *vm;
  hts_mutex_lock(&vfs_mutex);

  LIST_FOREACH(vm, &vfs_mappings, vm_link) {
    if(!strcmp(vm->vm_vdir, vdir))
      break;
  }

  if(vm == NULL) {
    vm = calloc(1, sizeof(vfs_mapping_t));
    vm->vm_vdir    = strdup(vdir);
    vm->vm_vdirlen = strlen(vdir);
    LIST_INSERT_SORTED(&vfs_mappings, vm, vm_link, vm_compar);
  }
  mystrset(&vm->vm_prefix, prefix);
  hts_mutex_unlock(&vfs_mutex);
}


/**
 *
 */
void
vfs_del_mapping(const char *vdir)
{
  vfs_mapping_t *vm;

  hts_mutex_lock(&vfs_mutex);
  LIST_FOREACH(vm, &vfs_mappings, vm_link) {
    if(!strcmp(vm->vm_vdir, vdir))
      break;
  }

  if(vm != NULL)
    LIST_REMOVE(vm, vm_link);
  hts_mutex_unlock(&vfs_mutex);

  free(vm->vm_vdir);
  free(vm->vm_prefix);
  free(vm);
}


/**
 *
 */
static vfs_mapping_t *
find_mapping(const char *path, const char **remain)
{
  int plen = strlen(path);
  vfs_mapping_t *vm;

  LIST_FOREACH(vm, &vfs_mappings, vm_link) {
    if(plen < vm->vm_vdirlen)
      continue;

    if(memcmp(path, vm->vm_vdir, vm->vm_vdirlen))
      continue;

    if(path[vm->vm_vdirlen] == 0) {
      *remain = NULL;
      return vm;
    }
    if(path[vm->vm_vdirlen] == '/') {
      *remain = path + vm->vm_vdirlen + 1;
      return vm;
    }
  }
  return NULL;
}


/**
 *
 */
static int
resolve_mapping(const char *path, char *newpath, size_t newpathlen)
{
  const char *remain;
  hts_mutex_lock(&vfs_mutex);

  vfs_mapping_t *vm = find_mapping(path, &remain);
  if(vm == NULL) {
    hts_mutex_unlock(&vfs_mutex);
    return -1;
  }
  if(remain)
    snprintf(newpath, newpathlen, "%s/%s", vm->vm_prefix, remain);
  else
    snprintf(newpath, newpathlen, "%s", vm->vm_prefix);
  hts_mutex_unlock(&vfs_mutex);
  return 0;
}


/**
 *
 */
static int
vfs_scandir(fa_dir_t *fd, const char *url, char *errbuf, size_t errlen)
{
  char newpath[1024];

  if(!strcmp(url, "/")) {
    vfs_mapping_t *vm;

    hts_mutex_lock(&vfs_mutex);

    if(LIST_FIRST(&vfs_mappings) == NULL)
      fa_dir_add(fd, "vfs:///README.TXT", "README.TXT", CONTENT_FILE);

    LIST_FOREACH(vm, &vfs_mappings, vm_link) {
      char u[512];
      snprintf(u, sizeof(u), "vfs:///%s", vm->vm_vdir);
      fa_dir_add(fd, u, vm->vm_vdir, CONTENT_DIR);
    }
    hts_mutex_unlock(&vfs_mutex);
    return 0;
  }

  url++;

  if(resolve_mapping(url, newpath, sizeof(newpath))) {
    snprintf(errbuf, errlen, "No such file or directory");
    return -1;
  }

  return fa_scandir2(fd, newpath, errbuf, errlen);
}


/**
 *
 */
static fa_handle_t *
vfs_open(fa_protocol_t *fap, const char *url, char *errbuf, size_t errlen,
         int flags, struct prop *stats)
{
  char newpath[1024];

  if(!strcmp(url, "/README.TXT"))
    return memfile_make(READMETXT, strlen(READMETXT));

  if(*url != '/') {
    snprintf(errbuf, errlen, "No such file or directory");
    return NULL;
  }

  url++;

  if(resolve_mapping(url, newpath, sizeof(newpath))) {
    snprintf(errbuf, errlen, "Invalid virtual directory");
    return NULL;
  }

  if(*newpath == 0) {
    snprintf(errbuf, errlen, "Invalid virtual directory");
    return NULL;
  }
  return fa_open_ex(newpath, errbuf, errlen, flags, stats);
}


/**
 *
 */
static int
vfs_stat(fa_protocol_t *fap, const char *url, struct fa_stat *fs,
	char *errbuf, size_t errlen, int non_interactive)
{
  char newpath[1024];

  memset(fs, 0, sizeof(struct fa_stat));


  if(!strcmp(url, "/README.TXT")) {
    fs->fs_type = CONTENT_FILE;
    fs->fs_size = strlen(READMETXT);
    return 0;
  }


  if(!strcmp(url, "/")) {
    fs->fs_type = CONTENT_DIR;
    return 0;
  }

  url++;

  if(resolve_mapping(url, newpath, sizeof(newpath))) {
    snprintf(errbuf, errlen, "No such file or directory");
    return -1;
  }
  return fa_stat(newpath, fs, errbuf, errlen);
}


/**
 *
 */
static void
vfs_init(void)
{
  hts_mutex_init(&vfs_mutex);
}


/**
 *
 */
fa_protocol_t fa_protocol_vfs = {
  .fap_name  = "vfs",
  .fap_init  = vfs_init,
  .fap_scan  = vfs_scandir,
  .fap_open  = vfs_open,
  .fap_stat  = vfs_stat,
};

FAP_REGISTER(vfs);
