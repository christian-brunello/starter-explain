/*
 * starter-explain - audio.h
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#ifndef ST_EXPLAIN_AUDIO_H_INCLUDED
#define ST_EXPLAIN_AUDIO_H_INCLUDED

#include <glib.h>
#include "stex-config.h"

G_BEGIN_DECLS

typedef struct _STExplainAudio STExplainAudio;

STExplainAudio *st_explain_audio_new (STExplainConfig * cfg);
void st_explain_audio_free (STExplainAudio * audio);

/* Play file via configured player command ({{file}}). Sync spawn. */
gboolean st_explain_audio_play (STExplainAudio * audio, const gchar * path,
				GError ** error);

G_END_DECLS
#endif
