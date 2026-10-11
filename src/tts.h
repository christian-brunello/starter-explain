/*
 * starter-explain - tts.h
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#ifndef ST_EXPLAIN_TTS_H_INCLUDED
#define ST_EXPLAIN_TTS_H_INCLUDED

#include <glib.h>
#include "stex-config.h"

G_BEGIN_DECLS

typedef struct _STExplainTts STExplainTts;

typedef void (*STExplainTtsDoneFunc) (gboolean ok, const gchar * audio_path,
				      const gchar * error_message,
				      gpointer user_data);

STExplainTts *st_explain_tts_new (STExplainConfig * cfg);
void st_explain_tts_free (STExplainTts * tts);

gboolean st_explain_tts_speak (STExplainTts * tts, const gchar * text,
			       const gchar * coalesce_key,
			       STExplainTtsDoneFunc done, gpointer user_data,
			       GDestroyNotify destroy);

G_END_DECLS
#endif
