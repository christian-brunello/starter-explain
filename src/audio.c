/*
 * starter-explain - audio.c
 *
 * Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
 */

#include "audio.h"

#include <string.h>

#define LOGI(msg...) g_log ("audio", G_LOG_LEVEL_INFO, msg)
#define LOGW(msg...) g_log ("audio", G_LOG_LEVEL_WARNING, msg)

struct _STExplainAudio
{
  STExplainConfig *cfg;
};

STExplainAudio *
st_explain_audio_new (STExplainConfig * cfg)
{
  STExplainAudio *a = g_new0 (STExplainAudio, 1);
  a->cfg = cfg;
  return a;
}

void
st_explain_audio_free (STExplainAudio * audio)
{
  g_free (audio);
}

gboolean
st_explain_audio_play (STExplainAudio * audio, const gchar * path,
		       GError ** error)
{
  g_autofree gchar *cmd = NULL;
  const gchar *tmpl;
  const gchar *p;
  GString *s;
  const gchar *key = "{{file}}";
  gsize klen;
  gint status = 0;

  g_return_val_if_fail (audio != NULL && path != NULL, FALSE);

  tmpl = audio->cfg->audio_player;
  if (tmpl == NULL || *tmpl == '\0')
    {
      g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED,
		   "no audio player configured");
      return FALSE;
    }

  klen = strlen (key);
  s = g_string_new ("");
  p = tmpl;
  while (*p)
    {
      if (strncmp (p, key, klen) == 0)
	{
	  g_autofree gchar *quoted = g_shell_quote (path);
	  g_string_append (s, quoted);
	  p += klen;
	}
      else
	{
	  g_string_append_c (s, *p);
	  p++;
	}
    }
  cmd = g_string_free (s, FALSE);

  LOGI ("playing: %s", cmd);
  {
    g_autofree gchar *stdout_buf = NULL;
    g_autofree gchar *stderr_buf = NULL;

    if (!g_spawn_command_line_sync
	(cmd, &stdout_buf, &stderr_buf, &status, error))
      return FALSE;

    if (stderr_buf && *stderr_buf)
      LOGW ("player stderr: %s", stderr_buf);

    if (status != 0)
      {
	LOGW ("player exited with status %d", status);
	g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED,
		     "player status %d", status);
	return FALSE;
      }
  }
  return TRUE;
}
