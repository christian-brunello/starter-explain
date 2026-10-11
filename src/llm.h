/*
 * starter-explain - llm.h
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#ifndef ST_EXPLAIN_LLM_H_INCLUDED
#define ST_EXPLAIN_LLM_H_INCLUDED

#include <glib.h>
#include "stex-config.h"
#include "http-queue.h"

G_BEGIN_DECLS

typedef struct _STExplainLlm STExplainLlm;

typedef void (*STExplainLlmDoneFunc) (gboolean ok, const gchar * text,
				      const gchar * error_message,
				      gpointer user_data);

STExplainLlm *st_explain_llm_new (STExplainConfig * cfg);
void st_explain_llm_free (STExplainLlm * llm);

/* Enqueue chat completion. coalesce_key optional. Returns FALSE if queue full. */
gboolean st_explain_llm_ask (STExplainLlm * llm, const gchar * system,
			     const gchar * user, const gchar * coalesce_key,
			     STExplainLlmDoneFunc done, gpointer user_data,
			     GDestroyNotify destroy);

G_END_DECLS
#endif
