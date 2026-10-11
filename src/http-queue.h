/*
 * starter-explain - http-queue.h
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef ST_EXPLAIN_HTTP_QUEUE_H_INCLUDED
#define ST_EXPLAIN_HTTP_QUEUE_H_INCLUDED

#include <glib.h>
#include "stex-config.h"

G_BEGIN_DECLS

typedef struct _STExplainHttpQueue STExplainHttpQueue;

typedef void (*STExplainHttpDoneFunc) (gboolean ok,
				       const guint8 * body,
				       gsize body_len,
				       const gchar * content_type,
				       const gchar * error_message,
				       gpointer user_data);

/*
 * Serial HTTP POST queue. max_queue = waiting slots (not counting in-flight).
 * If full, enqueue returns FALSE (drop).
 * coalesce_key: if non-NULL and an identical key is already queued (not
 * in-flight), replace that entry's payload instead of growing the queue.
 */
STExplainHttpQueue *st_explain_http_queue_new (const gchar * log_domain,
					       guint max_queue,
					       guint timeout_ms);
void st_explain_http_queue_free (STExplainHttpQueue * q);

gboolean st_explain_http_queue_enqueue (STExplainHttpQueue * q,
					const gchar * method,
					const gchar * url,
					GPtrArray * headers,
					const gchar * content_type,
					const guint8 * body, gsize body_len,
					const gchar * coalesce_key,
					STExplainHttpDoneFunc done,
					gpointer user_data,
					GDestroyNotify user_data_destroy);

guint st_explain_http_queue_length (STExplainHttpQueue * q);

G_END_DECLS
#endif /* ST_EXPLAIN_HTTP_QUEUE_H_INCLUDED */
