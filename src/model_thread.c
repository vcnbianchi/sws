#include "platform.h"

#include "connection.h"
#include "log.h"
#include "models.h"
#include "net.h"

#include <stdint.h>

#define SWS_THREAD_BACKOFF_MS 10

static const sws_config_t *shared_config = NULL;

#ifndef _WIN32

#include <pthread.h>

static void *
connection_thread (void *argument)
{
  sws_socket_t client = (sws_socket_t)(intptr_t)argument;

  sws_net_prepare_client (client);
  sws_connection_serve (client, shared_config);
  return NULL;
}

static int
start_connection_thread (const pthread_attr_t *attributes,
                         sws_socket_t client)
{
  pthread_t thread;
  int error = pthread_create (&thread, attributes, connection_thread,
                              (void *)(intptr_t)client);

  if (error != 0)
    {
      errno = error;
      return -1;
    }
  return 0;
}

int
sws_model_thread_run (sws_socket_t listen_socket, const sws_config_t *cfg)
{
  pthread_attr_t attributes;

  shared_config = cfg;

  if (pthread_attr_init (&attributes) != 0
      || pthread_attr_setdetachstate (&attributes, PTHREAD_CREATE_DETACHED)
             != 0)
    {
      sws_log_error ("could not initialize thread attributes");
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

      if (start_connection_thread (&attributes, client) != 0)
        {
          sws_log_socket_error ("pthread_create failed");
          sws_socket_close (client);
          sws_platform_sleep_ms (SWS_THREAD_BACKOFF_MS);
        }
    }

  pthread_attr_destroy (&attributes);
  return 0;
}

#else

#include <process.h>

static unsigned __stdcall
connection_thread (void *argument)
{
  sws_socket_t client = (sws_socket_t)(uintptr_t)argument;

  sws_net_prepare_client (client);
  sws_connection_serve (client, shared_config);
  return 0;
}

int
sws_model_thread_run (sws_socket_t listen_socket, const sws_config_t *cfg)
{
  shared_config = cfg;

  while (!sws_platform_stop_requested ())
    {
      sws_socket_t client = accept (listen_socket, NULL, NULL);
      uintptr_t thread;

      if (client == SWS_INVALID_SOCKET)
        {
          if (sws_net_handle_accept_error (1))
            {
              break;
            }
          continue;
        }

      thread = _beginthreadex (NULL, 0, connection_thread,
                               (void *)(uintptr_t)client, 0, NULL);
      if (thread == 0)
        {
          sws_log_error ("_beginthreadex failed");
          sws_socket_close (client);
          sws_platform_sleep_ms (SWS_THREAD_BACKOFF_MS);
          continue;
        }
      CloseHandle ((HANDLE)thread);
    }
  return 0;
}

#endif
