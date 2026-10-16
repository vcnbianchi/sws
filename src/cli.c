#include "platform.h"

#include "cli.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SWS_MAX_PORT 65535
#define SWS_MAX_KEEPALIVE_TIMEOUT 3600
#define SWS_DECIMAL_BASE 10

const char *
sws_model_name (sws_model_t model)
{
  switch (model)
    {
    case SWS_MODEL_FORK:
      return "fork";
    case SWS_MODEL_THREAD:
      return "thread";
    case SWS_MODEL_EVENT:
      return "event";
    case SWS_MODEL_NONE:
    default:
      return "none";
    }
}

static void
print_usage (FILE *stream, const char *program)
{
  fprintf (
      stream,
      "Usage: %s -m <model> [options]\n"
      "\n"
      "Minimal HTTP/1.1 static file server used to compare concurrency\n"
      "architectures.\n"
      "\n"
      "Concurrency models (-m, --model):\n"
      "  fork     one process per connection (fork() on POSIX,\n"
      "           CreateProcess() on Windows)\n"
      "  thread   one thread per connection (pthreads on POSIX,\n"
      "           Win32 threads on Windows)\n"
      "  event    single thread, non-blocking I/O and an event loop\n"
      "           (epoll edge-triggered on Linux, poll() elsewhere)\n"
      "\n"
      "Options:\n"
      "  -m, --model <name>        concurrency model (required)\n"
      "  -p, --port <port>         TCP port to listen on (default: %d)\n"
      "  -r, --root <dir>          directory with the static files\n"
      "                            (default: %s)\n"
      "  -b, --backlog <n>         listen() backlog (default: %d)\n"
      "  -k, --keepalive <sec>     idle timeout of persistent connections;\n"
      "                            0 closes every connection after one\n"
      "                            response (default: %d)\n"
      "  -v, --verbose             log every request (slows the server down,\n"
      "                            do not use during benchmarks)\n"
      "  -h, --help                show this help and exit\n"
      "  -V, --version             show the version and exit\n"
      "\n"
      "Example:\n"
      "  %s --model event --port 8080 --root ./www\n",
      program, SWS_DEFAULT_PORT, SWS_DEFAULT_ROOT, SWS_DEFAULT_BACKLOG,
      SWS_DEFAULT_KEEPALIVE_TIMEOUT, program);
}

static int
parse_model (const char *text, sws_model_t *model)
{
  if (strcmp (text, "fork") == 0 || strcmp (text, "process") == 0)
    {
      *model = SWS_MODEL_FORK;
    }
  else if (strcmp (text, "thread") == 0 || strcmp (text, "pthread") == 0)
    {
      *model = SWS_MODEL_THREAD;
    }
  else if (strcmp (text, "event") == 0 || strcmp (text, "epoll") == 0)
    {
      *model = SWS_MODEL_EVENT;
    }
  else
    {
      return -1;
    }
  return 0;
}

static int
parse_int (const char *text, long min_value, long max_value, long *value)
{
  char *end = NULL;
  long parsed;

  errno = 0;
  parsed = strtol (text, &end, SWS_DECIMAL_BASE);
  if (errno != 0 || end == text || *end != '\0' || parsed < min_value
      || parsed > max_value)
    {
      return -1;
    }
  *value = parsed;
  return 0;
}

static int
match_option (int argc, char **argv, int *index, const char *short_name,
              const char *long_name, const char **value)
{
  const char *arg = argv[*index];
  size_t long_length = strlen (long_name);

  if (strcmp (arg, short_name) == 0 || strcmp (arg, long_name) == 0)
    {
      *value = (*index + 1 < argc) ? argv[*index + 1] : NULL;
      if (*value != NULL)
        {
          (*index)++;
        }
      return 1;
    }
  if (strncmp (arg, long_name, long_length) == 0 && arg[long_length] == '=')
    {
      *value = arg + long_length + 1;
      return 1;
    }
  return 0;
}

static int
match_flag (const char *arg, const char *short_name, const char *long_name)
{
  return strcmp (arg, short_name) == 0 || strcmp (arg, long_name) == 0;
}

static int
missing_value (const char *program, const char *option)
{
  fprintf (stderr, "%s: option '%s' requires a value\n", program, option);
  return -1;
}

static int
invalid_value (const char *program, const char *option, const char *value)
{
  fprintf (stderr, "%s: invalid value '%s' for option '%s'\n", program, value,
           option);
  return -1;
}

static int
parse_valued_option (int argc, char **argv, int *index, sws_config_t *cfg)
{
  const char *program = argv[0];
  const char *option = argv[*index];
  const char *value = NULL;
  long number = 0;

  if (match_option (argc, argv, index, "-m", "--model", &value))
    {
      if (value == NULL)
        {
          return missing_value (program, option);
        }
      if (parse_model (value, &cfg->model) != 0)
        {
          return invalid_value (program, option, value);
        }
      return 1;
    }
  if (match_option (argc, argv, index, "-p", "--port", &value))
    {
      if (value == NULL)
        {
          return missing_value (program, option);
        }
      if (parse_int (value, 1, SWS_MAX_PORT, &number) != 0)
        {
          return invalid_value (program, option, value);
        }
      cfg->port = (unsigned short)number;
      return 1;
    }
  if (match_option (argc, argv, index, "-r", "--root", &value))
    {
      if (value == NULL || value[0] == '\0')
        {
          return missing_value (program, option);
        }
      cfg->root = value;
      return 1;
    }
  if (match_option (argc, argv, index, "-b", "--backlog", &value))
    {
      if (value == NULL)
        {
          return missing_value (program, option);
        }
      if (parse_int (value, 1, INT_MAX, &number) != 0)
        {
          return invalid_value (program, option, value);
        }
      cfg->backlog = (int)number;
      return 1;
    }
  if (match_option (argc, argv, index, "-k", "--keepalive", &value))
    {
      if (value == NULL)
        {
          return missing_value (program, option);
        }
      if (parse_int (value, 0, SWS_MAX_KEEPALIVE_TIMEOUT, &number) != 0)
        {
          return invalid_value (program, option, value);
        }
      cfg->keepalive_timeout = (int)number;
      return 1;
    }
  return 0;
}

sws_cli_result_t
sws_cli_parse (int argc, char **argv, sws_config_t *cfg)
{
  const char *program = argc > 0 ? argv[0] : "sws";
  int index;

  cfg->model = SWS_MODEL_NONE;
  cfg->port = SWS_DEFAULT_PORT;
  cfg->backlog = SWS_DEFAULT_BACKLOG;
  cfg->root = SWS_DEFAULT_ROOT;
  cfg->keepalive_timeout = SWS_DEFAULT_KEEPALIVE_TIMEOUT;
  cfg->verbose = 0;
  cfg->worker = 0;

  for (index = 1; index < argc; index++)
    {
      const char *arg = argv[index];
      int handled;

      if (match_flag (arg, "-h", "--help"))
        {
          print_usage (stdout, program);
          return SWS_CLI_EXIT_OK;
        }
      if (match_flag (arg, "-V", "--version"))
        {
          printf ("sws %s\n", SWS_VERSION);
          return SWS_CLI_EXIT_OK;
        }
      if (match_flag (arg, "-v", "--verbose"))
        {
          cfg->verbose = 1;
          continue;
        }
      if (strcmp (arg, "--worker") == 0)
        {
          cfg->worker = 1;
          continue;
        }

      handled = parse_valued_option (argc, argv, &index, cfg);
      if (handled < 0)
        {
          return SWS_CLI_EXIT_FAILURE;
        }
      if (handled == 0)
        {
          fprintf (stderr, "%s: unknown option '%s'\n", program, arg);
          fprintf (stderr, "Try '%s --help' for more information.\n",
                   program);
          return SWS_CLI_EXIT_FAILURE;
        }
    }

  if (cfg->model == SWS_MODEL_NONE)
    {
      fprintf (stderr, "%s: a concurrency model is required (-m fork|thread"
                       "|event)\n\n",
               program);
      print_usage (stderr, program);
      return SWS_CLI_EXIT_FAILURE;
    }
  return SWS_CLI_RUN;
}
