#include "platform.h"

#include "log.h"
#include "net.h"

#include <string.h>
#include <time.h>

#define SWS_ACCEPT_BACKOFF_MS 10

sws_socket_t
sws_net_listen (unsigned short port, int backlog)
{
  sws_socket_t server_socket;
  struct sockaddr_in server_addr;
  int enabled = 1;

  server_socket = socket (AF_INET, SOCK_STREAM, 0);
  if (server_socket == SWS_INVALID_SOCKET)
    {
      sws_log_socket_error ("socket initialization failed");
      return SWS_INVALID_SOCKET;
    }

#ifdef _WIN32
  if (setsockopt (server_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                  (const char *)&enabled, sizeof (enabled))
      != 0)
    {
      sws_log_socket_error ("setsockopt(SO_EXCLUSIVEADDRUSE) failed");
    }
  SetHandleInformation ((HANDLE)server_socket, HANDLE_FLAG_INHERIT, 0);
#else
  if (setsockopt (server_socket, SOL_SOCKET, SO_REUSEADDR, &enabled,
                  sizeof (enabled))
      != 0)
    {
      sws_log_socket_error ("setsockopt(SO_REUSEADDR) failed");
    }
#endif

  memset (&server_addr, 0, sizeof (server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl (INADDR_ANY);
  server_addr.sin_port = htons (port);

  if (bind (server_socket, (struct sockaddr *)&server_addr,
            sizeof (server_addr))
      != 0)
    {
      sws_log_socket_error ("bind failed");
      sws_socket_close (server_socket);
      return SWS_INVALID_SOCKET;
    }

  if (listen (server_socket, backlog) != 0)
    {
      sws_log_socket_error ("listen failed");
      sws_socket_close (server_socket);
      return SWS_INVALID_SOCKET;
    }

  return server_socket;
}

void
sws_net_prepare_client (sws_socket_t client)
{
  sws_socket_set_nodelay (client);
}

int
sws_net_send_all (sws_socket_t sock, const char *data, size_t length)
{
  size_t sent = 0;

  while (sent < length)
    {
      long written = sws_socket_send (sock, data + sent, length - sent);

      if (written < 0)
        {
          if (sws_socket_error_is_interrupted (sws_socket_last_error ())
              && !sws_platform_stop_requested ())
            {
              continue;
            }
          return -1;
        }
      sent += (size_t)written;
    }
  return 0;
}

int
sws_net_handle_accept_error (int may_sleep)
{
  static time_t last_exhaustion_log = 0;
  int error = sws_socket_last_error ();

  if (sws_platform_stop_requested ())
    {
      return 1;
    }
  if (sws_socket_error_is_interrupted (error))
    {
      return 0;
    }
  if (sws_socket_error_is_fatal_for_listener (error))
    {
      sws_log_socket_error ("accept failed on the listening socket");
      return 1;
    }
  if (sws_socket_error_is_fd_exhaustion (error))
    {
      time_t now = time (NULL);

      if (now != last_exhaustion_log)
        {
          last_exhaustion_log = now;
          sws_log_socket_error ("accept failed (resources exhausted)");
        }
      if (may_sleep)
        {
          sws_platform_sleep_ms (SWS_ACCEPT_BACKOFF_MS);
        }
      return 0;
    }
  if (!sws_socket_error_is_would_block (error))
    {
      sws_log_socket_error ("fail to accept connection");
    }
  return 0;
}
