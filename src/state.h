/*
 * starter-explain - state.h
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

#ifndef ST_EXPLAIN_STATE_H_INCLUDED
#define ST_EXPLAIN_STATE_H_INCLUDED

#include <glib.h>
#include "stex-config.h"

G_BEGIN_DECLS

typedef enum
{
  ST_EXPLAIN_IO_INPUT = 0,
  ST_EXPLAIN_IO_OUTPUT = 1
} STExplainIoKind;

typedef struct
{
  gdouble value;
  guint64 flags;
  gint64 monotonic_ts;		/* g_get_monotonic_time() */
} STExplainSample;

typedef struct
{
  gchar *name;
  STExplainIoKind kind;
  gchar *description;
  gint unit;
  gdouble min;
  gdouble max;
  gdouble step;
  GArray *history;		/* STExplainSample, ring via head/count */
  guint hist_head;		/* next write index */
  guint hist_count;
} STExplainIo;

typedef struct
{
  gchar *id;
  gchar *name;
  gchar *description;
  GPtrArray *io_names;		/* gchar* owned */
} STExplainContext;

typedef struct _STExplainState STExplainState;

STExplainState *st_explain_state_new (STExplainConfig * cfg);
void st_explain_state_free (STExplainState * state);

/* Rebuild membership patterns against known names (call after upserts). */
void st_explain_state_rebuild_membership (STExplainState * state);

gboolean st_explain_state_is_ignored (STExplainState * state,
				      const gchar * name);

/*
 * Upsert I/O into global map (ignored names are skipped / removed).
 * Returns TRUE if explain should be considered (passed deadband).
 * old_value/old_flags filled when previous sample existed.
 */
gboolean st_explain_state_upsert (STExplainState * state, const gchar * name,
				  STExplainIoKind kind,
				  const gchar * description, gint unit,
				  gdouble min, gdouble max, gdouble step,
				  gdouble value, guint64 flags,
				  gboolean * explain_out,
				  gdouble * old_value_out,
				  guint64 * old_flags_out);

STExplainIo *st_explain_state_get_io (STExplainState * state,
				      const gchar * name);

/* Returns GPtrArray of STExplainContext* (not owned) matching name. */
GPtrArray *st_explain_state_contexts_of (STExplainState * state,
					 const gchar * name);

/*
 * Build merged set of io names for contexts in @contexts (STExplainContext*).
 * Returns GPtrArray of const gchar* names (not strdup'd; do not free elements).
 */
GPtrArray *st_explain_state_merge_io_names (STExplainState * state,
					    GPtrArray * contexts);

guint st_explain_state_history_length (STExplainState * state);
/* Effective deadband for @name: first matching <deadband>/<io min-delta>, else global. */
gdouble st_explain_state_min_delta_for (STExplainState * state,
					const gchar * name);
gdouble st_explain_state_min_delta (STExplainState * state);

/* Current sample helpers */
gboolean st_explain_io_current (const STExplainIo * io, STExplainSample * out);

G_END_DECLS
#endif /* ST_EXPLAIN_STATE_H_INCLUDED */
