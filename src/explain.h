/*
 * starter-explain - explain.h
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#ifndef ST_EXPLAIN_EXPLAIN_H_INCLUDED
#define ST_EXPLAIN_EXPLAIN_H_INCLUDED

#include <glib.h>
#include "stex-config.h"
#include "state.h"
#include "debug-client.h"

G_BEGIN_DECLS

typedef struct _STExplainService STExplainService;

STExplainService *st_explain_service_new (STExplainConfig * cfg,
					  gboolean dry_run,
					  const gchar * log_text_path);
void st_explain_service_free (STExplainService * svc);

gboolean st_explain_service_start (STExplainService * svc, GError ** error);
void st_explain_service_stop (STExplainService * svc);

/* Wired as debug-client I/O callback */
void st_explain_service_on_io (STExplainDebugClient * client,
			       const gchar * name, STExplainIoKind kind,
			       const gchar * description, gint unit,
			       gdouble min, gdouble max, gdouble step,
			       gdouble value, guint64 flags,
			       gpointer user_data);

/* Wired as debug-client RuleTriggered / TriggerTriggered callback */
void st_explain_service_on_engine (STExplainDebugClient * client,
				   STExplainEngineKind kind,
				   const gchar * dest,
				   const gchar * describe,
				   gpointer user_data);

STExplainDebugClient *st_explain_service_get_client (STExplainService * svc);
STExplainState *st_explain_service_get_state (STExplainService * svc);

G_END_DECLS
#endif
