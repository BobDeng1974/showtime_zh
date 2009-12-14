/*
 *  GL Widgets, model loader
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

#include <unistd.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <alloca.h>

#include "glw.h"
#include "glw_model.h"

/**
 *
 */
static glw_t *
glw_model_error(glw_root_t *gr, errorinfo_t *ei, glw_t *parent)
{
  char buf[256];

  snprintf(buf, sizeof(buf), "GLW %s:%d", ei->file, ei->line);

  trace(TRACE_ERROR, buf, "%s", ei->error);

  snprintf(buf, sizeof(buf), "GLW %s:%d: Error: %s",
	   ei->file, ei->line, ei->error);

  return glw_create_i(gr,
		      GLW_LABEL,
		      GLW_ATTRIB_PARENT, parent,
		      GLW_ATTRIB_CAPTION, buf,
		      NULL);
}


/**
 *
 */
glw_t *
glw_model_create(glw_root_t *gr, const char *src,
		 glw_t *parent, prop_t *prop, prop_t *prop_parent)
{
  token_t *eof, *l, *t;
  errorinfo_t ei;
  glw_t *r;
  glw_model_eval_context_t ec;
  glw_model_t *gm;

  LIST_FOREACH(gm, &gr->gr_models, gm_link) {
    if(!strcmp(gm->gm_source, src))
      break;
  }

  if(gm == NULL) {
    token_t *sof = calloc(1, sizeof(token_t));
    sof->type = TOKEN_START;
#ifdef GLW_MODEL_ERRORINFO
    sof->file = rstr_alloc(src);
#endif

    if((l = glw_model_load1(gr, src, &ei, sof)) == NULL) {
      glw_model_free_chain(sof);
      return glw_model_error(gr, &ei, parent);
    }
    eof = calloc(1, sizeof(token_t));
    eof->type = TOKEN_END;
#ifdef GLW_MODEL_ERRORINFO
    eof->file = rstr_alloc(src);
#endif
    l->next = eof;
  
    if(glw_model_preproc(gr, sof, &ei) || glw_model_parse(sof, &ei)) {
      glw_model_free_chain(sof);
      return glw_model_error(gr, &ei, parent);
    }

    gm = calloc(1, sizeof(glw_model_t));
    gm->gm_sof = sof;
    gm->gm_source = strdup(src);
    LIST_INSERT_HEAD(&gr->gr_models, gm, gm_link);
  }

  memset(&ec, 0, sizeof(ec));

  r = glw_create_i(gr,
		   GLW_MODEL,
		   GLW_ATTRIB_CAPTION, src,
		   GLW_ATTRIB_PARENT, parent,
		   NULL);
  ec.gr = gr;
  ec.w = r;
  ec.ei = &ei;
  ec.prop0 = prop;
  ec.prop_parent = prop_parent;
  ec.sublist = &ec.w->glw_prop_subscriptions;

  t = glw_model_clone_chain(gm->gm_sof);

  if(glw_model_eval_block(t, &ec)) {
    glw_destroy0(ec.w);
    glw_model_free_chain(t);
    return glw_model_error(gr, &ei, parent);
  }
  glw_model_free_chain(t);
  return r;
}


/**
 *
 */
static int
glw_model_callback(glw_t *w, void *opaque, glw_signal_t signal, void *extra)
{
  glw_t *c = TAILQ_FIRST(&w->glw_childs);

  switch(signal) {
  case GLW_SIGNAL_LAYOUT:
  case GLW_SIGNAL_RENDER:
  case GLW_SIGNAL_EVENT:
    if(c != NULL)
      return glw_signal0(c, signal, extra);
    return 0;

  case GLW_SIGNAL_CHILD_CONSTRAINTS_CHANGED:
    glw_copy_constraints(w, extra);
    return 1;

  default:
    break;
  }
  return 0;
}

/**
 *
 */
void 
glw_model_ctor(glw_t *w, int init, va_list ap)
{
  if(init)
    glw_signal_handler_int(w, glw_model_callback);
}

