/*
 * starter-explain - config.h
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

#ifndef ST_EXPLAIN_CONFIG_H_INCLUDED
#define ST_EXPLAIN_CONFIG_H_INCLUDED

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
  gchar *name;
  gchar *value;
} STExplainHeader;

typedef struct
{
  gchar *name;			/* exact name, or NULL if pattern */
  gchar *pattern;		/* regex pattern, or NULL if name */
  gboolean has_min_delta;	/* TRUE if min-delta attribute was set */
  gdouble min_delta;		/* per-I/O deadband override */
} STExplainIoRef;

typedef struct
{
  gchar *id;
  gchar *name;
  gchar *description;
  GPtrArray *io_refs;		/* STExplainIoRef* */
} STExplainContextCfg;

typedef struct
{
  gchar *host;
  guint16 port;

  gchar *llm_url;
  gchar *llm_model;
  guint llm_timeout_ms;
  guint llm_max_queue;
  gint llm_num_predict;		/* Ollama options.num_predict */
  gint llm_keep_alive;		/* Ollama keep_alive (-1 = forever) */
  GPtrArray *llm_headers;	/* STExplainHeader* */
  gchar *llm_system;
  gchar *llm_user_template;

  gchar *tts_url;		/* may contain {{text}} for GET */
  gchar *tts_method;		/* "GET" or "POST" (default POST) */
  guint tts_timeout_ms;
  guint tts_max_queue;
  guint tts_max_chars;		/* 0 = unlimited; Google TTS ~180 */
  gchar *tts_content_type;
  gchar *tts_body_template;	/* required for POST; unused for GET */
  gchar *tts_response;		/* e.g. audio/mpeg, audio/wav */
  GPtrArray *tts_headers;	/* STExplainHeader* */

  gchar *audio_player;

  GPtrArray *ignore;		/* STExplainIoRef* */
  GPtrArray *contexts;		/* STExplainContextCfg* */
  GPtrArray *deadbands;		/* STExplainIoRef* with has_min_delta */

  guint debounce_ms;
  gboolean coalesce;
  guint history_length;
  gdouble min_delta;		/* global default deadband */
} STExplainConfig;

STExplainConfig *st_explain_config_new (void);
void st_explain_config_free (STExplainConfig * cfg);

gboolean st_explain_config_load (STExplainConfig * cfg, const gchar * path,
				 GError ** error);

/* Override core host:port after load (CLI). */
gboolean st_explain_config_set_core (STExplainConfig * cfg,
				     const gchar * host_port, GError ** error);

G_END_DECLS
#endif /* ST_EXPLAIN_CONFIG_H_INCLUDED */
