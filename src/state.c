/*
 * starter-explain - state.c
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

#include <math.h>
#include <string.h>

#include "state.h"

#define LOGD(msg...) g_log ("state", G_LOG_LEVEL_DEBUG, msg)
#define LOGW(msg...) g_log ("state", G_LOG_LEVEL_WARNING, msg)

typedef struct
{
  gchar *exact;			/* or NULL */
  GRegex *regex;		/* or NULL */
} STExplainMatcher;

typedef struct
{
  STExplainMatcher *matcher;
  gdouble min_delta;
} STExplainDeadbandRule;

struct _STExplainState
{
  STExplainConfig *cfg;		/* not owned */
  GHashTable *ios;		/* name -> STExplainIo* */
  GPtrArray *contexts;		/* STExplainContext* */
  GHashTable *name_to_contexts;	/* name -> GPtrArray of STExplainContext* */
  GPtrArray *ignore_regexes;	/* STExplainMatcher* */
  GPtrArray *ctx_matchers;	/* parallel to cfg contexts: GPtrArray of matchers */
  GPtrArray *deadband_exact;	/* STExplainDeadbandRule* (name=) */
  GPtrArray *deadband_pattern;	/* STExplainDeadbandRule* (pattern=) */
};

static void
matcher_free (gpointer p)
{
  STExplainMatcher *m = p;
  if (m == NULL)
    return;
  g_free (m->exact);
  if (m->regex)
    g_regex_unref (m->regex);
  g_free (m);
}

static void
deadband_rule_free (gpointer p)
{
  STExplainDeadbandRule *r = p;
  if (r == NULL)
    return;
  matcher_free (r->matcher);
  g_free (r);
}

static void
io_free (gpointer p)
{
  STExplainIo *io = p;
  if (io == NULL)
    return;
  g_free (io->name);
  g_free (io->description);
  if (io->history)
    g_array_free (io->history, TRUE);
  g_free (io);
}

static void
context_free (gpointer p)
{
  STExplainContext *c = p;
  if (c == NULL)
    return;
  g_free (c->id);
  g_free (c->name);
  g_free (c->description);
  if (c->io_names)
    g_ptr_array_free (c->io_names, TRUE);
  g_free (c);
}

static void
ptr_array_unref_free (gpointer p)
{
  if (p)
    g_ptr_array_unref (p);
}

static STExplainMatcher *
matcher_from_ref (const STExplainIoRef * ref, GError ** error)
{
  STExplainMatcher *m = g_new0 (STExplainMatcher, 1);

  if (ref->name && *ref->name)
    m->exact = g_strdup (ref->name);
  if (ref->pattern && *ref->pattern)
    {
      m->regex = g_regex_new (ref->pattern, G_REGEX_OPTIMIZE, 0, error);
      if (m->regex == NULL)
	{
	  matcher_free (m);
	  return NULL;
	}
    }
  return m;
}

static gboolean
matcher_matches (STExplainMatcher * m, const gchar * name)
{
  if (m->exact && g_strcmp0 (m->exact, name) == 0)
    return TRUE;
  if (m->regex && g_regex_match (m->regex, name, 0, NULL))
    return TRUE;
  return FALSE;
}

static gboolean
refs_match (GPtrArray * matchers, const gchar * name)
{
  guint i;
  for (i = 0; i < matchers->len; i++)
    {
      if (matcher_matches (matchers->pdata[i], name))
	return TRUE;
    }
  return FALSE;
}

STExplainState *
st_explain_state_new (STExplainConfig * cfg)
{
  STExplainState *state;
  guint i, j;
  GError *err = NULL;

  g_return_val_if_fail (cfg != NULL, NULL);

  state = g_new0 (STExplainState, 1);
  state->cfg = cfg;
  state->ios = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, io_free);
  state->contexts = g_ptr_array_new_with_free_func (context_free);
  state->name_to_contexts =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
			   ptr_array_unref_free);
  state->ignore_regexes = g_ptr_array_new_with_free_func (matcher_free);
  state->ctx_matchers = g_ptr_array_new_with_free_func ((GDestroyNotify)
							g_ptr_array_unref);
  state->deadband_exact =
    g_ptr_array_new_with_free_func (deadband_rule_free);
  state->deadband_pattern =
    g_ptr_array_new_with_free_func (deadband_rule_free);

  for (i = 0; i < cfg->ignore->len; i++)
    {
      STExplainMatcher *m =
	matcher_from_ref (cfg->ignore->pdata[i], &err);
      if (m == NULL)
	{
	  LOGW ("bad ignore pattern: %s", err ? err->message : "?");
	  g_clear_error (&err);
	  continue;
	}
      g_ptr_array_add (state->ignore_regexes, m);
    }

  for (i = 0; i < cfg->deadbands->len; i++)
    {
      STExplainIoRef *ref = cfg->deadbands->pdata[i];
      STExplainMatcher *m;
      STExplainDeadbandRule *rule;

      if (!ref->has_min_delta)
	continue;
      m = matcher_from_ref (ref, &err);
      if (m == NULL)
	{
	  LOGW ("bad deadband pattern: %s", err ? err->message : "?");
	  g_clear_error (&err);
	  continue;
	}
      rule = g_new0 (STExplainDeadbandRule, 1);
      rule->matcher = m;
      rule->min_delta = ref->min_delta;
      if (ref->name && *ref->name)
	g_ptr_array_add (state->deadband_exact, rule);
      else
	g_ptr_array_add (state->deadband_pattern, rule);
    }

  for (i = 0; i < cfg->contexts->len; i++)
    {
      STExplainContextCfg *cc = cfg->contexts->pdata[i];
      STExplainContext *c = g_new0 (STExplainContext, 1);
      GPtrArray *matchers =
	g_ptr_array_new_with_free_func (matcher_free);

      c->id = g_strdup (cc->id);
      c->name = g_strdup (cc->name ? cc->name : cc->id);
      c->description = g_strdup (cc->description ? cc->description : "");
      c->io_names = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (state->contexts, c);

      for (j = 0; j < cc->io_refs->len; j++)
	{
	  STExplainMatcher *m =
	    matcher_from_ref (cc->io_refs->pdata[j], &err);
	  if (m == NULL)
	    {
	      LOGW ("bad context pattern: %s", err ? err->message : "?");
	      g_clear_error (&err);
	      continue;
	    }
	  g_ptr_array_add (matchers, m);
	}
      g_ptr_array_add (state->ctx_matchers, matchers);
    }

  return state;
}

void
st_explain_state_free (STExplainState * state)
{
  if (state == NULL)
    return;
  g_hash_table_destroy (state->ios);
  g_ptr_array_free (state->contexts, TRUE);
  g_hash_table_destroy (state->name_to_contexts);
  g_ptr_array_free (state->ignore_regexes, TRUE);
  g_ptr_array_free (state->ctx_matchers, TRUE);
  g_ptr_array_free (state->deadband_exact, TRUE);
  g_ptr_array_free (state->deadband_pattern, TRUE);
  g_free (state);
}

gboolean
st_explain_state_is_ignored (STExplainState * state, const gchar * name)
{
  guint i;
  g_return_val_if_fail (state != NULL && name != NULL, TRUE);

  for (i = 0; i < state->ignore_regexes->len; i++)
    {
      if (matcher_matches (state->ignore_regexes->pdata[i], name))
	return TRUE;
    }
  return FALSE;
}

void
st_explain_state_rebuild_membership (STExplainState * state)
{
  GHashTableIter iter;
  gpointer key;
  guint i;

  g_hash_table_remove_all (state->name_to_contexts);

  for (i = 0; i < state->contexts->len; i++)
    {
      STExplainContext *c = state->contexts->pdata[i];
      g_ptr_array_set_size (c->io_names, 0);
    }

  g_hash_table_iter_init (&iter, state->ios);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      const gchar *name = key;
      GPtrArray *list = NULL;

      for (i = 0; i < state->contexts->len; i++)
	{
	  GPtrArray *matchers = state->ctx_matchers->pdata[i];
	  STExplainContext *c = state->contexts->pdata[i];

	  if (!refs_match (matchers, name))
	    continue;

	  g_ptr_array_add (c->io_names, g_strdup (name));

	  if (list == NULL)
	    {
	      list = g_ptr_array_new ();
	      g_hash_table_insert (state->name_to_contexts, g_strdup (name),
				   list);
	    }
	  g_ptr_array_add (list, c);
	}
    }
}

static void
io_push_sample (STExplainIo * io, guint hist_len, gdouble value,
		guint64 flags)
{
  STExplainSample sample;

  sample.value = value;
  sample.flags = flags;
  sample.monotonic_ts = g_get_monotonic_time ();

  if (io->history->len < hist_len)
    {
      g_array_set_size (io->history, hist_len);
      memset (&g_array_index (io->history, STExplainSample, 0), 0,
	      sizeof (STExplainSample) * hist_len);
      io->hist_head = 0;
      io->hist_count = 0;
    }

  g_array_index (io->history, STExplainSample, io->hist_head) = sample;
  io->hist_head = (io->hist_head + 1) % hist_len;
  if (io->hist_count < hist_len)
    io->hist_count++;
}

gboolean
st_explain_io_current (const STExplainIo * io, STExplainSample * out)
{
  guint idx;
  guint hist_len;

  if (io == NULL || out == NULL || io->hist_count == 0)
    return FALSE;

  hist_len = io->history->len;
  idx = (io->hist_head + hist_len - 1) % hist_len;
  *out = g_array_index (io->history, STExplainSample, idx);
  return TRUE;
}

gboolean
st_explain_state_upsert (STExplainState * state, const gchar * name,
			 STExplainIoKind kind, const gchar * description,
			 gint unit, gdouble min, gdouble max, gdouble step,
			 gdouble value, guint64 flags,
			 gboolean * explain_out, gdouble * old_value_out,
			 guint64 * old_flags_out)
{
  STExplainIo *io;
  STExplainSample prev;
  gboolean had_prev = FALSE;
  gboolean do_explain = TRUE;
  guint hist_len;

  g_return_val_if_fail (state != NULL && name != NULL, FALSE);

  if (explain_out)
    *explain_out = FALSE;

  if (st_explain_state_is_ignored (state, name))
    {
      g_hash_table_remove (state->ios, name);
      return FALSE;
    }

  hist_len = state->cfg->history_length;
  if (hist_len == 0)
    hist_len = 1;

  io = g_hash_table_lookup (state->ios, name);
  if (io == NULL)
    {
      io = g_new0 (STExplainIo, 1);
      io->name = g_strdup (name);
      io->history = g_array_new (FALSE, TRUE, sizeof (STExplainSample));
      g_hash_table_insert (state->ios, io->name, io);
    }
  else
    {
      had_prev = st_explain_io_current (io, &prev);
    }

  io->kind = kind;
  g_free (io->description);
  io->description = g_strdup (description ? description : "");
  io->unit = unit;
  io->min = min;
  io->max = max;
  io->step = step;

  if (had_prev)
    {
      if (old_value_out)
	*old_value_out = prev.value;
      if (old_flags_out)
	*old_flags_out = prev.flags;

      if (flags == prev.flags &&
	  fabs (value - prev.value) <
	  st_explain_state_min_delta_for (state, name))
	do_explain = FALSE;
    }
  else
    {
      if (old_value_out)
	*old_value_out = value;
      if (old_flags_out)
	*old_flags_out = flags;
      /* First sample: no explain burst from bootstrap unless desired.
         Bootstrap callers can ignore explain_out. */
      do_explain = FALSE;
    }

  io_push_sample (io, hist_len, value, flags);

  if (explain_out)
    *explain_out = do_explain;

  return TRUE;
}

STExplainIo *
st_explain_state_get_io (STExplainState * state, const gchar * name)
{
  g_return_val_if_fail (state != NULL, NULL);
  return g_hash_table_lookup (state->ios, name);
}

GPtrArray *
st_explain_state_contexts_of (STExplainState * state, const gchar * name)
{
  GPtrArray *cached;
  GPtrArray *result;
  guint i;

  g_return_val_if_fail (state != NULL && name != NULL, NULL);

  cached = g_hash_table_lookup (state->name_to_contexts, name);
  if (cached != NULL)
    {
      result = g_ptr_array_new ();
      for (i = 0; i < cached->len; i++)
	g_ptr_array_add (result, cached->pdata[i]);
      return result;
    }

  /* Name may be new: match against context matchers live. */
  result = g_ptr_array_new ();
  for (i = 0; i < state->contexts->len; i++)
    {
      GPtrArray *matchers = state->ctx_matchers->pdata[i];
      if (refs_match (matchers, name))
	g_ptr_array_add (result, state->contexts->pdata[i]);
    }

  if (result->len == 0)
    {
      g_ptr_array_free (result, TRUE);
      return g_ptr_array_new ();
    }

  /* Update index + context io_names */
  {
    GPtrArray *store = g_ptr_array_new ();
    for (i = 0; i < result->len; i++)
      {
	STExplainContext *c = result->pdata[i];
	gboolean found = FALSE;
	guint k;
	g_ptr_array_add (store, c);
	for (k = 0; k < c->io_names->len; k++)
	  {
	    if (g_strcmp0 (c->io_names->pdata[k], name) == 0)
	      {
		found = TRUE;
		break;
	      }
	  }
	if (!found)
	  g_ptr_array_add (c->io_names, g_strdup (name));
      }
    g_hash_table_insert (state->name_to_contexts, g_strdup (name), store);
  }

  return result;
}

GPtrArray *
st_explain_state_merge_io_names (STExplainState * state, GPtrArray * contexts)
{
  GHashTable *seen;
  GPtrArray *names;
  guint i, j;

  (void) state;
  seen = g_hash_table_new (g_str_hash, g_str_equal);
  names = g_ptr_array_new ();

  for (i = 0; i < contexts->len; i++)
    {
      STExplainContext *c = contexts->pdata[i];
      for (j = 0; j < c->io_names->len; j++)
	{
	  const gchar *n = c->io_names->pdata[j];
	  if (g_hash_table_contains (seen, n))
	    continue;
	  g_hash_table_add (seen, (gpointer) n);
	  g_ptr_array_add (names, (gpointer) n);
	}
    }

  g_hash_table_destroy (seen);
  return names;
}

guint
st_explain_state_history_length (STExplainState * state)
{
  return state ? state->cfg->history_length : 0;
}

gdouble
st_explain_state_min_delta_for (STExplainState * state, const gchar * name)
{
  guint i;

  g_return_val_if_fail (state != NULL && name != NULL, 0.0);

  for (i = 0; i < state->deadband_exact->len; i++)
    {
      STExplainDeadbandRule *r = state->deadband_exact->pdata[i];
      if (matcher_matches (r->matcher, name))
	return r->min_delta;
    }
  for (i = 0; i < state->deadband_pattern->len; i++)
    {
      STExplainDeadbandRule *r = state->deadband_pattern->pdata[i];
      if (matcher_matches (r->matcher, name))
	return r->min_delta;
    }
  return state->cfg->min_delta;
}

gdouble
st_explain_state_min_delta (STExplainState * state)
{
  return state ? state->cfg->min_delta : 0.0;
}
