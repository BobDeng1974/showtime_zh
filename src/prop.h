/*
 *  Property trees
 *  Copyright (C) 2008 Andreas Öman
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

#ifndef PROP_H__
#define PROP_H__

#include <stdlib.h>

#include "event.h"
#include "arch/threads.h"
#include "misc/queue.h"
#include "htsmsg/htsmsg.h"
#include "misc/rstr.h"

struct prop;
struct prop_sub;
struct pixmap;

#define PROP_ADD_SELECTED 0x1

typedef enum {
  PROP_SET_VOID,
  PROP_SET_RSTRING,
  PROP_SET_INT,
  PROP_SET_FLOAT,
  PROP_SET_DIR,
  PROP_SET_PIXMAP,
  PROP_SET_RLINK,

  PROP_ADD_CHILD,
  PROP_ADD_CHILD_BEFORE,
  PROP_DEL_CHILD,
  PROP_MOVE_CHILD,
  PROP_SELECT_CHILD,
  PROP_REQ_NEW_CHILD,
  PROP_REQ_DELETE_MULTI,
  PROP_DESTROYED,
  PROP_EXT_EVENT,
  PROP_SUBSCRIPTION_MONITOR_ACTIVE,
} prop_event_t;

typedef void (prop_callback_t)(void *opaque, prop_event_t event, ...);
typedef void (prop_callback_string_t)(void *opaque, const char *str);
typedef void (prop_callback_int_t)(void *opaque, int value);
typedef void (prop_callback_float_t)(void *opaque, float value);

struct prop_sub;
typedef void (prop_trampoline_t)(struct prop_sub *s, prop_event_t event, ...);

typedef void (prop_lockmgr_t)(void *ptr, int lock);

TAILQ_HEAD(prop_queue, prop);
LIST_HEAD(prop_list, prop);
LIST_HEAD(prop_sub_list, prop_sub);



/**
 *
 */
TAILQ_HEAD(prop_notify_queue, prop_notify);

typedef struct prop_courier {

  struct prop_notify_queue pc_queue;

  hts_mutex_t *pc_entry_mutex;
  hts_cond_t pc_cond;
  hts_thread_t pc_thread;

  int pc_run;
  int pc_detached;

  void (*pc_notify)(void *opaque);
  void *pc_opaque;
  
} prop_courier_t;



/**
 * Property types
 */
typedef enum {
  PROP_VOID,
  PROP_DIR,
  PROP_STRING,
  PROP_FLOAT,
  PROP_INT,
  PROP_PIXMAP,
  PROP_LINK,
  PROP_ZOMBIE, /* Destroyed can never be changed again */
} prop_type_t;


/**
 *
 */
typedef struct prop {

  /**
   * Refcount. Not protected by mutex. Modification needs to be issued
   * using atomic ops. This refcount only protects the memory allocated
   * for this property, or in other words you can assume that a pointer
   * to a prop_t is valid as long as you own a reference to it.
   *
   * Note: hp_xref which is another refcount protecting contents of the
   * entire property
   */
  int hp_refcount;

  /**
   * Property name. Protected by mutex
   */
  const char *hp_name;

  /**
   * Parent linkage. Protected by mutex
   */
  struct prop *hp_parent;
  TAILQ_ENTRY(prop) hp_parent_link;


  /**
   * Originating property. Used when reflecting properties
   * in the tree (aka symlinks). Protected by mutex
   */
  struct prop *hp_originator;
  LIST_ENTRY(prop) hp_originator_link;

  /**
   * Properties receiving our values. Protected by mutex
   */
  struct prop_list hp_targets;


  /**
   * Subscriptions. Protected by mutex
   */
  struct prop_sub_list hp_value_subscriptions;
  struct prop_sub_list hp_canonical_subscriptions;

  /**
   * Payload type
   * Protected by mutex
   */
  uint8_t hp_type;

  /**
   * Various flags
   * Protected by mutex
   */
  uint8_t hp_flags;
#define PROP_CLIPPED_VALUE 0x1
#define PROP_SORTED_CHILDS 0x2
#define PROP_SORT_CASE_INSENSITIVE 0x4
#define PROP_NAME_NOT_ALLOCATED    0x8
#define PROP_XREFED_ORIGINATOR 0x10

  /**
   * Number of monitoring subscriptions (linked via hp_value_subscriptions)
   * We limit this to 255, should never be a problem. And it's checked
   * in the code as well
   * Protected by mutex
   */
  uint8_t hp_monitors;

  /**
   * Extended refcount. Used to keep contents of the property alive
   * We limit this to 255, should never be a problem. And it's checked
   * in the code as well
   * Protected by mutex
   */
  uint8_t hp_xref;


  /**
   * Actual payload
   * Protected by mutex
   */
  union {
    struct {
      float val, min, max;
    } f;
    struct {
      int val, min, max;
    } i;
    rstr_t *rstr;
    struct {
      struct prop_queue childs;
      struct prop *selected;
    } c;
    struct pixmap *pixmap;
    struct {
      rstr_t *rtitle;
      rstr_t *rurl;
    } link;
    void *ptr;
  } u;

#define hp_rstring   u.rstr
#define hp_float    u.f.val
#define hp_int      u.i.val
#define hp_childs   u.c.childs
#define hp_selected u.c.selected
#define hp_pixmap   u.pixmap
#define hp_link_rtitle u.link.rtitle
#define hp_link_rurl   u.link.rurl
#define hp_ptr         u.ptr

} prop_t;



/**
 *
 */
typedef struct prop_sub {

  /**
   * Refcount. Not protected by mutex. Modification needs to be issued
   * using atomic ops.
   */
  int hps_refcount;

  /**
   * Callback. May never be changed. Not protected by mutex
   */
  void *hps_callback;

  /**
   * Opaque value for callback
   */
  void *hps_opaque;

  /**
   * Trampoline. A tranform function that invokes the actual user
   * supplied callback.
   * May never be changed. Not protected by mutex.
   */
  prop_trampoline_t *hps_trampoline;

  /**
   * Pointer to courier, May never be changed. Not protected by mutex
   */
  prop_courier_t *hps_courier;

  /**
   * Lock to be held when invoking callback. It must also be held
   * when destroying the subscription.
   */
  void *hps_lock;

  /**
   * Function to call to obtain / release the lock.
   */
  prop_lockmgr_t *hps_lockmgr;

  /**
   * Set when a subscription is destroyed. Protected by hps_lock.
   * In other words. It's impossible to destroy a subscription
   * if no lock is specified.
   */
  uint8_t hps_zombie;

  /**
   * Used to avoid sending two notification when relinking
   * to another tree. Protected by global mutex
   */
  uint8_t hps_pending_unlink;

  /**
   * Flags as passed to prop_subscribe(). May never be changed
   */
  uint8_t hps_flags;

  /**
   * Linkage to property. Protected by global mutex
   */
  LIST_ENTRY(prop_sub) hps_value_prop_link;
  prop_t *hps_value_prop;

  /**
   * Linkage to property. Protected by global mutex
   */
  LIST_ENTRY(prop_sub) hps_canonical_prop_link;
  prop_t *hps_canonical_prop;

} prop_sub_t;


/**
 *
 */

prop_t *prop_get_global(void);

void prop_init(void);

/**
 * Use with PROP_TAG_NAME_VECTOR
 */
#define PNVEC(name...) (const char *[]){name, NULL}

#define PROP_SUB_DIRECT_UPDATE 0x1
#define PROP_SUB_NO_INITIAL_UPDATE 0x2
#define PROP_SUB_TRACK_DESTROY 0x4
#define PROP_SUB_DEBUG         0x8 // TRACE(TRACE_DEBUG, ...) changes
#define PROP_SUB_SUBSCRIPTION_MONITOR 0x10

enum {
  PROP_TAG_END = 0,
  PROP_TAG_NAME_VECTOR,
  PROP_TAG_CALLBACK,
  PROP_TAG_CALLBACK_STRING,
  PROP_TAG_CALLBACK_INT,
  PROP_TAG_CALLBACK_FLOAT,
  PROP_TAG_COURIER,
  PROP_TAG_ROOT,
  PROP_TAG_NAMED_ROOT,
  PROP_TAG_MUTEX,
  PROP_TAG_EXTERNAL_LOCK,
};

#define PROP_TAG_NAME(name...) \
 PROP_TAG_NAME_VECTOR, (const char *[]){name, NULL}

prop_sub_t *prop_subscribe(int flags, ...) __attribute__((__sentinel__(0)));

void prop_unsubscribe(prop_sub_t *s);

prop_t *prop_create_ex(prop_t *parent, const char *name,
		       prop_sub_t *skipme, int flags)
     __attribute__ ((malloc));

#define prop_create(parent, name) \
 prop_create_ex(parent, name, NULL, __builtin_constant_p(name) ? \
 PROP_NAME_NOT_ALLOCATED : 0)

void prop_destroy(prop_t *p);

void prop_move(prop_t *p, prop_t *before);

void prop_set_string_ex(prop_t *p, prop_sub_t *skipme, const char *str);

void prop_set_rstring_ex(prop_t *p, prop_sub_t *skipme, rstr_t *rstr);

void prop_set_stringf_ex(prop_t *p, prop_sub_t *skipme, const char *fmt, ...);

void prop_set_float_ex(prop_t *p, prop_sub_t *skipme, float v);

void prop_set_float_clipping_range(prop_t *p, float min, float max);

void prop_add_float_ex(prop_t *p, prop_sub_t *skipme, float v);

void prop_set_int_ex(prop_t *p, prop_sub_t *skipme, int v);

void prop_toggle_int_ex(prop_t *p, prop_sub_t *skipme);

void prop_add_int_ex(prop_t *p, prop_sub_t *skipme, int v);

void prop_set_int_clipping_range(prop_t *p, int min, int max);

void prop_set_void_ex(prop_t *p, prop_sub_t *skipme);

void prop_set_pixmap_ex(prop_t *p, prop_sub_t *skipme, struct pixmap *pm);

void prop_set_link_ex(prop_t *p, prop_sub_t *skipme, const char *title,
		      const char *url);

#define prop_set_string(p, str) prop_set_string_ex(p, NULL, str)

#define prop_set_stringf(p, fmt...) prop_set_stringf_ex(p, NULL, fmt)

#define prop_set_float(p, v) prop_set_float_ex(p, NULL, v)

#define prop_add_float(p, v) prop_add_float_ex(p, NULL, v)

#define prop_set_int(p, v) prop_set_int_ex(p, NULL, v)

#define prop_add_int(p, v) prop_add_int_ex(p, NULL, v)

#define prop_toggle_int(p) prop_toggle_int_ex(p, NULL)

#define prop_set_void(p) prop_set_void_ex(p, NULL)

#define prop_set_pixmap(p, pp) prop_set_pixmap_ex(p, NULL, pp)

#define prop_set_link(p, title, link) prop_set_link_ex(p, NULL, title, link)

#define prop_set_rstring(p, rstr) prop_set_rstring_ex(p, NULL, rstr)

int prop_get_string(prop_t *p, char *buf, size_t bufsize)
     __attribute__ ((warn_unused_result));

void prop_ref_dec(prop_t *p);

void prop_ref_inc(prop_t *p);

prop_t *prop_xref_addref(prop_t *p) __attribute__ ((warn_unused_result));

int prop_set_parent_ex(prop_t *p, prop_t *parent, prop_t *before, 
		       prop_sub_t *skipme)
     __attribute__ ((warn_unused_result));
     
#define prop_set_parent(p, parent) prop_set_parent_ex(p, parent, NULL, NULL)

void prop_unparent_ex(prop_t *p, prop_sub_t *skipme);

#define prop_unparent(p) prop_unparent_ex(p, NULL)

void prop_rename_ex(prop_t *p, const char *name, prop_sub_t *skipme);

#define prop_rename(p, name) prop_rename_ex(p, name, NULL)

void prop_link_ex(prop_t *src, prop_t *dst, prop_sub_t *skipme, int hard);

#define prop_link(src, dst) prop_link_ex(src, dst, NULL, 0)

void prop_unlink_ex(prop_t *p, prop_sub_t *skipme);

#define prop_unlink(p) prop_unlink_ex(p, NULL)

void prop_select_ex(prop_t *p, int advisory, prop_sub_t *skipme);

#define prop_select(p, advisory) prop_select_ex(p, advisory, NULL)

void prop_unselect_ex(prop_t *parent, prop_sub_t *skipme);

#define prop_unselect(parent) prop_unselect_ex(parent, NULL)

void prop_destroy_childs(prop_t *parent);

prop_t **prop_get_ancestors(prop_t *p);

void prop_ancestors_unref(prop_t **r);

prop_t *prop_get_by_name(const char **name, int follow_symlinks, ...)
     __attribute__((__sentinel__(0)));

void prop_request_new_child(prop_t *p);

void prop_request_delete(prop_t *p);

void prop_request_delete_multi(prop_t **vec);

prop_courier_t *prop_courier_create_thread(hts_mutex_t *entrymutex,
					   const char *name);

prop_courier_t *prop_courier_create_passive(void);

prop_courier_t *prop_courier_create_notify(void (*notify)(void *opaque),
					   void *opaque);

void prop_courier_poll(prop_courier_t *pc);

void prop_courier_destroy(prop_courier_t *pc);

void prop_courier_stop(prop_courier_t *pc);

prop_t *prop_get_by_names(prop_t *parent, ...) 
     __attribute__((__sentinel__(0)));

htsmsg_t *prop_tree_to_htsmsg(prop_t *p);

void prop_send_ext_event(prop_t *p, event_t *e);

void prop_pvec_free(prop_t **a);

int prop_pvec_len(prop_t **src);

prop_t **prop_pvec_clone(prop_t **src);

/* DEBUGish */
const char *propname(prop_t *p);

void prop_print_tree(prop_t *p, int followlinks);

void prop_test(void);

#endif /* PROP_H__ */
