#include "platform.h"

#include "cli.h"
#include "config.h"
#include "log.h"
#include "models.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>

const char *
sws_model_mechanism (sws_model_t model)
{
  switch (model)
    {
    case SWS_MODEL_FORK:
#ifdef _WIN32
      return "CreateProcess() per connection (Windows has no fork)";
#else
      return "fork() per connection, children reaped on SIGCHLD";
#endif
    case SWS_MODEL_THREAD:
#ifdef _WIN32
      return "_beginthreadex() per connection";
#else
      return "pthread_create() per connection (detached)";
#endif
    case SWS_MODEL_EVENT:
#if defined(__linux__)
      return "single thread, non-blocking sockets, epoll edge-triggered";
#elif defined(_WIN32)
      return "single thread, non-blocking sockets, WSAPoll()";
#else
      return "single thread, non-blocking sockets, poll()";
#endif
    case SWS_MODEL_NONE:
    default:
      return "none";
    }
}

static void
show_welcome_message (void)
{
  printf ("\n");
  printf ("=============================================\n");
  printf ("SWS - Simple Web Server %s\n", SWS_VERSION);
  printf ("=============================================\n");
  printf ("\n");
}

static int
validate_root (const char *root)
{
  sws_stat_t info;

  if (sws_stat (root, &info) != 0 || !SWS_IS_DIR (info.st_mode))
    {
      sws_log_error ("document root '%s' is not an accessible directory",
                     root);
      return -1;
    }
  return 0;
}

static int
run_model (sws_socket_t listen_socket, const sws_config_t *cfg)
{
  switch (cfg->model)
    {
    case SWS_MODEL_FORK:
      return sws_model_fork_run (listen_socket, cfg);
    case SWS_MODEL_THREAD:
      return sws_model_thread_run (listen_socket, cfg);
    case SWS_MODEL_EVENT:
      return sws_model_event_run (listen_socket, cfg);
    case SWS_MODEL_NONE:
    default:
      return -1;
    }
}

static void
show_configuration (const sws_config_t *cfg, long fd_limit)
{
  sws_log_info ("platform: %s, pid %ld", SWS_PLATFORM_NAME,
                sws_platform_pid ());
  sws_log_info ("model: %s - %s", sws_model_name (cfg->model),
                sws_model_mechanism (cfg->model));
  sws_log_info ("document root: %s", cfg->root);
  if (cfg->keepalive_timeout > 0)
    {
      sws_log_info ("keep-alive: %d s, backlog: %d", cfg->keepalive_timeout,
                    cfg->backlog);
    }
  else
    {
      sws_log_info ("keep-alive: disabled, backlog: %d", cfg->backlog);
    }
  if (fd_limit > 0)
    {
      sws_log_info ("open file limit: %ld", fd_limit);
    }
  if (cfg->verbose)
    {
      sws_log_info ("verbose logging enabled (do not use for benchmarks)");
    }
}

static int
start_http_server (const sws_config_t *cfg)
{
  sws_socket_t listen_socket;
  long fd_limit;
  int result;

  if (validate_root (cfg->root) != 0)
    {
      return -1;
    }

  fd_limit = sws_platform_raise_fd_limit ();
  listen_socket = sws_net_listen (cfg->port, cfg->backlog);
  if (listen_socket == SWS_INVALID_SOCKET)
    {
      return -1;
    }

  show_configuration (cfg, fd_limit);
  sws_platform_install_stop_handler (listen_socket);
  sws_log_info ("server listening at %d...", cfg->port);

  result = run_model (listen_socket, cfg);

  if (sws_platform_forget_stop_socket ())
    {
      sws_socket_close (listen_socket);
    }
  sws_log_info ("server stopped");
  return result;
}

int
main (int argc, char **argv)
{
  sws_config_t cfg;
  int result;

  switch (sws_cli_parse (argc, argv, &cfg))
    {
    case SWS_CLI_EXIT_OK:
      return EXIT_SUCCESS;
    case SWS_CLI_EXIT_FAILURE:
      return EXIT_FAILURE;
    case SWS_CLI_RUN:
    default:
      break;
    }

  sws_log_init ();
  if (sws_platform_init () != 0)
    {
      sws_log_error ("platform initialization failed");
      return EXIT_FAILURE;
    }

  if (cfg.worker)
    {
      result = sws_model_fork_worker (&cfg);
    }
  else
    {
      show_welcome_message ();
      result = start_http_server (&cfg);
    }

  sws_platform_cleanup ();
  return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
