/*
 * starter-explain - prompt.h
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

#ifndef ST_EXPLAIN_PROMPT_H_INCLUDED
#define ST_EXPLAIN_PROMPT_H_INCLUDED

#include <glib.h>
#include "state.h"
#include "stex-config.h"
#include "debug-client.h"

G_BEGIN_DECLS

gchar *st_explain_flags_to_text (guint64 flags);
gchar *st_explain_unit_to_text (gint unit);

/*
 * Build user message from template + state.
 * @contexts: GPtrArray of STExplainContext*
 * @changed_name: I/O that triggered
 * @engine_describe: optional RULE/TRIGGER text (NULL if none); @engine_kind used when set
 */
gchar *st_explain_prompt_build (STExplainConfig * cfg, STExplainState * state,
				GPtrArray * contexts,
				const gchar * changed_name,
				STExplainIoKind changed_kind,
				gdouble old_value, gdouble new_value,
				guint64 flags,
				STExplainEngineKind engine_kind,
				const gchar * engine_describe);

G_END_DECLS
#endif /* ST_EXPLAIN_PROMPT_H_INCLUDED */
