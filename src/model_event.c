#include "platform.h"

#include "http.h"
#include "log.h"
#include "models.h"
#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__linux__)
#define SWS_USE_EPOLL 1
#include <sys/epoll.h>
#elif !defined(_WIN32)
#include <poll.h>
#endif

#define SWS_EVENT_BATCH_SIZE 1024
#define SWS_EVENT_QUOTA 32
#define SWS_EVENT_TICK_MS 1000
#define SWS_POLL_INITIAL_CAPACITY 256

typedef struct connection
{
  sws_socket_t sock;

  char input[SWS_HTTP_MAX_REQUEST_SIZE];
  size_t input_length;

  sws_http_response_t output;
  size_t output_sent;
  int close_after_output;

  int drained;
  int peer_closed;
  int closed;
  int in_ready_list;
  time_t last_activity;

  struct connection *prev;
  struct connection *next;
  struct connection *ready_next;
  struct connection *closed_next;
#ifndef SWS_USE_EPOLL
  size_t poll_index;
#endif
} connection_t;

#ifdef _WIN32
typedef WSAPOLLFD sws_pollfd_t;
#elif !defined(SWS_USE_EPOLL)
typedef struct pollfd sws_pollfd_t;
#endif

typedef struct
{
  const sws_config_t *cfg;
  sws_socket_t listen_socket;
  int idle_timeout;
  time_t now;
  time_t last_sweep;
  time_t accept_paused_until;
  int listener_failed;

  connection_t *open_list;
  connection_t *ready_list;
  connection_t *closed_list;

#ifdef SWS_USE_EPOLL
  int epoll_fd;
  struct epoll_event *events;
#else
  sws_pollfd_t *fds;
  connection_t **owners;
  size_t count;
  size_t capacity;
#endif
} event_loop_t;

static void accept_connections (event_loop_t *loop);
static int drive_connection (event_loop_t *loop, connection_t *conn);
static void close_connection (event_loop_t *loop, connection_t *conn);

#ifdef SWS_USE_EPOLL

static int
backend_init (event_loop_t *loop)
{
  struct epoll_event event;

  loop->events = calloc (SWS_EVENT_BATCH_SIZE, sizeof (struct epoll_event));
  if (loop->events == NULL)
    {
      return -1;
    }
  loop->epoll_fd = epoll_create1 (0);
  if (loop->epoll_fd < 0)
    {
      return -1;
    }

  memset (&event, 0, sizeof (event));
  event.events = EPOLLIN;
  event.data.ptr = NULL;
  return epoll_ctl (loop->epoll_fd, EPOLL_CTL_ADD, loop->listen_socket,
                    &event);
}

static void
backend_destroy (event_loop_t *loop)
{
  if (loop->epoll_fd >= 0)
    {
      close (loop->epoll_fd);
    }
  free (loop->events);
}

static int
backend_add (event_loop_t *loop, connection_t *conn)
{
  struct epoll_event event;

  memset (&event, 0, sizeof (event));
  event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
  event.data.ptr = conn;
  return epoll_ctl (loop->epoll_fd, EPOLL_CTL_ADD, conn->sock, &event);
}

static void
backend_remove (event_loop_t *loop, connection_t *conn)
{
  (void)loop;
  (void)conn;
}

static void
backend_set_accepting (event_loop_t *loop, int enabled)
{
  struct epoll_event event;

  memset (&event, 0, sizeof (event));
  event.events = enabled ? EPOLLIN : 0;
  event.data.ptr = NULL;
  epoll_ctl (loop->epoll_fd, EPOLL_CTL_MOD, loop->listen_socket, &event);
}

static void
backend_compact (event_loop_t *loop)
{
  (void)loop;
}

static void
dispatch_epoll_event (event_loop_t *loop, const struct epoll_event *event)
{
  connection_t *conn = event->data.ptr;

  if (conn == NULL)
    {
      accept_connections (loop);
      return;
    }
  if (conn->closed)
    {
      return;
    }
  if (event->events & EPOLLERR)
    {
      close_connection (loop, conn);
      return;
    }
  if (event->events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))
    {
      conn->drained = 0;
    }
  if (!drive_connection (loop, conn))
    {
      close_connection (loop, conn);
    }
}

static int
backend_wait (event_loop_t *loop, int timeout_ms)
{
  int ready = epoll_wait (loop->epoll_fd, loop->events, SWS_EVENT_BATCH_SIZE,
                          timeout_ms);
  int index;

  loop->now = time (NULL);
  if (ready < 0)
    {
      return sws_socket_error_is_interrupted (errno) ? 0 : -1;
    }
  for (index = 0; index < ready; index++)
    {
      dispatch_epoll_event (loop, &loop->events[index]);
    }
  return 0;
}

static const char *
backend_name (void)
{
  return "epoll (edge-triggered)";
}

#else

static int
sws_poll (sws_pollfd_t *fds, size_t count, int timeout_ms)
{
#ifdef _WIN32
  return WSAPoll (fds, (ULONG)count, timeout_ms);
#else
  return poll (fds, (nfds_t)count, timeout_ms);
#endif
}

static int
backend_init (event_loop_t *loop)
{
  loop->capacity = SWS_POLL_INITIAL_CAPACITY;
  loop->fds = calloc (loop->capacity, sizeof (sws_pollfd_t));
  loop->owners = calloc (loop->capacity, sizeof (connection_t *));
  if (loop->fds == NULL || loop->owners == NULL)
    {
      return -1;
    }
  loop->fds[0].fd = loop->listen_socket;
  loop->fds[0].events = POLLIN;
  loop->owners[0] = NULL;
  loop->count = 1;
  return 0;
}

static void
backend_destroy (event_loop_t *loop)
{
  free (loop->fds);
  free (loop->owners);
}

static int
backend_add (event_loop_t *loop, connection_t *conn)
{
  if (loop->count == loop->capacity)
    {
      size_t capacity = loop->capacity * 2;
      sws_pollfd_t *fds = realloc (loop->fds, capacity * sizeof (*fds));
      connection_t **owners;

      if (fds == NULL)
        {
          return -1;
        }
      loop->fds = fds;
      owners = realloc (loop->owners, capacity * sizeof (*owners));
      if (owners == NULL)
        {
          return -1;
        }
      loop->owners = owners;
      loop->capacity = capacity;
    }
  loop->fds[loop->count].fd = conn->sock;
  loop->fds[loop->count].events = POLLIN;
  loop->fds[loop->count].revents = 0;
  loop->owners[loop->count] = conn;
  conn->poll_index = loop->count;
  loop->count++;
  return 0;
}

static void
backend_remove (event_loop_t *loop, connection_t *conn)
{
  loop->owners[conn->poll_index] = NULL;
}

static void
backend_set_accepting (event_loop_t *loop, int enabled)
{
  loop->fds[0].events = enabled ? POLLIN : 0;
}

static void
backend_compact (event_loop_t *loop)
{
  size_t from;
  size_t to = 1;

  for (from = 1; from < loop->count; from++)
    {
      if (loop->owners[from] == NULL)
        {
          continue;
        }
      loop->fds[to] = loop->fds[from];
      loop->owners[to] = loop->owners[from];
      loop->owners[to]->poll_index = to;
      to++;
    }
  loop->count = to;
}

static int
backend_wait (event_loop_t *loop, int timeout_ms)
{
  size_t index;
  size_t current;
  int ready;

  for (index = 1; index < loop->count; index++)
    {
      const connection_t *conn = loop->owners[index];
      int writing = conn != NULL && conn->output.data != NULL;

      loop->fds[index].revents = 0;
      loop->fds[index].events = (short)(writing ? (POLLIN | POLLOUT) : POLLIN);
    }
  loop->fds[0].revents = 0;

  ready = sws_poll (loop->fds, loop->count, timeout_ms);
  loop->now = time (NULL);
  if (ready < 0)
    {
      return sws_socket_error_is_interrupted (sws_socket_last_error ()) ? 0
                                                                        : -1;
    }

  current = loop->count;
  if (loop->fds[0].revents != 0)
    {
      accept_connections (loop);
    }
  for (index = 1; index < current; index++)
    {
      connection_t *conn = loop->owners[index];
      short revents = loop->fds[index].revents;

      if (conn == NULL || revents == 0)
        {
          continue;
        }
      if (revents & (POLLERR | POLLNVAL))
        {
          close_connection (loop, conn);
          continue;
        }
      if (revents & (POLLIN | POLLHUP))
        {
          conn->drained = 0;
        }
      if (!drive_connection (loop, conn))
        {
          close_connection (loop, conn);
        }
    }
  return 0;
}

static const char *
backend_name (void)
{
#ifdef _WIN32
  return "WSAPoll (level-triggered)";
#else
  return "poll (level-triggered)";
#endif
}

#endif

static void
schedule_connection (event_loop_t *loop, connection_t *conn)
{
  if (!conn->in_ready_list)
    {
      conn->in_ready_list = 1;
      conn->ready_next = loop->ready_list;
      loop->ready_list = conn;
    }
}

static void
close_connection (event_loop_t *loop, connection_t *conn)
{
  if (conn->closed)
    {
      return;
    }
  conn->closed = 1;
  backend_remove (loop, conn);
  sws_socket_close (conn->sock);

  if (conn->prev != NULL)
    {
      conn->prev->next = conn->next;
    }
  else
    {
      loop->open_list = conn->next;
    }
  if (conn->next != NULL)
    {
      conn->next->prev = conn->prev;
    }

  conn->closed_next = loop->closed_list;
  loop->closed_list = conn;
}

static void
release_closed_connections (event_loop_t *loop)
{
  while (loop->closed_list != NULL)
    {
      connection_t *conn = loop->closed_list;

      loop->closed_list = conn->closed_next;
      sws_http_response_free (&conn->output);
      free (conn);
    }
}

static sws_io_status_t
flush_output (event_loop_t *loop, connection_t *conn)
{
  while (conn->output_sent < conn->output.length)
    {
      long written
          = sws_socket_send (conn->sock, conn->output.data + conn->output_sent,
                             conn->output.length - conn->output_sent);

      if (written < 0)
        {
          int error = sws_socket_last_error ();

          if (sws_socket_error_is_would_block (error))
            {
              return SWS_IO_WOULD_BLOCK;
            }
          if (sws_socket_error_is_interrupted (error))
            {
              continue;
            }
          return SWS_IO_ERROR;
        }
      conn->output_sent += (size_t)written;
      conn->last_activity = loop->now;
    }
  sws_http_response_free (&conn->output);
  conn->output_sent = 0;
  return SWS_IO_DONE;
}

static sws_io_status_t
fill_input (event_loop_t *loop, connection_t *conn)
{
  while (conn->input_length < sizeof (conn->input))
    {
      long received = sws_socket_recv (conn->sock,
                                       conn->input + conn->input_length,
                                       sizeof (conn->input)
                                           - conn->input_length);

      if (received > 0)
        {
          conn->input_length += (size_t)received;
          conn->last_activity = loop->now;
          continue;
        }
      if (received == 0)
        {
          conn->peer_closed = 1;
          return SWS_IO_DONE;
        }
      {
        int error = sws_socket_last_error ();

        if (sws_socket_error_is_would_block (error))
          {
            conn->drained = 1;
            return SWS_IO_DONE;
          }
        if (!sws_socket_error_is_interrupted (error))
          {
            return SWS_IO_ERROR;
          }
      }
    }
  return SWS_IO_DONE;
}

static int
process_next_request (event_loop_t *loop, connection_t *conn)
{
  size_t request_length
      = sws_http_find_request_end (conn->input, conn->input_length);

  if (request_length == 0)
    {
      if (conn->input_length < sizeof (conn->input))
        {
          return 0;
        }
      if (sws_http_build_error (SWS_HTTP_STATUS_HEADER_TOO_LARGE,
                                &conn->output)
          != 0)
        {
          return -1;
        }
      conn->close_after_output = 1;
      conn->input_length = 0;
      return 1;
    }

  if (sws_http_handle_request (conn->input, request_length, loop->cfg,
                               &conn->output)
      != 0)
    {
      return -1;
    }
  conn->close_after_output = !conn->output.keep_alive;
  conn->output_sent = 0;

  conn->input_length -= request_length;
  memmove (conn->input, conn->input + request_length, conn->input_length);
  return 1;
}

static int
drive_connection (event_loop_t *loop, connection_t *conn)
{
  int responses = 0;

  for (;;)
    {
      int processed;

      if (conn->output.data != NULL)
        {
          sws_io_status_t status = flush_output (loop, conn);

          if (status == SWS_IO_ERROR)
            {
              return 0;
            }
          if (status == SWS_IO_WOULD_BLOCK)
            {
              return 1;
            }
          if (conn->close_after_output)
            {
              return 0;
            }
        }

      if (responses == SWS_EVENT_QUOTA
          && sws_http_find_request_end (conn->input, conn->input_length) > 0)
        {
          schedule_connection (loop, conn);
          return 1;
        }

      processed = process_next_request (loop, conn);
      if (processed < 0)
        {
          return 0;
        }
      if (processed > 0)
        {
          responses++;
          continue;
        }

      if (conn->peer_closed)
        {
          return 0;
        }
      if (conn->drained)
        {
          return 1;
        }
      if (fill_input (loop, conn) == SWS_IO_ERROR)
        {
          return 0;
        }
    }
}

static void
accept_connections (event_loop_t *loop)
{
  for (;;)
    {
      sws_socket_t client = accept (loop->listen_socket, NULL, NULL);
      connection_t *conn;

      if (client == SWS_INVALID_SOCKET)
        {
          int error = sws_socket_last_error ();

          if (sws_socket_error_is_would_block (error))
            {
              return;
            }
          if (sws_socket_error_is_fd_exhaustion (error))
            {
              loop->accept_paused_until = loop->now + 1;
              backend_set_accepting (loop, 0);
            }
          if (sws_net_handle_accept_error (0)
              && !sws_platform_stop_requested ())
            {
              loop->listener_failed = 1;
            }
          return;
        }

      if (sws_socket_set_nonblocking (client) != 0)
        {
          sws_log_socket_error ("could not make the socket non-blocking");
          sws_socket_close (client);
          continue;
        }
      sws_net_prepare_client (client);

      conn = calloc (1, sizeof (*conn));
      if (conn == NULL)
        {
          sws_log_error ("out of memory for a new connection");
          sws_socket_close (client);
          continue;
        }
      conn->sock = client;
      conn->last_activity = loop->now;

      if (backend_add (loop, conn) != 0)
        {
          sws_log_socket_error ("could not register the connection");
          sws_socket_close (client);
          free (conn);
          continue;
        }

      conn->next = loop->open_list;
      if (loop->open_list != NULL)
        {
          loop->open_list->prev = conn;
        }
      loop->open_list = conn;
    }
}

static void
process_ready_list (event_loop_t *loop)
{
  connection_t *conn = loop->ready_list;

  loop->ready_list = NULL;
  while (conn != NULL)
    {
      connection_t *next = conn->ready_next;

      conn->in_ready_list = 0;
      conn->ready_next = NULL;
      if (!conn->closed && !drive_connection (loop, conn))
        {
          close_connection (loop, conn);
        }
      conn = next;
    }
}

static void
sweep_idle_connections (event_loop_t *loop)
{
  connection_t *conn;

  if (loop->now == loop->last_sweep)
    {
      return;
    }
  loop->last_sweep = loop->now;

  if (loop->accept_paused_until != 0 && loop->now >= loop->accept_paused_until)
    {
      loop->accept_paused_until = 0;
      backend_set_accepting (loop, 1);
    }

  conn = loop->open_list;
  while (conn != NULL)
    {
      connection_t *next = conn->next;

      if (!conn->in_ready_list
          && loop->now - conn->last_activity >= loop->idle_timeout)
        {
          close_connection (loop, conn);
        }
      conn = next;
    }
}

int
sws_model_event_run (sws_socket_t listen_socket, const sws_config_t *cfg)
{
  event_loop_t loop;
  int result = 0;

  memset (&loop, 0, sizeof (loop));
  loop.cfg = cfg;
  loop.listen_socket = listen_socket;
  loop.idle_timeout = cfg->keepalive_timeout > 0 ? cfg->keepalive_timeout
                                                 : SWS_REQUEST_TIMEOUT;
  loop.now = time (NULL);
  loop.last_sweep = loop.now;
#ifdef SWS_USE_EPOLL
  loop.epoll_fd = -1;
#endif

  if (sws_socket_set_nonblocking (listen_socket) != 0
      || backend_init (&loop) != 0)
    {
      sws_log_socket_error ("could not initialize the event loop");
      backend_destroy (&loop);
      return -1;
    }
  sws_log_info ("event loop using %s", backend_name ());

  while (!sws_platform_stop_requested () && !loop.listener_failed)
    {
      int timeout = loop.ready_list != NULL ? 0 : SWS_EVENT_TICK_MS;

      if (backend_wait (&loop, timeout) != 0)
        {
          if (!sws_platform_stop_requested ())
            {
              sws_log_socket_error ("event wait failed");
              result = -1;
            }
          break;
        }
      process_ready_list (&loop);
      sweep_idle_connections (&loop);
      backend_compact (&loop);
      release_closed_connections (&loop);
    }
  if (loop.listener_failed)
    {
      result = -1;
    }

  while (loop.open_list != NULL)
    {
      close_connection (&loop, loop.open_list);
    }
  release_closed_connections (&loop);
  backend_destroy (&loop);
  return result;
}
