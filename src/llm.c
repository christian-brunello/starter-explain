/*
 * starter-explain - llm.c
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#include "llm.h"

#include <json-glib/json-glib.h>
#include <string.h>

#define LOGI(msg...) g_log ("llm", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log ("llm", G_LOG_LEVEL_WARNING, msg)

struct _STExplainLlm
{
  STExplainConfig *cfg;
  STExplainHttpQueue *queue;
};

typedef struct
{
  STExplainLlmDoneFunc done;
  gpointer user_data;
  GDestroyNotify destroy;
} LlmCb;

static void
llm_cb_free (gpointer p)
{
  LlmCb *cb = p;
  if (cb && cb->destroy && cb->user_data)
    cb->destroy (cb->user_data);
  g_free (cb);
}

static const gchar *
extract_reply_text (JsonObject * obj)
{
  const gchar *content = NULL;

  if (obj == NULL)
    return NULL;

  /* Ollama /api/generate */
  if (json_object_has_member (obj, "response"))
    {
      content = json_object_get_string_member (obj, "response");
      if (content && *content)
	return content;
    }

  /* OpenAI-compatible /v1/chat/completions */
  if (json_object_has_member (obj, "choices"))
    {
      JsonArray *choices = json_object_get_array_member (obj, "choices");
      if (choices && json_array_get_length (choices) > 0)
	{
	  JsonObject *choice = json_array_get_object_element (choices, 0);
	  if (choice && json_object_has_member (choice, "message"))
	    {
	      JsonObject *message =
		json_object_get_object_member (choice, "message");
	      if (message && json_object_has_member (message, "content"))
		content = json_object_get_string_member (message, "content");
	    }
	}
    }

  return content;
}

static void
on_http_done (gboolean ok, const guint8 * body, gsize body_len,
	      const gchar * content_type, const gchar * error_message,
	      gpointer user_data)
{
  LlmCb *cb = user_data;
  g_autofree gchar *text = NULL;
  JsonParser *parser;
  GError *err = NULL;
  const gchar *content;

  (void) content_type;

  if (!ok || body == NULL)
    {
      if (cb->done)
	cb->done (FALSE, NULL, error_message ? error_message : "http failed",
		  cb->user_data);
      return;
    }

  parser = json_parser_new ();
  if (!json_parser_load_from_data
      (parser, (const gchar *) body, (gssize) body_len, &err))
    {
      LOGW ("JSON parse failed: %s", err ? err->message : "?");
      if (cb->done)
	cb->done (FALSE, NULL, err ? err->message : "json", cb->user_data);
      g_clear_error (&err);
      g_object_unref (parser);
      return;
    }

  content = extract_reply_text (json_node_get_object (json_parser_get_root (parser)));
  if (content == NULL || *content == '\0')
    {
      LOGW ("no response text in LLM JSON");
      if (cb->done)
	cb->done (FALSE, NULL, "bad response shape", cb->user_data);
    }
  else
    {
      text = g_strdup (content);
      g_strstrip (text);
      LOGI ("LLM reply: %s", text);
      if (cb->done)
	cb->done (TRUE, text, NULL, cb->user_data);
    }

  g_object_unref (parser);
}

STExplainLlm *
st_explain_llm_new (STExplainConfig * cfg)
{
  STExplainLlm *llm = g_new0 (STExplainLlm, 1);
  llm->cfg = cfg;
  llm->queue =
    st_explain_http_queue_new ("llm", cfg->llm_max_queue, cfg->llm_timeout_ms);
  return llm;
}

void
st_explain_llm_free (STExplainLlm * llm)
{
  if (llm == NULL)
    return;
  st_explain_http_queue_free (llm->queue);
  g_free (llm);
}

gboolean
st_explain_llm_ask (STExplainLlm * llm, const gchar * system,
		    const gchar * user, const gchar * coalesce_key,
		    STExplainLlmDoneFunc done, gpointer user_data,
		    GDestroyNotify destroy)
{
  JsonBuilder *b;
  JsonGenerator *gen;
  JsonNode *root;
  gchar *json;
  gsize len;
  LlmCb *cb;

  g_return_val_if_fail (llm != NULL && user != NULL, FALSE);

  /* Ollama /api/generate body */
  b = json_builder_new ();
  json_builder_begin_object (b);
  if (llm->cfg->llm_model)
    {
      json_builder_set_member_name (b, "model");
      json_builder_add_string_value (b, llm->cfg->llm_model);
    }
  if (system && *system)
    {
      json_builder_set_member_name (b, "system");
      json_builder_add_string_value (b, system);
    }
  json_builder_set_member_name (b, "prompt");
  json_builder_add_string_value (b, user);
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, FALSE);
  json_builder_set_member_name (b, "keep_alive");
  /* Seconds; stex keep-alive=-1 means forever (Ollama). */
  if (llm->cfg->llm_keep_alive < 0)
    json_builder_add_string_value (b, "-1");
  else
    json_builder_add_int_value (b, llm->cfg->llm_keep_alive);
  json_builder_set_member_name (b, "options");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "num_predict");
  json_builder_add_int_value (b, llm->cfg->llm_num_predict);
  json_builder_set_member_name (b, "temperature");
  json_builder_add_double_value (b, 0.2);
  json_builder_set_member_name (b, "num_ctx");
  json_builder_add_int_value (b, 2048);
  json_builder_end_object (b);
  json_builder_end_object (b);

  root = json_builder_get_root (b);
  gen = json_generator_new ();
  json_generator_set_root (gen, root);
  json = json_generator_to_data (gen, &len);
  json_node_unref (root);
  g_object_unref (gen);
  g_object_unref (b);

  cb = g_new0 (LlmCb, 1);
  cb->done = done;
  cb->user_data = user_data;
  cb->destroy = destroy;

  {
    gboolean ok =
      st_explain_http_queue_enqueue (llm->queue, "POST", llm->cfg->llm_url,
				     llm->cfg->llm_headers,
				     "application/json",
				     (const guint8 *) json, len,
				     coalesce_key, on_http_done, cb,
				     llm_cb_free);
    g_free (json);
    return ok;
  }
}
