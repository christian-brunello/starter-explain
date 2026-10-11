/*
 * starter-explain - debug-client.h
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#ifndef ST_EXPLAIN_DEBUG_CLIENT_H_INCLUDED
#define ST_EXPLAIN_DEBUG_CLIENT_H_INCLUDED

#include <glib.h>
#include <gio/gio.h>
#include "state.h"

G_BEGIN_DECLS

typedef struct _STExplainDebugClient STExplainDebugClient;

typedef enum
{
  ST_EXPLAIN_ENGINE_RULE = 0,
  ST_EXPLAIN_ENGINE_TRIGGER
} STExplainEngineKind;

typedef void (*STExplainIoChangedFunc) (STExplainDebugClient * client,
					const gchar * name,
					STExplainIoKind kind,
					const gchar * description,
					gint unit, gdouble min, gdouble max,
					gdouble step, gdouble value,
					guint64 flags, gpointer user_data);

typedef void (*STExplainEngineTriggeredFunc) (STExplainDebugClient * client,
					      STExplainEngineKind kind,
					      const gchar * dest,
					      const gchar * describe,
					      gpointer user_data);

STExplainDebugClient *st_explain_debug_client_new (void);
void st_explain_debug_client_free (STExplainDebugClient * client);

void st_explain_debug_client_set_io_callback (STExplainDebugClient * client,
					     STExplainIoChangedFunc cb,
					     gpointer user_data);

void st_explain_debug_client_set_engine_callback (STExplainDebugClient *
						  client,
						  STExplainEngineTriggeredFunc
						  cb, gpointer user_data);

gboolean st_explain_debug_client_connect (STExplainDebugClient * client,
					  const gchar * host, guint16 port,
					  GError ** error);
void st_explain_debug_client_disconnect (STExplainDebugClient * client);
gboolean st_explain_debug_client_is_connected (STExplainDebugClient * client);

/* Snapshot ListInputs/ListOutputs into callback (same as signals). */
gboolean st_explain_debug_client_refresh_io (STExplainDebugClient * client,
					     GError ** error);

gboolean st_explain_debug_client_log_status (STExplainDebugClient * client,
					     GError ** error);

G_END_DECLS
#endif
