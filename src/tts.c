/*
 * starter-explain - tts.c
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#include "tts.h"
#include "http-queue.h"
#include "wyoming.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <gio/gio.h>

#define LOGW(msg...) g_log ("tts", G_LOG_LEVEL_WARNING, msg)
#define LOGI(msg...) g_log ("tts", G_LOG_LEVEL_INFO, msg)

typedef struct
{
  STExplainTtsDoneFunc done;
  gpointer user_data;
  GDestroyNotify destroy;
  gchar *suffix;
} TtsCb;

typedef struct
{
  gchar *text;
  gchar *coalesce_key;
  STExplainTtsDoneFunc done;
  gpointer user_data;
  GDestroyNotify destroy;
} WyPending;

struct _STExplainTts
{
  STExplainConfig *cfg;
  STExplainHttpQueue *queue;
  /* Wyoming serial queue */
  GMutex wy_lock;
  GQueue *wy_pending;		/* WyPending* */
  gboolean wy_busy;
};

static void wyoming_pump (STExplainTts * tts);

static void
tts_cb_free (gpointer p)
{
  TtsCb *cb = p;
  if (!cb)
    return;
  if (cb->destroy && cb->user_data)
    cb->destroy (cb->user_data);
  g_free (cb->suffix);
  g_free (cb);
}

static void
wy_pending_free (WyPending * p)
{
  if (!p)
    return;
  if (p->destroy && p->user_data)
    p->destroy (p->user_data);
  g_free (p->text);
  g_free (p->coalesce_key);
  g_free (p);
}

static gchar *
json_escape (const gchar * s)
{
  GString *out = g_string_new ("");
  const guchar *p = (const guchar *) (s ? s : "");

  for (; *p; p++)
    {
      switch (*p)
	{
	case '\\':
	  g_string_append (out, "\\\\");
	  break;
	case '"':
	  g_string_append (out, "\\\"");
	  break;
	case '\n':
	  g_string_append (out, "\\n");
	  break;
	case '\r':
	  g_string_append (out, "\\r");
	  break;
	case '\t':
	  g_string_append (out, "\\t");
	  break;
	default:
	  if (*p < 0x20)
	    g_string_append_printf (out, "\\u%04x", *p);
	  else
	    g_string_append_c (out, (gchar) * p);
	  break;
	}
    }
  return g_string_free (out, FALSE);
}

static gchar *
apply_template (const gchar * tmpl, const gchar * text, gboolean uri_escape)
{
  g_autofree gchar *payload = NULL;
  GString *out;
  const gchar *p;
  const gchar *key = "{{text}}";
  gsize klen = strlen (key);

  if (tmpl == NULL)
    return g_strdup ("");

  if (uri_escape)
    payload = g_uri_escape_string (text ? text : "", NULL, FALSE);
  else
    payload = json_escape (text);

  out = g_string_new ("");
  p = tmpl;
  while (*p)
    {
      if (strncmp (p, key, klen) == 0)
	{
	  g_string_append (out, payload);
	  p += klen;
	}
      else
	{
	  g_string_append_c (out, *p);
	  p++;
	}
    }
  return g_string_free (out, FALSE);
}

static const gchar *
suffix_for_response (const gchar * response)
{
  if (response == NULL)
    return ".bin";
  if (strstr (response, "mpeg") || strstr (response, "mp3"))
    return ".mp3";
  if (strstr (response, "ogg") || strstr (response, "opus"))
    return ".ogg";
  if (strstr (response, "wav"))
    return ".wav";
  return ".bin";
}

static gboolean
body_looks_like_audio (const guint8 * body, gsize len, const gchar * content_type)
{
  if (body == NULL || len < 4)
    return FALSE;

  if (content_type != NULL)
    {
      g_autofree gchar *ct = g_ascii_strdown (content_type, -1);
      if (g_str_has_prefix (ct, "text/") ||
	  strstr (ct, "html") != NULL || strstr (ct, "json") != NULL)
	return FALSE;
    }

  if (body[0] == '<' ||
      (len >= 9 &&
       g_ascii_strncasecmp ((const gchar *) body, "<!DOCTYPE", 9) == 0) ||
      (len >= 1 && body[0] == '{'))
    return FALSE;

  if ((body[0] == 'I' && body[1] == 'D' && body[2] == '3') ||
      (body[0] == 0xff && (body[1] & 0xe0) == 0xe0))
    return TRUE;

  if (memcmp (body, "OggS", 4) == 0)
    return TRUE;
  if (len >= 12 && memcmp (body, "RIFF", 4) == 0 &&
      memcmp (body + 8, "WAVE", 4) == 0)
    return TRUE;

  if (content_type != NULL &&
      g_ascii_strncasecmp (content_type, "audio/", 6) == 0)
    return TRUE;

  return FALSE;
}

static void
on_http_done (gboolean ok, const guint8 * body, gsize body_len,
	      const gchar * content_type, const gchar * error_message,
	      gpointer user_data)
{
  TtsCb *cb = user_data;
  g_autofree gchar *path = NULL;
  g_autofree gchar *tmpl = NULL;
  GError *err = NULL;

  if (!ok || body == NULL || body_len == 0)
    {
      if (cb->done)
	cb->done (FALSE, NULL, error_message ? error_message : "tts failed",
		  cb->user_data);
      return;
    }

  if (!body_looks_like_audio (body, body_len, content_type))
    {
      LOGW ("TTS response is not audio (ct=%s len=%" G_GSIZE_FORMAT ")",
	    content_type ? content_type : "(none)", body_len);
      if (cb->done)
	cb->done (FALSE, NULL, "tts response is not audio", cb->user_data);
      return;
    }

  tmpl = g_strdup_printf ("starter-explain-XXXXXX%s",
			  cb->suffix ? cb->suffix : ".bin");
  {
    gint fd = g_file_open_tmp (tmpl, &path, &err);
    if (fd < 0)
      {
	LOGW ("temp file: %s", err ? err->message : "?");
	if (cb->done)
	  cb->done (FALSE, NULL, "temp file", cb->user_data);
	g_clear_error (&err);
	return;
      }
    if (write (fd, body, body_len) < 0)
      {
	close (fd);
	if (cb->done)
	  cb->done (FALSE, NULL, "write failed", cb->user_data);
	return;
      }
    close (fd);
  }

  LOGI ("TTS audio written to %s (%" G_GSIZE_FORMAT " bytes, ct=%s)", path,
	body_len, content_type ? content_type : "?");
  if (cb->done)
    cb->done (TRUE, path, NULL, cb->user_data);
}

static gchar *
clip_tts_text (const gchar * text, guint max_chars)
{
  g_autofree gchar *flat = NULL;
  gchar *p;
  gchar *out;
  glong n;

  if (text == NULL)
    return g_strdup ("");

  flat = g_strdup (text);
  for (p = flat; *p; p++)
    {
      if (*p == '\n' || *p == '\r' || *p == '\t')
	*p = ' ';
    }
  g_strstrip (flat);

  if (max_chars == 0)
    return g_steal_pointer (&flat);

  n = g_utf8_strlen (flat, -1);
  if (n <= (glong) max_chars)
    return g_steal_pointer (&flat);

  out = g_utf8_substring (flat, 0, max_chars);
  LOGW ("TTS text clipped from %ld to %u chars", n, max_chars);
  return out;
}

static gboolean
is_wyoming (STExplainConfig * cfg)
{
  const gchar *m = cfg->tts_method;
  if (m && g_ascii_strcasecmp (m, "wyoming") == 0)
    return TRUE;
  if (cfg->tts_url &&
      (g_str_has_prefix (cfg->tts_url, "tcp://") ||
       g_str_has_prefix (cfg->tts_url, "wyoming://")))
    return TRUE;
  return FALSE;
}

typedef struct
{
  STExplainTts *tts;
  WyPending *job;
  gchar *host;
  guint16 port;
} WyTask;

static void
wy_task_thread (GTask * task, gpointer source_object, gpointer task_data,
		GCancellable * cancellable)
{
  WyTask *wt = task_data;
  GError *err = NULL;
  gchar *path;

  (void) source_object;
  (void) cancellable;

  path =
    st_explain_wyoming_synthesize (wt->host, wt->port, wt->job->text,
				   wt->tts->cfg->tts_timeout_ms, &err);
  if (path == NULL)
    g_task_return_error (task, err);
  else
    g_task_return_pointer (task, path, g_free);
}

static void
wy_task_done (GObject * source, GAsyncResult * result, gpointer user_data)
{
  WyTask *wt = user_data;
  STExplainTts *tts = wt->tts;
  WyPending *job = wt->job;
  GError *err = NULL;
  g_autofree gchar *path = NULL;

  (void) source;
  path = g_task_propagate_pointer (G_TASK (result), &err);
  if (path == NULL)
    {
      LOGW ("wyoming TTS failed: %s", err ? err->message : "?");
      if (job->done)
	job->done (FALSE, NULL, err ? err->message : "wyoming failed",
		   job->user_data);
      g_clear_error (&err);
    }
  else
    {
      LOGI ("wyoming TTS ok: %s", path);
      if (job->done)
	job->done (TRUE, path, NULL, job->user_data);
    }

  /* Don't run destroy here — done callback owns user_data lifetime like HTTP. */
  job->destroy = NULL;
  wy_pending_free (job);
  g_free (wt->host);
  g_free (wt);

  g_mutex_lock (&tts->wy_lock);
  tts->wy_busy = FALSE;
  g_mutex_unlock (&tts->wy_lock);
  wyoming_pump (tts);
}

static void
wyoming_pump (STExplainTts * tts)
{
  WyPending *job;
  WyTask *wt;
  GTask *task;
  GError *err = NULL;

  g_mutex_lock (&tts->wy_lock);
  if (tts->wy_busy || g_queue_is_empty (tts->wy_pending))
    {
      g_mutex_unlock (&tts->wy_lock);
      return;
    }
  job = g_queue_pop_head (tts->wy_pending);
  tts->wy_busy = TRUE;
  g_mutex_unlock (&tts->wy_lock);

  wt = g_new0 (WyTask, 1);
  wt->tts = tts;
  wt->job = job;
  if (!st_explain_wyoming_parse_uri
      (tts->cfg->tts_url, &wt->host, &wt->port, &err))
    {
      LOGW ("wyoming uri: %s", err ? err->message : "?");
      if (job->done)
	job->done (FALSE, NULL, err ? err->message : "bad uri",
		   job->user_data);
      g_clear_error (&err);
      job->destroy = NULL;
      wy_pending_free (job);
      g_free (wt);
      g_mutex_lock (&tts->wy_lock);
      tts->wy_busy = FALSE;
      g_mutex_unlock (&tts->wy_lock);
      wyoming_pump (tts);
      return;
    }

  task = g_task_new (NULL, NULL, wy_task_done, wt);
  g_task_set_task_data (task, wt, NULL);
  g_task_run_in_thread (task, wy_task_thread);
  g_object_unref (task);
}

static gboolean
wyoming_enqueue (STExplainTts * tts, const gchar * text,
		 const gchar * coalesce_key, STExplainTtsDoneFunc done,
		 gpointer user_data, GDestroyNotify destroy)
{
  WyPending *job;
  GList *l;

  g_mutex_lock (&tts->wy_lock);

  if (coalesce_key)
    {
      for (l = tts->wy_pending->head; l; l = l->next)
	{
	  WyPending *ex = l->data;
	  if (ex->coalesce_key &&
	      g_strcmp0 (ex->coalesce_key, coalesce_key) == 0)
	    {
	      if (ex->destroy && ex->user_data)
		ex->destroy (ex->user_data);
	      g_free (ex->text);
	      ex->text = g_strdup (text);
	      ex->done = done;
	      ex->user_data = user_data;
	      ex->destroy = destroy;
	      g_mutex_unlock (&tts->wy_lock);
	      return TRUE;
	    }
	}
    }

  if (g_queue_get_length (tts->wy_pending) >= tts->cfg->tts_max_queue)
    {
      g_mutex_unlock (&tts->wy_lock);
      LOGW ("wyoming queue full, dropping");
      if (destroy && user_data)
	destroy (user_data);
      return FALSE;
    }

  job = g_new0 (WyPending, 1);
  job->text = g_strdup (text);
  job->coalesce_key = g_strdup (coalesce_key);
  job->done = done;
  job->user_data = user_data;
  job->destroy = destroy;
  g_queue_push_tail (tts->wy_pending, job);
  g_mutex_unlock (&tts->wy_lock);

  wyoming_pump (tts);
  return TRUE;
}

STExplainTts *
st_explain_tts_new (STExplainConfig * cfg)
{
  STExplainTts *tts = g_new0 (STExplainTts, 1);
  tts->cfg = cfg;
  tts->queue =
    st_explain_http_queue_new ("tts", cfg->tts_max_queue, cfg->tts_timeout_ms);
  g_mutex_init (&tts->wy_lock);
  tts->wy_pending = g_queue_new ();
  return tts;
}

void
st_explain_tts_free (STExplainTts * tts)
{
  WyPending *p;
  if (tts == NULL)
    return;
  st_explain_http_queue_free (tts->queue);
  while ((p = g_queue_pop_head (tts->wy_pending)) != NULL)
    wy_pending_free (p);
  g_queue_free (tts->wy_pending);
  g_mutex_clear (&tts->wy_lock);
  g_free (tts);
}

gboolean
st_explain_tts_speak (STExplainTts * tts, const gchar * text,
		      const gchar * coalesce_key, STExplainTtsDoneFunc done,
		      gpointer user_data, GDestroyNotify destroy)
{
  g_autofree gchar *body = NULL;
  g_autofree gchar *url = NULL;
  g_autofree gchar *clipped = NULL;
  const gchar *method;
  TtsCb *cb;
  gboolean is_get;

  g_return_val_if_fail (tts != NULL && text != NULL, FALSE);

  if (tts->cfg->tts_url == NULL)
    {
      LOGW ("TTS url not configured");
      if (destroy && user_data)
	destroy (user_data);
      return FALSE;
    }

  clipped = clip_tts_text (text, tts->cfg->tts_max_chars);
  if (clipped == NULL || *clipped == '\0')
    {
      LOGW ("TTS text empty after clip");
      if (destroy && user_data)
	destroy (user_data);
      return FALSE;
    }

  if (is_wyoming (tts->cfg))
    {
      LOGI ("TTS wyoming %s (text_chars=%ld)", tts->cfg->tts_url,
	    (long) g_utf8_strlen (clipped, -1));
      return wyoming_enqueue (tts, clipped, coalesce_key, done, user_data,
			      destroy);
    }

  method = tts->cfg->tts_method ? tts->cfg->tts_method : "POST";
  is_get = (g_ascii_strcasecmp (method, "GET") == 0);

  if (is_get)
    url = apply_template (tts->cfg->tts_url, clipped, TRUE);
  else
    {
      url = g_strdup (tts->cfg->tts_url);
      body = apply_template (tts->cfg->tts_body_template, clipped, FALSE);
    }

  LOGI ("TTS request %s %s (text_chars=%ld)", method, url,
	(long) g_utf8_strlen (clipped, -1));

  cb = g_new0 (TtsCb, 1);
  cb->done = done;
  cb->user_data = user_data;
  cb->destroy = destroy;
  cb->suffix = g_strdup (suffix_for_response (tts->cfg->tts_response));

  return st_explain_http_queue_enqueue (tts->queue, method, url,
					tts->cfg->tts_headers,
					is_get ? NULL : tts->cfg->
					tts_content_type,
					is_get ? NULL : (const guint8 *) body,
					is_get ? 0 : strlen (body),
					coalesce_key, on_http_done, cb,
					tts_cb_free);
}
