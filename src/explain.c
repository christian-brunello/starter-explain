/*
 * starter-explain - explain.c
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#include "explain.h"
#include "prompt.h"
#include "llm.h"
#include "tts.h"
#include "audio.h"

#include <glib/gstdio.h>
#include <string.h>

#define LOGI(msg...) g_log ("explain", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log ("explain", G_LOG_LEVEL_WARNING, msg)
#define LOGD(msg...) g_log ("explain", G_LOG_LEVEL_DEBUG, msg)

typedef struct
{
  gchar *coalesce_key;
  gchar *changed_name;
  STExplainIoKind kind;
  gdouble old_value;
  gdouble new_value;
  guint64 flags;
  GPtrArray *contexts;		/* STExplainContext* not owned */
  STExplainEngineKind engine_kind;
  gchar *engine_describe;	/* stolen from rule cache; may be NULL */
  guint timeout_id;
} PendingExplain;

typedef struct
{
  STExplainEngineKind kind;
  gchar *describe;
} EngineCacheEntry;

struct _STExplainService
{
  STExplainConfig *cfg;
  gboolean dry_run;
  gchar *log_text_path;		/* optional append-file for replies */
  STExplainState *state;
  STExplainDebugClient *client;
  STExplainLlm *llm;
  STExplainTts *tts;
  STExplainAudio *audio;
  GHashTable *debounce;		/* coalesce_key -> PendingExplain* */
  GHashTable *rule_by_dest;	/* dest -> EngineCacheEntry* */
};

typedef struct
{
  STExplainService *svc;
  gchar *io;			/* changed I/O that triggered explain */
  gchar *system;		/* system string sent to LLM / app.py */
  gchar *prompt;		/* user prompt (stex template) sent to app.py */
} LlmAskCb;

static void
engine_cache_entry_free (gpointer p)
{
  EngineCacheEntry *e = p;
  if (!e)
    return;
  g_free (e->describe);
  g_free (e);
}

static void
pending_free (gpointer p)
{
  PendingExplain *pe = p;
  if (!pe)
    return;
  if (pe->timeout_id)
    g_source_remove (pe->timeout_id);
  g_free (pe->coalesce_key);
  g_free (pe->changed_name);
  g_free (pe->engine_describe);
  if (pe->contexts)
    g_ptr_array_free (pe->contexts, TRUE);
  g_free (pe);
}

/* Steal cached RULE/TRIGGER describe for @dest (consume-once). */
static gchar *
rule_cache_take (STExplainService * svc, const gchar * dest,
		 STExplainEngineKind * kind_out)
{
  EngineCacheEntry *e;
  gchar *describe;

  if (kind_out)
    *kind_out = ST_EXPLAIN_ENGINE_RULE;
  if (dest == NULL || svc->rule_by_dest == NULL)
    return NULL;

  e = g_hash_table_lookup (svc->rule_by_dest, dest);
  if (e == NULL)
    return NULL;

  if (kind_out)
    *kind_out = e->kind;
  describe = e->describe;
  e->describe = NULL;
  g_hash_table_remove (svc->rule_by_dest, dest);
  return describe;
}

void
st_explain_service_on_engine (STExplainDebugClient * client,
			      STExplainEngineKind kind, const gchar * dest,
			      const gchar * describe, gpointer user_data)
{
  STExplainService *svc = user_data;
  EngineCacheEntry *e;

  (void) client;
  if (dest == NULL || *dest == '\0')
    return;

  e = g_new0 (EngineCacheEntry, 1);
  e->kind = kind;
  e->describe = g_strdup (describe ? describe : "");
  g_hash_table_replace (svc->rule_by_dest, g_strdup (dest), e);
  LOGD ("cached %s for %s: %s",
	kind == ST_EXPLAIN_ENGINE_TRIGGER ? "TRIGGER" : "RULE", dest,
	e->describe);
}

static gint
cmp_str_ptr (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 (*(const gchar * const *) a, *(const gchar * const *) b);
}

static gchar *
contexts_fingerprint (GPtrArray * contexts)
{
  GPtrArray *ids;
  guint i;
  GString *s;

  ids = g_ptr_array_new_with_free_func (g_free);
  for (i = 0; i < contexts->len; i++)
    {
      STExplainContext *c = contexts->pdata[i];
      g_ptr_array_add (ids, g_strdup (c->id));
    }
  g_ptr_array_sort (ids, cmp_str_ptr);

  s = g_string_new ("");
  for (i = 0; i < ids->len; i++)
    {
      if (i)
	g_string_append_c (s, '+');
      g_string_append (s, ids->pdata[i]);
    }
  g_ptr_array_free (ids, TRUE);
  return g_string_free (s, FALSE);
}

static void
llm_ask_cb_free (gpointer p)
{
  LlmAskCb *cb = p;
  if (!cb)
    return;
  g_free (cb->io);
  g_free (cb->system);
  g_free (cb->prompt);
  g_free (cb);
}

static gchar *
build_reason (PendingExplain * pe)
{
  GString *s;
  guint i;
  const gchar *kind =
    pe->kind == ST_EXPLAIN_IO_INPUT ? "input" : "output";

  s = g_string_new ("");
  g_string_append_printf (s, "%s (%s) %.6g -> %.6g",
			  pe->changed_name, kind, pe->old_value,
			  pe->new_value);
  if (pe->contexts && pe->contexts->len > 0)
    {
      g_string_append (s, " [");
      for (i = 0; i < pe->contexts->len; i++)
	{
	  STExplainContext *c = pe->contexts->pdata[i];
	  if (i)
	    g_string_append (s, ", ");
	  g_string_append (s, c->name && *c->name ? c->name : c->id);
	}
      g_string_append_c (s, ']');
    }
  return g_string_free (s, FALSE);
}

static void
append_text_log (STExplainService * svc, const gchar * io,
		 const gchar * system, const gchar * prompt,
		 const gchar * response)
{
  g_autoptr (GDateTime) now = NULL;
  g_autofree gchar *ts = NULL;
  FILE *fp;

  if (svc->log_text_path == NULL || *svc->log_text_path == '\0')
    return;

  now = g_date_time_new_now_local ();
  ts = g_date_time_format_iso8601 (now);

  fp = g_fopen (svc->log_text_path, "a");
  if (fp == NULL)
    {
      LOGW ("cannot append to --log-text %s", svc->log_text_path);
      return;
    }

  /* One iteration: timestamp, trigger I/O, request to app.py, response.
   * Entries separated by a blank line (double newline). */
  fprintf (fp, "%s\n", ts ? ts : "?");
  fprintf (fp, "io: %s\n", io ? io : "?");
  fprintf (fp, "request:\n");
  fprintf (fp, "system:\n%s\n", system && *system ? system : "(none)");
  fprintf (fp, "prompt:\n%s\n", prompt && *prompt ? prompt : "(empty)");
  fprintf (fp, "response: %s\n", response ? response : "");
  fprintf (fp, "\n");
  fflush (fp);
  fclose (fp);
}

static void
on_tts_done (gboolean ok, const gchar * audio_path, const gchar * error_message,
	     gpointer user_data)
{
  STExplainService *svc = user_data;
  GError *err = NULL;

  if (!ok)
    {
      LOGW ("TTS failed: %s", error_message ? error_message : "?");
      return;
    }

  if (svc->dry_run)
    {
      LOGI ("dry-run: skip audio play (%s)", audio_path);
      if (audio_path)
	g_unlink (audio_path);
      return;
    }

  if (!st_explain_audio_play (svc->audio, audio_path, &err))
    {
      LOGW ("audio play failed: %s", err ? err->message : "?");
      g_clear_error (&err);
    }
  if (audio_path)
    g_unlink (audio_path);
}

static void
on_llm_done (gboolean ok, const gchar * text, const gchar * error_message,
	     gpointer user_data)
{
  LlmAskCb *cb = user_data;
  STExplainService *svc = cb->svc;

  if (!ok || text == NULL || *text == '\0')
    {
      g_autofree gchar *err =
	g_strdup_printf ("(LLM failed: %s)",
			 error_message ? error_message : "?");
      LOGW ("LLM failed: %s", error_message ? error_message : "?");
      append_text_log (svc, cb->io, cb->system, cb->prompt, err);
      return;
    }

  LOGI ("explanation: %s", text);
  append_text_log (svc, cb->io, cb->system, cb->prompt, text);

  if (svc->dry_run)
    {
      LOGI ("dry-run: skip TTS");
      return;
    }

  if (svc->cfg->tts_url == NULL)
    {
      LOGW ("no TTS configured");
      return;
    }

  if (!st_explain_tts_speak (svc->tts, text, NULL, on_tts_done, svc, NULL))
    LOGW ("TTS queue full, dropped");
}

static void
run_explain (STExplainService * svc, PendingExplain * pe)
{
  g_autofree gchar *prompt = NULL;
  LlmAskCb *cb;

  prompt =
    st_explain_prompt_build (svc->cfg, svc->state, pe->contexts,
			     pe->changed_name, pe->kind, pe->old_value,
			     pe->new_value, pe->flags, pe->engine_kind,
			     pe->engine_describe);

  LOGD ("prompt:\n%s", prompt);

  cb = g_new0 (LlmAskCb, 1);
  cb->svc = svc;
  cb->io = build_reason (pe);
  cb->system = g_strdup (svc->cfg->llm_system ? svc->cfg->llm_system : "");
  cb->prompt = g_strdup (prompt ? prompt : "");

  if (!st_explain_llm_ask
      (svc->llm, svc->cfg->llm_system, prompt, pe->coalesce_key, on_llm_done,
       cb, llm_ask_cb_free))
    {
      append_text_log (svc, cb->io, cb->system, cb->prompt,
		       "(dropped: LLM queue full)");
      llm_ask_cb_free (cb);
      LOGW ("LLM queue full, dropped explain for %s", pe->changed_name);
    }
}

typedef struct
{
  STExplainService *svc;
  gchar *key;
} DebounceData;

static gboolean
on_debounce (gpointer user_data)
{
  DebounceData *dd = user_data;
  STExplainService *svc = dd->svc;
  PendingExplain *pe;

  pe = g_hash_table_lookup (svc->debounce, dd->key);
  if (pe)
    {
      pe->timeout_id = 0;
      g_hash_table_steal (svc->debounce, dd->key);
      run_explain (svc, pe);
      pending_free (pe);
    }

  g_free (dd->key);
  g_free (dd);
  return G_SOURCE_REMOVE;
}

STExplainService *
st_explain_service_new (STExplainConfig * cfg, gboolean dry_run,
			const gchar * log_text_path)
{
  STExplainService *svc = g_new0 (STExplainService, 1);

  svc->cfg = cfg;
  svc->dry_run = dry_run;
  svc->log_text_path = g_strdup (log_text_path);
  svc->state = st_explain_state_new (cfg);
  svc->client = st_explain_debug_client_new ();
  svc->llm = st_explain_llm_new (cfg);
  svc->tts = st_explain_tts_new (cfg);
  svc->audio = st_explain_audio_new (cfg);
  svc->debounce =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, pending_free);
  svc->rule_by_dest =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
			   engine_cache_entry_free);

  st_explain_debug_client_set_io_callback (svc->client,
					   st_explain_service_on_io, svc);
  st_explain_debug_client_set_engine_callback (svc->client,
					       st_explain_service_on_engine,
					       svc);
  return svc;
}

void
st_explain_service_free (STExplainService * svc)
{
  if (svc == NULL)
    return;
  st_explain_service_stop (svc);
  g_hash_table_destroy (svc->debounce);
  g_hash_table_destroy (svc->rule_by_dest);
  st_explain_audio_free (svc->audio);
  st_explain_tts_free (svc->tts);
  st_explain_llm_free (svc->llm);
  st_explain_debug_client_free (svc->client);
  st_explain_state_free (svc->state);
  g_free (svc->log_text_path);
  g_free (svc);
}

gboolean
st_explain_service_start (STExplainService * svc, GError ** error)
{
  g_return_val_if_fail (svc != NULL, FALSE);

  if (!st_explain_debug_client_connect
      (svc->client, svc->cfg->host, svc->cfg->port, error))
    return FALSE;

  st_explain_debug_client_log_status (svc->client, NULL);

  if (!st_explain_debug_client_refresh_io (svc->client, error))
    {
      LOGW ("ListInputs/Outputs failed: %s",
	    error && *error ? (*error)->message : "?");
      g_clear_error (error);
    }

  st_explain_state_rebuild_membership (svc->state);
  LOGI ("explain service started (dry_run=%s log_text=%s)",
	svc->dry_run ? "true" : "false",
	svc->log_text_path ? svc->log_text_path : "(none)");
  return TRUE;
}

void
st_explain_service_stop (STExplainService * svc)
{
  if (svc == NULL)
    return;
  st_explain_debug_client_disconnect (svc->client);
  g_hash_table_remove_all (svc->debounce);
  g_hash_table_remove_all (svc->rule_by_dest);
}

STExplainDebugClient *
st_explain_service_get_client (STExplainService * svc)
{
  return svc ? svc->client : NULL;
}

STExplainState *
st_explain_service_get_state (STExplainService * svc)
{
  return svc ? svc->state : NULL;
}

void
st_explain_service_on_io (STExplainDebugClient * client, const gchar * name,
			  STExplainIoKind kind, const gchar * description,
			  gint unit, gdouble min, gdouble max, gdouble step,
			  gdouble value, guint64 flags, gpointer user_data)
{
  STExplainService *svc = user_data;
  gboolean do_explain = FALSE;
  gdouble old_value = 0;
  guint64 old_flags = 0;
  GPtrArray *contexts = NULL;
  PendingExplain *pe;
  PendingExplain *existing;
  g_autofree gchar *fp = NULL;
  DebounceData *dd;
  guint i;

  (void) client;
  (void) old_flags;

  if (!st_explain_state_upsert
      (svc->state, name, kind, description, unit, min, max, step, value,
       flags, &do_explain, &old_value, &old_flags))
    return;

  if (!do_explain)
    return;

  st_explain_state_rebuild_membership (svc->state);
  contexts = st_explain_state_contexts_of (svc->state, name);
  if (contexts == NULL || contexts->len == 0)
    {
      LOGD ("no context for %s, skip explain", name);
      if (contexts)
	g_ptr_array_free (contexts, TRUE);
      return;
    }

  fp = contexts_fingerprint (contexts);

  pe = g_new0 (PendingExplain, 1);
  pe->coalesce_key = g_strdup (fp);
  pe->changed_name = g_strdup (name);
  pe->kind = kind;
  pe->old_value = old_value;
  pe->new_value = value;
  pe->flags = flags;
  pe->contexts = g_ptr_array_new ();
  for (i = 0; i < contexts->len; i++)
    g_ptr_array_add (pe->contexts, contexts->pdata[i]);
  g_ptr_array_free (contexts, TRUE);
  pe->engine_describe =
    rule_cache_take (svc, name, &pe->engine_kind);

  if (svc->cfg->debounce_ms == 0)
    {
      run_explain (svc, pe);
      pending_free (pe);
      return;
    }

  existing = g_hash_table_lookup (svc->debounce, fp);
  if (existing && existing->timeout_id)
    {
      g_source_remove (existing->timeout_id);
      existing->timeout_id = 0;
    }

  g_hash_table_replace (svc->debounce, g_strdup (fp), pe);

  dd = g_new0 (DebounceData, 1);
  dd->svc = svc;
  dd->key = g_strdup (fp);
  pe->timeout_id = g_timeout_add (svc->cfg->debounce_ms, on_debounce, dd);
}
