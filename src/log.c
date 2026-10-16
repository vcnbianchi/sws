#include "platform.h"

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SWS_LOG_LINE_SIZE 1024
#define SWS_ERROR_TEXT_SIZE 256

static void
write_line (FILE *stream, const char *level, const char *format,
            va_list args)
{
  char line[SWS_LOG_LINE_SIZE];
  size_t length;
  int prefix = snprintf (line, sizeof (line) - 1, "%s | ", level);

  if (prefix < 0)
    {
      return;
    }
  vsnprintf (line + prefix, sizeof (line) - 1 - (size_t)prefix, format, args);
  length = strlen (line);
  line[length] = '\n';
  line[length + 1] = '\0';
  fputs (line, stream);
}

void
sws_log_init (void)
{
#ifdef _WIN32
  setvbuf (stdout, NULL, _IONBF, 0);
#else
  setvbuf (stdout, NULL, _IOLBF, BUFSIZ);
#endif
  setvbuf (stderr, NULL, _IONBF, 0);
}

void
sws_log_info (const char *format, ...)
{
  va_list args;

  va_start (args, format);
  write_line (stdout, "INFO", format, args);
  va_end (args);
}

void
sws_log_error (const char *format, ...)
{
  va_list args;

  va_start (args, format);
  write_line (stderr, "ERROR", format, args);
  va_end (args);
}

void
sws_log_socket_error (const char *what)
{
  char text[SWS_ERROR_TEXT_SIZE];

  sws_socket_error_string (sws_socket_last_error (), text, sizeof (text));
  sws_log_error ("%s: %s", what, text);
}
