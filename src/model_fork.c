#include "platform.h"

#include "connection.h"
#include "log.h"
#include "models.h"
#include "net.h"

#include <stdlib.h>
#include <string.h>

#define SWS_FORK_BACKOFF_MS 10

#ifndef _WIN32

#include <signal.h>
#include <sys/wait.h>

static void
reap_children (int signal_number)
{
  int saved_errno = errno;

  (void)signal_number;
  while (waitpid (-1, NULL, WNOHANG) > 0)
    {
    }
  errno = saved_errno;
}

static int
install_sigchld_handler (void)
{
  struct sigaction action;

  memset (&action, 0, sizeof (action));
  action.sa_handler = reap_children;
  sigemptyset (&action.sa_mask);
  action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  return sigaction (SIGCHLD, &action, NULL);
}

static void
run_child (sws_socket_t listen_socket, sws_socket_t client,
           const sws_config_t *cfg)
{
  sws_platform_forget_stop_socket ();
  sws_socket_close (listen_socket);

  sws_net_prepare_client (client);
  sws_connection_serve (client, cfg);

  _exit (EXIT_SUCCESS);
}

int
sws_model_fork_run (sws_socket_t listen_socket, const sws_config_t *cfg)
{
  if (install_sigchld_handler () != 0)
    {
      sws_log_socket_error ("sigaction(SIGCHLD) failed");
      return -1;
    }

  while (!sws_platform_stop_requested ())
    {
      sws_socket_t client = accept (listen_socket, NULL, NULL);
      pid_t pid;

      if (client == SWS_INVALID_SOCKET)
        {
          if (sws_net_handle_accept_error (1))
            {
              break;
            }
          continue;
        }

      pid = fork ();
      if (pid == 0)
        {
          run_child (listen_socket, client, cfg);
        }
      if (pid < 0)
        {
          sws_log_socket_error ("fork failed");
          sws_platform_sleep_ms (SWS_FORK_BACKOFF_MS);
        }

      sws_socket_close (client);
    }
  return 0;
}

int
sws_model_fork_worker (const sws_config_t *cfg)
{
  (void)cfg;
  sws_log_error ("--worker is only used internally on Windows");
  return -1;
}

#else

#include <wchar.h>

static const WCHAR WORKER_FLAG[] = L" --worker";

typedef struct
{
  WCHAR executable[MAX_PATH];
  WCHAR *command_line;
  WCHAR *scratch;
  size_t command_length;
} spawner_t;

static int
spawner_init (spawner_t *spawner)
{
  const WCHAR *original = GetCommandLineW ();
  size_t original_length = wcslen (original);
  size_t flag_length = wcslen (WORKER_FLAG);
  DWORD length;

  length = GetModuleFileNameW (NULL, spawner->executable, MAX_PATH);
  if (length == 0 || length >= MAX_PATH)
    {
      return -1;
    }

  spawner->command_length = original_length + flag_length + 1;
  spawner->command_line = malloc (spawner->command_length * sizeof (WCHAR));
  spawner->scratch = malloc (spawner->command_length * sizeof (WCHAR));
  if (spawner->command_line == NULL || spawner->scratch == NULL)
    {
      free (spawner->command_line);
      free (spawner->scratch);
      return -1;
    }
  memcpy (spawner->command_line, original, original_length * sizeof (WCHAR));
  memcpy (spawner->command_line + original_length, WORKER_FLAG,
          (flag_length + 1) * sizeof (WCHAR));
  return 0;
}

static void
spawner_destroy (spawner_t *spawner)
{
  free (spawner->command_line);
  free (spawner->scratch);
}

static int
spawn_worker (spawner_t *spawner, sws_socket_t client)
{
  SECURITY_ATTRIBUTES attributes;
  STARTUPINFOW startup;
  PROCESS_INFORMATION process;
  WSAPROTOCOL_INFOW protocol_info;
  HANDLE pipe_read;
  HANDLE pipe_write;
  DWORD written = 0;
  BOOL created;
  int result = -1;

  memset (&attributes, 0, sizeof (attributes));
  attributes.nLength = sizeof (attributes);
  attributes.bInheritHandle = TRUE;
  if (!CreatePipe (&pipe_read, &pipe_write, &attributes, 0))
    {
      return -1;
    }
  SetHandleInformation (pipe_write, HANDLE_FLAG_INHERIT, 0);

  memset (&startup, 0, sizeof (startup));
  startup.cb = sizeof (startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = pipe_read;
  startup.hStdOutput = GetStdHandle (STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle (STD_ERROR_HANDLE);

  memcpy (spawner->scratch, spawner->command_line,
          spawner->command_length * sizeof (WCHAR));
  created = CreateProcessW (spawner->executable, spawner->scratch, NULL, NULL,
                            TRUE, 0, NULL, NULL, &startup, &process);
  CloseHandle (pipe_read);
  if (!created)
    {
      CloseHandle (pipe_write);
      return -1;
    }

  if (WSADuplicateSocketW (client, process.dwProcessId, &protocol_info) == 0
      && WriteFile (pipe_write, &protocol_info, sizeof (protocol_info),
                    &written, NULL)
      && written == sizeof (protocol_info))
    {
      result = 0;
    }
  CloseHandle (pipe_write);
  CloseHandle (process.hThread);
  CloseHandle (process.hProcess);
  return result;
}

int
sws_model_fork_run (sws_socket_t listen_socket, const sws_config_t *cfg)
{
  spawner_t spawner;

  (void)cfg;
  if (spawner_init (&spawner) != 0)
    {
      sws_log_error ("could not prepare the worker command line");
      return -1;
    }

  while (!sws_platform_stop_requested ())
    {
      sws_socket_t client = accept (listen_socket, NULL, NULL);

      if (client == SWS_INVALID_SOCKET)
        {
          if (sws_net_handle_accept_error (1))
            {
              break;
            }
          continue;
        }

      if (spawn_worker (&spawner, client) != 0)
        {
          sws_log_socket_error ("could not start a worker process");
          sws_platform_sleep_ms (SWS_FORK_BACKOFF_MS);
        }
      sws_socket_close (client);
    }

  spawner_destroy (&spawner);
  return 0;
}

int
sws_model_fork_worker (const sws_config_t *cfg)
{
  HANDLE input = GetStdHandle (STD_INPUT_HANDLE);
  WSAPROTOCOL_INFOW protocol_info;
  char *cursor = (char *)&protocol_info;
  DWORD remaining = sizeof (protocol_info);
  sws_socket_t client;

  while (remaining > 0)
    {
      DWORD bytes_read = 0;

      if (!ReadFile (input, cursor, remaining, &bytes_read, NULL)
          || bytes_read == 0)
        {
          return -1;
        }
      cursor += bytes_read;
      remaining -= bytes_read;
    }

  client = WSASocketW (FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                       FROM_PROTOCOL_INFO, &protocol_info, 0, 0);
  if (client == SWS_INVALID_SOCKET)
    {
      sws_log_socket_error ("worker could not open the inherited socket");
      return -1;
    }

  sws_net_prepare_client (client);
  sws_connection_serve (client, cfg);
  return 0;
}

#endif
