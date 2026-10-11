/*
 * starter-explain - http-queue.c
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "stex-config.h"
#include "http-queue.h"

#include <string.h>
#include <libsoup/soup.h>

typedef struct
{
  gchar *method;		/* GET / POST */
  gchar *url;
  GPtrArray *headers;		/* STExplainHeader* copies */
  gchar *content_type;
  guint8 *body;
  gsize body_len;
  gchar *coalesce_key;
  STExplainHttpDoneFunc done;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
} STExplainHttpJob;

struct _STExplainHttpQueue
{
  gchar *log_domain;
  guint max_queue;
  guint timeout_ms;
  GQueue *pending;		/* STExplainHttpJob* */
  gboolean in_flight;
  SoupSession *session;
  guint timeout_source;
  STExplainHttpJob *current;
};

static void
header_copy_free (gpointer p)
{
  STExplainHeader *h = p;
  if (!h)
    return;
  g_free (h->name);
  g_free (h->value);
  g_free (h);
}

static GPtrArray *
headers_dup (GPtrArray * src)
{
  GPtrArray *out;
  guint i;

  out = g_ptr_array_new_with_free_func (header_copy_free);
  if (src == NULL)
    return out;
  for (i = 0; i < src->len; i++)
    {
      STExplainHeader *h = src->pdata[i];
      STExplainHeader *c = g_new0 (STExplainHeader, 1);
      c->name = g_strdup (h->name);
      c->value = g_strdup (h->value);
      g_ptr_array_add (out, c);
    }
  return out;
}

static void
job_free (STExplainHttpJob * job)
{
  if (job == NULL)
    return;
  g_free (job->method);
  g_free (job->url);
  if (job->headers)
    g_ptr_array_free (job->headers, TRUE);
  g_free (job->content_type);
  g_free (job->body);
  g_free (job->coalesce_key);
  if (job->user_data_destroy && job->user_data)
    job->user_data_destroy (job->user_data);
  g_free (job);
}

static void st_explain_http_queue_pump (STExplainHttpQueue * q);

static void
clear_timeout (STExplainHttpQueue * q)
{
  if (q->timeout_source)
    {
      g_source_remove (q->timeout_source);
      q->timeout_source = 0;
    }
}

static gboolean
on_timeout (gpointer user_data)
{
  STExplainHttpQueue *q = user_data;
  STExplainHttpJob *job = q->current;

  q->timeout_source = 0;
  g_log (q->log_domain, G_LOG_LEVEL_WARNING, "HTTP request timed out");

  if (job && job->done)
    job->done (FALSE, NULL, 0, NULL, "timeout", job->user_data);

  job_free (job);
  q->current = NULL;
  q->in_flight = FALSE;
  st_explain_http_queue_pump (q);
  return G_SOURCE_REMOVE;
}

#ifdef HAVE_LIBSOUP_3
static void
on_send_done (GObject * source, GAsyncResult * result, gpointer user_data)
{
  STExplainHttpQueue *q = user_data;
  STExplainHttpJob *job = q->current;
  GError *error = NULL;
  GInputStream *stream;
  GByteArray *buf;
  guint8 tmp[4096];
  gssize n;
  SoupMessage *msg = NULL;

  clear_timeout (q);

  stream = soup_session_send_finish (SOUP_SESSION (source), result, &error);
  if (stream == NULL)
    {
      g_log (q->log_domain, G_LOG_LEVEL_WARNING, "HTTP send failed: %s",
	     error ? error->message : "?");
      if (job && job->done)
	job->done (FALSE, NULL, 0, NULL,
		   error ? error->message : "send failed", job->user_data);
      g_clear_error (&error);
      goto done;
    }

  msg = g_object_get_data (G_OBJECT (source), "st-explain-msg");

  buf = g_byte_array_new ();
  while ((n = g_input_stream_read (stream, tmp, sizeof (tmp), NULL, &error))
	 > 0)
    g_byte_array_append (buf, tmp, (guint) n);

  g_object_unref (stream);

  if (n < 0)
    {
      g_log (q->log_domain, G_LOG_LEVEL_WARNING, "HTTP read failed: %s",
	     error ? error->message : "?");
      if (job && job->done)
	job->done (FALSE, NULL, 0, NULL,
		   error ? error->message : "read failed", job->user_data);
      g_clear_error (&error);
      g_byte_array_unref (buf);
      goto done;
    }

  if (msg != NULL)
    {
      guint status = soup_message_get_status (msg);
      if (status < 200 || status >= 300)
	{
	  gchar snippet[161];
	  gsize snip = MIN (buf->len, (gsize) 160);
	  memcpy (snippet, buf->data, snip);
	  snippet[snip] = '\0';
	  for (gsize i = 0; i < snip; i++)
	    if ((guchar) snippet[i] < 0x20)
	      snippet[i] = ' ';
	  g_log (q->log_domain, G_LOG_LEVEL_WARNING,
		 "HTTP status %u url=%s body=%s", status,
		 job && job->url ? job->url : "?", snippet);
	  if (job && job->done)
	    job->done (FALSE, NULL, 0, NULL, "bad status", job->user_data);
	  g_byte_array_unref (buf);
	  goto done;
	}
    }

  {
    const gchar *resp_ct = NULL;
    if (msg != NULL)
      resp_ct =
	soup_message_headers_get_content_type (soup_message_get_response_headers
					       (msg), NULL);
    if (job && job->done)
      job->done (TRUE, buf->data, buf->len, resp_ct, NULL, job->user_data);
  }
  g_byte_array_unref (buf);

done:
  job_free (job);
  q->current = NULL;
  q->in_flight = FALSE;
  g_object_set_data (G_OBJECT (q->session), "st-explain-msg", NULL);
  st_explain_http_queue_pump (q);
}

static void
start_job (STExplainHttpQueue * q, STExplainHttpJob * job)
{
  SoupMessage *msg;
  const gchar *method;
  guint i;

  q->current = job;
  q->in_flight = TRUE;

  method = (job->method && *job->method) ? job->method : "POST";
  msg = soup_message_new (method, job->url);
  if (msg == NULL)
    {
      if (job->done)
	job->done (FALSE, NULL, 0, NULL, "bad url", job->user_data);
      job_free (job);
      q->current = NULL;
      q->in_flight = FALSE;
      st_explain_http_queue_pump (q);
      return;
    }

  for (i = 0; i < job->headers->len; i++)
    {
      STExplainHeader *h = job->headers->pdata[i];
      soup_message_headers_append (soup_message_get_request_headers (msg),
				   h->name, h->value);
    }

  if (g_ascii_strcasecmp (method, "GET") != 0 && job->body_len > 0)
    {
      GBytes *bytes = g_bytes_new (job->body, job->body_len);
      soup_message_set_request_body_from_bytes (msg,
						job->content_type ? job->
						content_type :
						"application/octet-stream",
						bytes);
      g_bytes_unref (bytes);
    }

  g_object_set_data_full (G_OBJECT (q->session), "st-explain-msg",
			  g_object_ref (msg), g_object_unref);

  if (q->timeout_ms > 0)
    q->timeout_source =
      g_timeout_add (q->timeout_ms, on_timeout, q);

  soup_session_send_async (q->session, msg, G_PRIORITY_DEFAULT, NULL,
			   on_send_done, q);
  g_object_unref (msg);
}

#else /* libsoup-2.4 */

static void
on_soup2_done (SoupSession * session, SoupMessage * msg, gpointer user_data)
{
  STExplainHttpQueue *q = user_data;
  STExplainHttpJob *job = q->current;
  SoupBuffer *buf = NULL;

  (void) session;
  clear_timeout (q);

  if (msg->status_code < 200 || msg->status_code >= 300)
    {
      g_log (q->log_domain, G_LOG_LEVEL_WARNING, "HTTP status %u",
	     msg->status_code);
      if (job && job->done)
	job->done (FALSE, NULL, 0, NULL, "bad status", job->user_data);
    }
  else
    {
      const gchar *resp_ct =
	soup_message_headers_get_content_type (msg->response_headers, NULL);
      buf = soup_message_body_flatten (msg->response_body);
      if (job && job->done)
	job->done (TRUE, (const guint8 *) buf->data, buf->length,
		   resp_ct, NULL, job->user_data);
      if (buf)
	soup_buffer_free (buf);
    }

  job_free (job);
  q->current = NULL;
  q->in_flight = FALSE;
  st_explain_http_queue_pump (q);
}

static void
start_job (STExplainHttpQueue * q, STExplainHttpJob * job)
{
  SoupMessage *msg;
  const gchar *method;
  guint i;

  q->current = job;
  q->in_flight = TRUE;

  method = (job->method && *job->method) ? job->method : "POST";
  msg = soup_message_new (method, job->url);
  if (msg == NULL)
    {
      if (job->done)
	job->done (FALSE, NULL, 0, NULL, "bad url", job->user_data);
      job_free (job);
      q->current = NULL;
      q->in_flight = FALSE;
      st_explain_http_queue_pump (q);
      return;
    }

  for (i = 0; i < job->headers->len; i++)
    {
      STExplainHeader *h = job->headers->pdata[i];
      soup_message_headers_append (msg->request_headers, h->name, h->value);
    }

  if (g_ascii_strcasecmp (method, "GET") != 0 && job->body_len > 0)
    soup_message_set_request (msg,
			      job->content_type ? job->
			      content_type : "application/octet-stream",
			      SOUP_MEMORY_COPY, (const gchar *) job->body,
			      job->body_len);

  if (q->timeout_ms > 0)
    q->timeout_source = g_timeout_add (q->timeout_ms, on_timeout, q);

  soup_session_queue_message (q->session, msg, on_soup2_done, q);
}
#endif

static void
st_explain_http_queue_pump (STExplainHttpQueue * q)
{
  STExplainHttpJob *job;

  if (q->in_flight)
    return;
  job = g_queue_pop_head (q->pending);
  if (job == NULL)
    return;
  start_job (q, job);
}

STExplainHttpQueue *
st_explain_http_queue_new (const gchar * log_domain, guint max_queue,
			   guint timeout_ms)
{
  STExplainHttpQueue *q = g_new0 (STExplainHttpQueue, 1);

  q->log_domain = g_strdup (log_domain ? log_domain : "http");
  q->max_queue = max_queue > 0 ? max_queue : 1;
  q->timeout_ms = timeout_ms;
  q->pending = g_queue_new ();
  q->session = soup_session_new ();
  return q;
}

void
st_explain_http_queue_free (STExplainHttpQueue * q)
{
  STExplainHttpJob *job;

  if (q == NULL)
    return;
  clear_timeout (q);
  while ((job = g_queue_pop_head (q->pending)) != NULL)
    job_free (job);
  g_queue_free (q->pending);
  if (q->current)
    job_free (q->current);
  g_clear_object (&q->session);
  g_free (q->log_domain);
  g_free (q);
}

gboolean
st_explain_http_queue_enqueue (STExplainHttpQueue * q, const gchar * method,
			       const gchar * url, GPtrArray * headers,
			       const gchar * content_type,
			       const guint8 * body, gsize body_len,
			       const gchar * coalesce_key,
			       STExplainHttpDoneFunc done, gpointer user_data,
			       GDestroyNotify user_data_destroy)
{
  STExplainHttpJob *job;
  GList *l;
  const gchar *m = (method && *method) ? method : "POST";

  g_return_val_if_fail (q != NULL && url != NULL, FALSE);

  /* Coalesce: replace matching pending job */
  if (coalesce_key)
    {
      for (l = q->pending->head; l; l = l->next)
	{
	  STExplainHttpJob *existing = l->data;
	  if (existing->coalesce_key &&
	      g_strcmp0 (existing->coalesce_key, coalesce_key) == 0)
	    {
	      g_free (existing->method);
	      existing->method = g_strdup (m);
	      g_free (existing->url);
	      existing->url = g_strdup (url);
	      if (existing->headers)
		g_ptr_array_free (existing->headers, TRUE);
	      existing->headers = headers_dup (headers);
	      g_free (existing->content_type);
	      existing->content_type = g_strdup (content_type);
	      g_free (existing->body);
	      existing->body = g_malloc (body_len ? body_len : 1);
	      if (body_len && body)
		memcpy (existing->body, body, body_len);
	      existing->body_len = body_len;
	      if (existing->user_data_destroy && existing->user_data)
		existing->user_data_destroy (existing->user_data);
	      existing->done = done;
	      existing->user_data = user_data;
	      existing->user_data_destroy = user_data_destroy;
	      return TRUE;
	    }
	}
    }

  if (g_queue_get_length (q->pending) >= q->max_queue)
    {
      g_log (q->log_domain, G_LOG_LEVEL_WARNING,
	     "queue full (max=%u), dropping event", q->max_queue);
      if (user_data_destroy && user_data)
	user_data_destroy (user_data);
      return FALSE;
    }

  job = g_new0 (STExplainHttpJob, 1);
  job->method = g_strdup (m);
  job->url = g_strdup (url);
  job->headers = headers_dup (headers);
  job->content_type = g_strdup (content_type);
  job->body = g_malloc (body_len ? body_len : 1);
  if (body_len && body)
    memcpy (job->body, body, body_len);
  job->body_len = body_len;
  job->coalesce_key = g_strdup (coalesce_key);
  job->done = done;
  job->user_data = user_data;
  job->user_data_destroy = user_data_destroy;

  g_queue_push_tail (q->pending, job);
  st_explain_http_queue_pump (q);
  return TRUE;
}

guint
st_explain_http_queue_length (STExplainHttpQueue * q)
{
  return q ? g_queue_get_length (q->pending) : 0;
}
