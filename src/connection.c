#include "platform.h"

#include "connection.h"
#include "http.h"
#include "net.h"

#include <string.h>

static size_t
read_request (sws_socket_t client, char *buffer, size_t capacity,
              size_t *used)
{
  size_t request_length = sws_http_find_request_end (buffer, *used);

  while (request_length == 0)
    {
      long received;

      if (*used == capacity)
        {
          sws_http_response_t response;

          if (sws_http_build_error (SWS_HTTP_STATUS_HEADER_TOO_LARGE, &response) == 0)
            {
              sws_net_send_all (client, response.data, response.length);
              sws_http_response_free (&response);
            }
          return 0;
        }

      received = sws_socket_recv (client, buffer + *used, capacity - *used);
      if (received < 0
          && sws_socket_error_is_interrupted (sws_socket_last_error ())
          && !sws_platform_stop_requested ())
        {
          continue;
        }
      if (received <= 0)
        {
          return 0;
        }
      *used += (size_t)received;
      request_length = sws_http_find_request_end (buffer, *used);
    }
  return request_length;
}

void
sws_connection_serve (sws_socket_t client, const sws_config_t *cfg)
{
  char buffer[SWS_HTTP_MAX_REQUEST_SIZE];
  size_t used = 0;

  sws_socket_set_timeouts (client, cfg->keepalive_timeout > 0
                                       ? cfg->keepalive_timeout
                                       : SWS_REQUEST_TIMEOUT);

  for (;;)
    {
      sws_http_response_t response;
      size_t request_length;
      int keep_alive;
      int sent;

      request_length = read_request (client, buffer, sizeof (buffer), &used);
      if (request_length == 0)
        {
          break;
        }

      if (sws_http_handle_request (buffer, request_length, cfg, &response)
          != 0)
        {
          break;
        }
      sent = sws_net_send_all (client, response.data, response.length);
      keep_alive = response.keep_alive;
      sws_http_response_free (&response);
      if (sent != 0 || !keep_alive)
        {
          break;
        }

      used -= request_length;
      memmove (buffer, buffer + request_length, used);
    }

  sws_socket_close (client);
}
