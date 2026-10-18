/*
 * starter-explain - prompt.c
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

#include <string.h>

#include "prompt.h"

/* Mirror starter/defs.h without requiring starter-core linkage. */
#define ST_IO_FLAG_MANUAL_OVERRIDE 0x0000000000000001ULL
#define ST_IO_FLAG_NO_HISTORY      0x0000000000000002ULL
#define ST_IO_FLAG_USR1            0x1000000000000000ULL
#define ST_IO_FLAG_USR2            0x2000000000000000ULL
#define ST_IO_FLAG_USR3            0x4000000000000000ULL
#define ST_IO_FLAG_USR4            0x8000000000000000ULL

enum
{
  ST_UNIT_NULL = 0,
  ST_UNIT_CELSIUS,
  ST_UNIT_PERCENT,
  ST_UNIT_BAR,
  ST_UNIT_ONOFF,
  ST_UNIT_ENUM,
  ST_UNIT_V,
  ST_UNIT_W,
  ST_UNIT_VAR,
  ST_UNIT_KW,
  ST_UNIT_WH,
  ST_UNIT_KWH,
  ST_UNIT_SECONDS,
  ST_UNIT_MINUTES,
  ST_UNIT_HOURS
};

gchar *
st_explain_flags_to_text (guint64 flags)
{
  GString *s = g_string_new ("");

  if (flags == 0)
    {
      g_string_append (s, "none");
      return g_string_free (s, FALSE);
    }

  if (flags & ST_IO_FLAG_MANUAL_OVERRIDE)
    g_string_append (s, "manual_override");
  if (flags & ST_IO_FLAG_NO_HISTORY)
    {
      if (s->len)
	g_string_append_c (s, ',');
      g_string_append (s, "no_history");
    }
  if (flags & ST_IO_FLAG_USR1)
    {
      if (s->len)
	g_string_append_c (s, ',');
      g_string_append (s, "usr1");
    }
  if (flags & ST_IO_FLAG_USR2)
    {
      if (s->len)
	g_string_append_c (s, ',');
      g_string_append (s, "usr2");
    }
  if (flags & ST_IO_FLAG_USR3)
    {
      if (s->len)
	g_string_append_c (s, ',');
      g_string_append (s, "usr3");
    }
  if (flags & ST_IO_FLAG_USR4)
    {
      if (s->len)
	g_string_append_c (s, ',');
      g_string_append (s, "usr4");
    }

  if (s->len == 0)
    g_string_append_printf (s, "0x%llx", (unsigned long long) flags);

  return g_string_free (s, FALSE);
}

gchar *
st_explain_unit_to_text (gint unit)
{
  switch (unit)
    {
    case ST_UNIT_NULL:
      return g_strdup ("null");
    case ST_UNIT_CELSIUS:
      return g_strdup ("celsius");
    case ST_UNIT_PERCENT:
      return g_strdup ("percent");
    case ST_UNIT_BAR:
      return g_strdup ("bar");
    case ST_UNIT_ONOFF:
      return g_strdup ("onoff");
    case ST_UNIT_ENUM:
      return g_strdup ("enum");
    case ST_UNIT_V:
      return g_strdup ("V");
    case ST_UNIT_W:
      return g_strdup ("W");
    case ST_UNIT_VAR:
      return g_strdup ("VAR");
    case ST_UNIT_KW:
      return g_strdup ("kW");
    case ST_UNIT_WH:
      return g_strdup ("Wh");
    case ST_UNIT_KWH:
      return g_strdup ("kWh");
    case ST_UNIT_SECONDS:
      return g_strdup ("seconds");
    case ST_UNIT_MINUTES:
      return g_strdup ("minutes");
    case ST_UNIT_HOURS:
      return g_strdup ("hours");
    default:
      return g_strdup_printf ("unit_%d", unit);
    }
}

static gchar *
replace_all (const gchar * template, const gchar * key, const gchar * value)
{
  GString *out;
  const gchar *p;
  gsize klen;

  if (template == NULL)
    return g_strdup ("");
  if (key == NULL || value == NULL)
    return g_strdup (template);

  klen = strlen (key);
  out = g_string_new ("");
  p = template;
  while (*p)
    {
      if (strncmp (p, key, klen) == 0)
	{
	  g_string_append (out, value);
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

static gchar *
build_contexts_block (GPtrArray * contexts)
{
  GString *s = g_string_new ("");
  guint i;

  if (contexts == NULL || contexts->len == 0)
    return g_string_free (s, FALSE);

  for (i = 0; i < contexts->len; i++)
    {
      STExplainContext *c = contexts->pdata[i];
      if (i)
	g_string_append (s, "; ");
      if (c->name && *c->name)
	g_string_append (s, c->name);
      else if (c->id)
	g_string_append (s, c->id);
      if (c->description && *c->description)
	g_string_append_printf (s, " - %s", c->description);
    }
  return g_string_free (s, FALSE);
}

static void
collect_ids_from_rule (const gchar * rule, GHashTable * set)
{
  const gchar *p;

  if (rule == NULL || set == NULL)
    return;

  for (p = rule; *p; p++)
    {
      const gchar *start;
      const gchar *colon;
      const gchar *end;
      g_autofree gchar *id = NULL;

      if (!(g_ascii_isalnum (*p) || *p == '_'))
	continue;
      start = p;
      while (g_ascii_isalnum (*p) || *p == '_' || *p == ':')
	p++;
      end = p;
      if (end <= start)
	continue;
      id = g_strndup (start, end - start);
      colon = strchr (id, ':');
      if (colon && colon > id && colon[1] != '\0' && strchr (colon + 1, ':') == NULL)
	g_hash_table_add (set, g_steal_pointer (&id));
      if (*p == '\0')
	break;
      p--;
    }
}

static gchar *
format_io_line (STExplainState * state, const gchar * name,
		gboolean with_history)
{
  STExplainIo *io;
  STExplainSample cur;
  g_autofree gchar *unit = NULL;
  GString *s;
  guint k;

  io = st_explain_state_get_io (state, name);
  if (io == NULL || !st_explain_io_current (io, &cur))
    return NULL;

  unit = st_explain_unit_to_text (io->unit);
  s = g_string_new ("");
  g_string_append_printf (s, "- %s [%s]: %g %s",
			  name,
			  io->kind ==
			  ST_EXPLAIN_IO_INPUT ? "input" : "output", cur.value,
			  unit);
  if (io->description && *io->description)
    g_string_append_printf (s, " - %s", io->description);
  g_string_append_c (s, '\n');

  if (with_history && io->hist_count > 1)
    {
      guint hist_len = io->history->len;
      g_string_append (s, "  history:");
      for (k = 0; k < io->hist_count; k++)
	{
	  guint idx =
	    (io->hist_head + hist_len - io->hist_count + k) % hist_len;
	  STExplainSample *sm =
	    &g_array_index (io->history, STExplainSample, idx);
	  g_string_append_printf (s, " %g", sm->value);
	}
      g_string_append_c (s, '\n');
    }

  return g_string_free (s, FALSE);
}

static void
append_state_line (GString * s, STExplainState * state, const gchar * name,
		   GHashTable * seen)
{
  g_autofree gchar *line = NULL;

  if (name == NULL || name[0] == '\0')
    return;
  if (g_hash_table_contains (seen, name))
    return;
  g_hash_table_add (seen, g_strdup (name));
  line = format_io_line (state, name, TRUE);
  if (line)
    g_string_append (s, line);
}

static gchar *
build_compact_state (STExplainState * state, GPtrArray * contexts,
		     const gchar * changed_name,
		     STExplainIoKind changed_kind,
		     const gchar * engine_describe)
{
  GString *s = g_string_new ("");
  g_autoptr (GHashTable) seen = NULL;
  g_autoptr (GHashTable) rule_ids = NULL;
  g_autoptr (GPtrArray) merged = NULL;
  GHashTableIter iter;
  gpointer key;
  guint i;

  seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* Changed I/O first, then other members of its context(s) (stable order). */
  append_state_line (s, state, changed_name, seen);

  if (contexts && contexts->len > 0)
    {
      merged = st_explain_state_merge_io_names (state, contexts);
      for (i = 0; i < merged->len; i++)
	append_state_line (s, state, merged->pdata[i], seen);
    }

  /* Action: also include any Service:io ids referenced by RULE text. */
  if (changed_kind == ST_EXPLAIN_IO_OUTPUT && engine_describe &&
      *engine_describe)
    {
      rule_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      collect_ids_from_rule (engine_describe, rule_ids);
      g_hash_table_iter_init (&iter, rule_ids);
      while (g_hash_table_iter_next (&iter, &key, NULL))
	append_state_line (s, state, key, seen);
    }

  return g_string_free (s, FALSE);
}

static gchar *
build_event (const gchar * changed_name, STExplainIoKind changed_kind,
	     gdouble old_value, gdouble new_value, const gchar * unit,
	     const gchar * description)
{
  GString *s = g_string_new ("");

  g_string_append_printf (s, "%s (%s) from %g to %g", changed_name,
			  changed_kind ==
			  ST_EXPLAIN_IO_INPUT ? "input" : "output", old_value,
			  new_value);
  if (unit && *unit && g_strcmp0 (unit, "null") != 0)
    g_string_append_printf (s, " (%s)", unit);
  if (description && *description)
    g_string_append_printf (s, ": %s", description);
  return g_string_free (s, FALSE);
}

gchar *
st_explain_prompt_build (STExplainConfig * cfg, STExplainState * state,
			 GPtrArray * contexts, const gchar * changed_name,
			 STExplainIoKind changed_kind, gdouble old_value,
			 gdouble new_value, guint64 flags,
			 STExplainEngineKind engine_kind,
			 const gchar * engine_describe)
{
  g_autofree gchar *contexts_list = NULL;
  g_autofree gchar *context_block = NULL;
  g_autofree gchar *snapshot = NULL;
  g_autofree gchar *unit = NULL;
  g_autofree gchar *flags_text = NULL;
  g_autofree gchar *old_s = NULL;
  g_autofree gchar *new_s = NULL;
  g_autofree gchar *kind = NULL;
  g_autofree gchar *type = NULL;
  g_autofree gchar *event = NULL;
  g_autofree gchar *rule = NULL;
  g_autofree gchar *reason_alias = NULL;
  gchar *text;
  STExplainIo *io;

  contexts_list = build_contexts_block (contexts);
  context_block = g_strdup (contexts_list);
  snapshot =
    build_compact_state (state, contexts, changed_name, changed_kind,
			 engine_describe);

  io = st_explain_state_get_io (state, changed_name);
  unit = st_explain_unit_to_text (io ? io->unit : 0);
  flags_text = st_explain_flags_to_text (flags);
  old_s = g_strdup_printf ("%g", old_value);
  new_s = g_strdup_printf ("%g", new_value);
  kind =
    g_strdup (changed_kind ==
	      ST_EXPLAIN_IO_INPUT ? "input" : "output");
  event =
    build_event (changed_name, changed_kind, old_value, new_value, unit,
		 io && io->description ? io->description : NULL);

  if (engine_describe && *engine_describe)
    {
      if (engine_kind == ST_EXPLAIN_ENGINE_TRIGGER &&
	  !g_str_has_prefix (engine_describe, "TRIGGER"))
	rule = g_strdup_printf ("TRIGGER %s", engine_describe);
      else
	rule = g_strdup (engine_describe);
    }
  else
    rule = g_strdup ("none");

  /* action only when an engine RULE/TRIGGER is present; else event + RULE: none */
  type =
    g_strdup ((engine_describe && *engine_describe) ? "action" : "event");

  /* Legacy {{reason}} / {{motivo}}: compact join for old templates */
  reason_alias =
    g_strdup_printf ("%s\nCONTEXT: %s\nSTATE:\n%s", rule,
		     context_block && *context_block ? context_block : "none",
		     snapshot && *snapshot ? snapshot : "(empty)");

  text = g_strdup (cfg->llm_user_template);
  {
    gchar *t;
    const gchar *desc =
      (io && io->description && *io->description) ? io->description : "";

    t = replace_all (text, "{{type}}", type);
    g_free (text);
    text = t;
    t = replace_all (text, "{{event}}", event);
    g_free (text);
    text = t;
    t = replace_all (text, "{{rule}}", rule);
    g_free (text);
    text = t;
    t =
      replace_all (text, "{{context}}",
		   context_block && *context_block ? context_block : "none");
    g_free (text);
    text = t;
    t =
      replace_all (text, "{{state}}",
		   snapshot && *snapshot ? snapshot : "(empty)");
    g_free (text);
    text = t;
    t = replace_all (text, "{{reason}}", reason_alias);
    g_free (text);
    text = t;
    t = replace_all (text, "{{evento}}", event);
    g_free (text);
    text = t;
    t = replace_all (text, "{{motivo}}", reason_alias);
    g_free (text);
    text = t;
    t = replace_all (text, "{{contexts.list}}", contexts_list);
    g_free (text);
    text = t;
    t = replace_all (text, "{{changed.name}}", changed_name);
    g_free (text);
    text = t;
    t = replace_all (text, "{{changed.kind}}", kind);
    g_free (text);
    text = t;
    t = replace_all (text, "{{changed.old}}", old_s);
    g_free (text);
    text = t;
    t = replace_all (text, "{{changed.new}}", new_s);
    g_free (text);
    text = t;
    t = replace_all (text, "{{changed.description}}", desc);
    g_free (text);
    text = t;
    t = replace_all (text, "{{changed.unit}}", unit);
    g_free (text);
    text = t;
    t = replace_all (text, "{{changed.flags}}", flags_text);
    g_free (text);
    text = t;
    t = replace_all (text, "{{snapshot}}", snapshot ? snapshot : "");
    g_free (text);
    text = t;
    /* drop leftover {{task}} if present */
    t = replace_all (text, "{{task}}", "");
    g_free (text);
    text = t;
  }

  return text;
}
