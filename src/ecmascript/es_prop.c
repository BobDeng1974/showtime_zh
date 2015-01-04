#include <assert.h>

#include "prop/prop_i.h"
#include "ecmascript.h"
#include "backend/backend_prop.h"


/**
 *
 */
typedef struct es_prop_sub {
  es_resource_t eps_super;
  prop_sub_t *eps_sub;
  char eps_autodestry;
} es_prop_sub_t;



/**
 *
 */
static void
es_prop_sub_destroy(es_resource_t *eres)
{
  es_prop_sub_t *eps = (es_prop_sub_t *)eres;
  if(eps->eps_sub == NULL)
    return;

  es_root_unregister(eres->er_ctx->ec_duk, eps);
  prop_unsubscribe(eps->eps_sub);
  eps->eps_sub = NULL;
  es_resource_unlink(eres);
}


/**
 *
 */
static const es_resource_class_t es_resource_prop_sub = {
  .erc_name = "propsub",
  .erc_size = sizeof(es_prop_sub_t),
  .erc_destroy = es_prop_sub_destroy,
};


/**
 *
 */
prop_t *
es_stprop_get(duk_context *ctx, int val_index)
{
  return es_get_native_obj(ctx, val_index, ES_NATIVE_PROP);
}


/**
 *
 */
void
es_stprop_push(duk_context *ctx, prop_t *p)
{
  es_push_native_obj(ctx, ES_NATIVE_PROP, prop_ref_inc(p));
}

/**
 *
 */
static int
es_prop_release_duk(duk_context *ctx)
{
  prop_t *p = duk_require_pointer(ctx, 0);

  hts_mutex_lock(&prop_mutex);

  if(p->hp_parent == NULL)
    prop_destroy0(p);

  prop_ref_dec_locked(p);

  hts_mutex_unlock(&prop_mutex);
  return 0;
}


/**
 *
 */
static int
es_prop_print_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  prop_print_tree(p, 1);
  return 0;
}


/**
 *
 */
static int
es_prop_create_duk(duk_context *ctx)
{
  es_stprop_push(ctx, prop_create_root(NULL));
  return 1;
}


/**
 *
 */
static int
es_prop_get_global(duk_context *ctx)
{
  es_stprop_push(ctx, prop_get_global());
  return 1;
}


/**
 *
 */
static int
es_prop_get_name_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);

  rstr_t *r = prop_get_name(p);
  duk_push_string(ctx, rstr_get(r));
  rstr_release(r);
  return 1;
}


/**
 *
 */
static int
es_prop_get_value_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  char tmp[64];
  hts_mutex_lock(&prop_mutex);

  if(p->hp_type == PROP_ZOMBIE) {
    hts_mutex_unlock(&prop_mutex);
    duk_error(ctx, ST_ERROR_PROP_ZOMBIE, NULL);
  }

  switch(p->hp_type) {
  case PROP_CSTRING:
    duk_push_string(ctx, p->hp_cstring);
    break;
  case PROP_RSTRING:
    duk_push_string(ctx, rstr_get(p->hp_rstring));
    break;
  case PROP_URI:
    duk_push_string(ctx, rstr_get(p->hp_uri_title));
    break;
  case PROP_FLOAT:
    duk_push_number(ctx, p->hp_float);
    break;
  case PROP_INT:
    duk_push_int(ctx, p->hp_int);
    break;
  case PROP_VOID:
    duk_push_null(ctx);
    break;
  case PROP_DIR:
    snprintf(tmp, sizeof(tmp), "[prop directory '%s']", p->hp_name);
    duk_push_string(ctx, tmp);
    break;
  default:
    snprintf(tmp, sizeof(tmp), "[prop internal type %d]", p->hp_type);
    duk_push_string(ctx, tmp);
    break;
  }
  hts_mutex_unlock(&prop_mutex);
  return 1;
}


/**
 *
 */
static int
es_prop_get_child_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  const char *str = NULL;
  int idx = 0;
  if(duk_is_number(ctx, 1)) {
    idx = duk_to_int(ctx, 1);
  } else {
    str = duk_require_string(ctx, 1);
  }

  hts_mutex_lock(&prop_mutex);

  if(p->hp_type == PROP_ZOMBIE) {
    hts_mutex_unlock(&prop_mutex);
    duk_error(ctx, ST_ERROR_PROP_ZOMBIE, NULL);
  }

  if(str != NULL) {
    p = prop_create0(p, str, NULL, 0);
  } else {
    prop_t *c;
    TAILQ_FOREACH(c, &p->hp_childs, hp_parent_link) {
      if(idx == 0)
        break;
      idx--;
    }

    p = c;
  }

  if(p != NULL) {
    es_push_native_obj(ctx, ES_NATIVE_PROP, prop_ref_inc(p));
    hts_mutex_unlock(&prop_mutex);
    return 1;
  }
  hts_mutex_unlock(&prop_mutex);
  return 0;
}


/**
 *
 */
static int
es_prop_enum_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);

  duk_push_array(ctx);

  hts_mutex_lock(&prop_mutex);


  if(p->hp_type == PROP_DIR) {
    prop_t *c;
    int idx = 0;
    TAILQ_FOREACH(c, &p->hp_childs, hp_parent_link) {
      if(c->hp_name)
        duk_push_string(ctx, c->hp_name);
      else
        duk_push_int(ctx, idx);

      duk_put_prop_index(ctx, -2, idx++);
    }
  }
  hts_mutex_unlock(&prop_mutex);
  return 1;
}


/**
 *
 */
static int
es_prop_has_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  const char *name = duk_get_string(ctx, 1);
  int yes = 0;

  hts_mutex_lock(&prop_mutex);

  if(p->hp_type == PROP_DIR) {
    prop_t *c;
    TAILQ_FOREACH(c, &p->hp_childs, hp_parent_link) {
      if(c->hp_name != NULL && !strcmp(c->hp_name, name)) {
        yes = 1;
        break;
      }
    }
  }
  hts_mutex_unlock(&prop_mutex);
  duk_push_boolean(ctx, yes);
  return 1;
}


/**
 *
 */
static int
es_prop_delete_child_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  const char *name = duk_require_string(ctx, 1);
  prop_destroy_by_name(p, name);
  duk_push_boolean(ctx, 1);
  return 1;
}


/**
 *
 */
static int
es_prop_delete_childs_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  prop_destroy_childs(p);
  return 0;
}


/**
 *
 */
static int
es_prop_destroy_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  prop_destroy(p);
  return 0;
}


//#define SETPRINTF(fmt...) printf(fmt);
#define SETPRINTF(fmt, ...)

/**
 *
 */
static int
es_prop_set_value_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  const char *str = duk_require_string(ctx, 1);

  SETPRINTF("Set %s.%s to ", p->hp_name, str);

  if(duk_is_boolean(ctx, 2)) {
    SETPRINTF("%s", duk_get_boolean(ctx, 2) ? "true" : "false");
    prop_set(p, str, PROP_SET_INT, duk_get_boolean(ctx, 2));
  } else if(duk_is_number(ctx, 2)) {
    double dbl = duk_get_number(ctx, 2);

    if(ceil(dbl) == dbl && dbl <= INT32_MAX && dbl >= INT32_MIN) {
      SETPRINTF("%d", (int)dbl);
      prop_set(p, str, PROP_SET_INT, (int)dbl);
    } else {
      SETPRINTF("%f", dbl);
      prop_set(p, str, PROP_SET_FLOAT, dbl);
    }
  } else if(duk_is_string(ctx, 2)) {
    SETPRINTF("\"%s\"", duk_get_string(ctx, 2));
    prop_set(p, str, PROP_SET_STRING, duk_get_string(ctx, 2));
  } else {
    SETPRINTF("(void)");
    prop_set(p, str, PROP_SET_VOID);
  }
  SETPRINTF("\n");
  return 0;
}


/**
 *
 */
static int
es_prop_set_rich_str_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  const char *key = duk_require_string(ctx, 1);
  const char *richstr = duk_require_string(ctx, 2);

  prop_t *c = prop_create_r(p, key);
  prop_set_string_ex(c, NULL, richstr, PROP_STR_RICH);
  prop_ref_dec(c);
  return 0;
}


/**
 *
 */
static int
es_prop_set_parent_duk(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  prop_t *parent = es_stprop_get(ctx, 1);

  if(prop_set_parent(p, parent))
    prop_destroy(p);

  return 0;
}


/**
 *
 */
static void
es_sub_cb(void *opaque, prop_event_t event, ...)
{
  es_prop_sub_t *eps = opaque;
  es_context_t *ec = eps->eps_super.er_ctx;
  va_list ap;
  const rstr_t *r, *r2;
  const char *c;
  double d;
  int i;
  int destroy = 0;
  const  event_t *e;
  prop_t *p1, *p2;
  duk_context *ctx = ec->ec_duk;

  es_push_root(ctx, eps);

  va_start(ap, event);

  int nargs;

  switch(event) {
  case PROP_SET_DIR:
    (void)va_arg(ap, prop_t *);
    duk_push_string(ctx, "dir");
    nargs = 1;
    break;

  case PROP_SET_VOID:
    (void)va_arg(ap, prop_t *);
    duk_push_string(ctx, "set");
    duk_push_null(ctx);
    nargs = 2;
    break;

  case PROP_SET_RSTRING:
    r = va_arg(ap, const rstr_t *);
    (void)va_arg(ap, prop_t *);
    duk_push_string(ctx, "set");
    duk_push_string(ctx, rstr_get(r));
    nargs = 2;
    break;

  case PROP_SET_CSTRING:
    c = va_arg(ap, const char *);
    (void)va_arg(ap, prop_t *);
    duk_push_string(ctx, "set");
    duk_push_string(ctx, c);
    nargs = 2;
    break;

  case PROP_SET_URI:
    r = va_arg(ap, const rstr_t *);
    r2 = va_arg(ap, const rstr_t *);
    (void)va_arg(ap, prop_t *);
    duk_push_string(ctx, "uri");
    duk_push_string(ctx, rstr_get(r));
    duk_push_string(ctx, rstr_get(r2));
    nargs = 3;
    break;

  case PROP_SET_INT:
    i = va_arg(ap, int);
    (void)va_arg(ap, prop_t *);
    duk_push_string(ctx, "set");
    duk_push_int(ctx, i);
    nargs = 2;
    break;

  case PROP_SET_FLOAT:
    d = va_arg(ap, double);
    (void)va_arg(ap, prop_t *);
    duk_push_string(ctx, "set");
    duk_push_number(ctx, d);
    nargs = 2;
    break;

  case PROP_WANT_MORE_CHILDS:
    duk_push_string(ctx, "wantmorechilds");
    nargs = 1;
    break;

  case PROP_DESTROYED:
    (void)va_arg(ap, prop_sub_t *);
    duk_push_string(ctx, "destroyed");
    nargs = 1;
    if(eps->eps_autodestry)
      destroy = 1;
    break;

  case PROP_REQ_MOVE_CHILD:
    duk_push_string(ctx, "reqmove");
    p1 = va_arg(ap, prop_t *);
    p2 = va_arg(ap, prop_t *);
    nargs = 3;
    es_stprop_push(ctx, p1);

    if(p2 != NULL) {
      es_stprop_push(ctx, p2);
    } else {
      duk_push_null(ctx);
    }
    break;

  case PROP_EXT_EVENT:
    e = va_arg(ap, const event_t *);

    if(event_is_type(e, EVENT_DYNAMIC_ACTION)) {
      const event_payload_t *ep = (const event_payload_t *)e;
      nargs = 2;
      duk_push_string(ctx, "action");
      duk_push_string(ctx, ep->payload);

    } else if(e->e_type_x == EVENT_ACTION_VECTOR) {
      const event_action_vector_t *eav = (const event_action_vector_t *)e;
      assert(eav->num > 0);
      nargs = 2;
      duk_push_string(ctx, "action");
      duk_push_string(ctx, action_code2str(eav->actions[0]));

    } else if(e->e_type_x == EVENT_UNICODE) {
      const event_int_t *eu = (const event_int_t *)e;
      nargs = 2;
      duk_push_string(ctx, "unicode");
      duk_push_int(ctx, eu->val);

    } else if(e->e_type_x == EVENT_PROPREF) {
      event_prop_t *ep = (event_prop_t *)e;
      nargs = 2;
      duk_push_string(ctx, "propref");
      es_stprop_push(ctx, ep->p);
    } else {
      nargs = 0;
    }

    if(nargs > 0 && e->e_nav != NULL) {
      es_stprop_push(ctx, e->e_nav);
      nargs++;
    }
    break;

  case PROP_SELECT_CHILD:
    duk_push_string(ctx, "selectchild");
    es_stprop_push(ctx, va_arg(ap, prop_t *));
    nargs = 2;
    break;

  default:
    nargs = 0;
    break;
  }

  va_end(ap);

  if(nargs > 0) {
    int rc = duk_pcall(ctx, nargs);
    if(rc)
      es_dump_err(ctx);
  }
  duk_pop(ctx);

  if(destroy)
    es_resource_destroy(&eps->eps_super);
}

/**
 *
 */
static int
es_prop_lockmgr(void *ptr, int op)
{
  es_context_t *ec = ptr;

  switch(op) {
  case PROP_LOCK_UNLOCK:
    hts_mutex_unlock(&ec->ec_mutex);
    return 0;
  case PROP_LOCK_LOCK:
    hts_mutex_lock(&ec->ec_mutex);
    return 0;
  case PROP_LOCK_TRY:
    return hts_mutex_trylock(&ec->ec_mutex);

  case PROP_LOCK_RETAIN:
    atomic_inc(&ec->ec_refcount);
    return 0;

  case PROP_LOCK_RELEASE:
    es_context_release(ec);
    return 0;
  }
  abort();
}

/**
 *
 */
static int
es_prop_subscribe(duk_context *ctx)
{
  es_context_t *ec = es_get(ctx);
  prop_t *p = es_stprop_get(ctx, 0);
  es_prop_sub_t *eps = es_resource_create(ec, &es_resource_prop_sub, 1);

  es_root_register(ctx, 1, eps);

  eps->eps_autodestry = es_prop_is_true(ctx, 2, "autoDestroy");

  int flags = PROP_SUB_TRACK_DESTROY;

  if(es_prop_is_true(ctx, 2, "ignoreVoid"))
    flags |= PROP_SUB_IGNORE_VOID;

  if(es_prop_is_true(ctx, 2, "debug"))
    flags |= PROP_SUB_DEBUG;

  if(es_prop_is_true(ctx, 2, "noInitialUpdate"))
    flags |= PROP_SUB_NO_INITIAL_UPDATE;

  eps->eps_sub =
      prop_subscribe(flags,
                   PROP_TAG_ROOT, p,
                   PROP_TAG_LOCKMGR, es_prop_lockmgr,
                   PROP_TAG_MUTEX, ec,
                   PROP_TAG_CALLBACK, es_sub_cb, eps,
                   NULL);

  es_resource_push(ctx, &eps->eps_super);
  return 1;
}


/**
 *
 */
static int
es_prop_have_more(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  int yes = duk_require_boolean(ctx, 1);
  prop_have_more_childs(p, yes);
  return 0;
}


/**
 *
 */
static int
es_prop_make_url(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  rstr_t *r = backend_prop_make(p, NULL);
  duk_push_string(ctx, rstr_get(r));
  rstr_release(r);
  return 1;
}


/**
 *
 */
static int
es_prop_select(duk_context *ctx)
{
  prop_select(es_stprop_get(ctx, 0));
  return 0;
}


/**
 *
 */
static int
es_prop_link(duk_context *ctx)
{
  prop_link(es_stprop_get(ctx, 0), es_stprop_get(ctx, 1));
  return 0;
}


/**
 *
 */
static int
es_prop_unlink(duk_context *ctx)
{
  prop_unlink(es_stprop_get(ctx, 0));
  return 0;
}


/**
 *
 */
static int
es_prop_send_event(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  const char *type = duk_require_string(ctx, 1);
  event_t *e;

  if(!strcmp(type, "redirect")) {
    e = event_create_str(EVENT_REDIRECT, duk_require_string(ctx, 2));
  } else if(!strcmp(type, "openurl")) {

    event_openurl_args_t args = {};

    rstr_t *url        = es_prop_to_rstr(ctx, 2, "url");
    rstr_t *view       = es_prop_to_rstr(ctx, 2, "view");
    rstr_t *how        = es_prop_to_rstr(ctx, 2, "how");
    rstr_t *parent_url = es_prop_to_rstr(ctx, 2, "parenturl");

    args.url        = rstr_get(url);
    args.view       = rstr_get(view);
    args.how        = rstr_get(how);
    args.parent_url = rstr_get(parent_url);

    e = event_create_openurl_args(&args);

    rstr_release(url);
    rstr_release(view);
    rstr_release(how);
    rstr_release(parent_url);

  } else {
    duk_error(ctx, DUK_ERR_ERROR, "Event type %s not understood", type);
  }

  prop_send_ext_event(p, e);
  event_release(e);
  return 0;
}


/**
 *
 */
static int
es_prop_is_value(duk_context *ctx)
{
  prop_t *p = es_get_native_obj_nothrow(ctx, 0, ES_NATIVE_PROP);
  if(p == NULL) {
    duk_push_false(ctx);

  } else {
    int v;

    hts_mutex_lock(&prop_mutex);

    switch(p->hp_type) {
    case PROP_CSTRING:
    case PROP_RSTRING:
    case PROP_URI:
    case PROP_FLOAT:
    case PROP_INT:
    case PROP_VOID:
      v = 1;
      break;
    default:
      v = 0;
      break;
    }
    hts_mutex_unlock(&prop_mutex);
    duk_push_boolean(ctx, v);
  }
  return 1;
}


/**
 *
 */
static int
es_prop_atomic_add(duk_context *ctx)
{
  prop_t *p = es_stprop_get(ctx, 0);
  int num = duk_require_number(ctx, 1);

  prop_add_int(p, num);
  return 0;
}


/**
 *
 */
static int
es_prop_is_same(duk_context *ctx)
{
  prop_t *a = es_stprop_get(ctx, 0);
  prop_t *b = es_stprop_get(ctx, 1);
  duk_push_boolean(ctx, a == b);
  return 1;
}


/**
 *
 */
static int
es_prop_move_before(duk_context *ctx)
{
  prop_t *a = es_stprop_get(ctx, 0);
  prop_t *b = es_get_native_obj_nothrow(ctx, 1, ES_NATIVE_PROP);
  prop_move(a, b);
  return 0;
}


/**
 *
 */
static int
es_prop_unload_destroy(duk_context *ctx)
{
  es_context_t *ec = es_get(ctx);
  prop_t *a = es_stprop_get(ctx, 0);
  ec->ec_prop_unload_destroy = prop_vec_append(ec->ec_prop_unload_destroy, a);
  return 0;
}


/**
 *
 */
static int
es_prop_is_zombie(duk_context *ctx)
{
  prop_t *a = es_stprop_get(ctx, 0);
  duk_push_boolean(ctx, a->hp_type == PROP_ZOMBIE);
  return 1;
}

/**
 * Showtime object exposed functions
 */
const duk_function_list_entry fnlist_Showtime_prop[] = {

  { "propPrint",               es_prop_print_duk,             1 },
  { "propRelease",             es_prop_release_duk,           1 },
  { "propCreate",              es_prop_create_duk,            0 },
  { "propGetValue",            es_prop_get_value_duk,         1 },
  { "propGetName",             es_prop_get_name_duk,          1 },
  { "propGetChild",            es_prop_get_child_duk,         2 },
  { "propSet",                 es_prop_set_value_duk,         3 },
  { "propSetRichStr",          es_prop_set_rich_str_duk,      3 },
  { "propSetParent",           es_prop_set_parent_duk,        2 },
  { "propSubscribe",           es_prop_subscribe,             3 },
  { "propHaveMore",            es_prop_have_more,             2 },
  { "propMakeUrl",             es_prop_make_url,              1 },
  { "propGlobal",              es_prop_get_global,            0 },
  { "propEnum",                es_prop_enum_duk,              1 },
  { "propHas",                 es_prop_has_duk,               2 },
  { "propDeleteChild",         es_prop_delete_child_duk,      2 },
  { "propDeleteChilds",        es_prop_delete_childs_duk,     1 },
  { "propDestroy",             es_prop_destroy_duk,           1 },
  { "propSelect",              es_prop_select,                1 },
  { "propLink",                es_prop_link,                  2 },
  { "propUnlink",              es_prop_unlink,                1 },
  { "propSendEvent",           es_prop_send_event,            3 },
  { "propIsValue",             es_prop_is_value,              1 },
  { "propAtomicAdd",           es_prop_atomic_add,            2 },
  { "propIsSame",              es_prop_is_same,               2 },
  { "propMoveBefore",          es_prop_move_before,           2 },
  { "propUnloadDestroy",       es_prop_unload_destroy,        1 },
  { "propIsZombie",            es_prop_is_zombie,             1 },
  { NULL, NULL, 0}
};
