#include "platform.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#define SWS_WINSOCK_MAJOR 2
#define SWS_WINSOCK_MINOR 2
#define SWS_MILLIS_PER_SECOND 1000

static volatile LONG stop_flag = 0;
static PVOID volatile stop_socket = NULL;

static BOOL WINAPI
on_console_event (DWORD event_type)
{
  switch (event_type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
      {
        PVOID sock;

        InterlockedExchange (&stop_flag, 1);
        sock = InterlockedExchangePointer (&stop_socket, NULL);
        if (sock != NULL)
          {
            closesocket ((SOCKET)(ULONG_PTR)sock);
          }
        return TRUE;
      }
    default:
      return FALSE;
    }
}

int
sws_platform_init (void)
{
  WSADATA wsa_data;

  if (WSAStartup (MAKEWORD (SWS_WINSOCK_MAJOR, SWS_WINSOCK_MINOR), &wsa_data)
      != 0)
    {
      return -1;
    }
  return 0;
}

void
sws_platform_cleanup (void)
{
  WSACleanup ();
}

long
sws_platform_raise_fd_limit (void)
{
  return -1;
}

void
sws_platform_install_stop_handler (sws_socket_t listen_socket)
{
  InterlockedExchangePointer (&stop_socket, (PVOID)(ULONG_PTR)listen_socket);
  SetConsoleCtrlHandler (on_console_event, TRUE);
}

int
sws_platform_forget_stop_socket (void)
{
  return InterlockedExchangePointer (&stop_socket, NULL) != NULL;
}

int
sws_platform_stop_requested (void)
{
  return InterlockedCompareExchange (&stop_flag, 0, 0) != 0;
}

long
sws_platform_pid (void)
{
  return (long)GetCurrentProcessId ();
}

void
sws_platform_sleep_ms (unsigned int milliseconds)
{
  Sleep (milliseconds);
}

int
sws_platform_gmtime (time_t timestamp, struct tm *result)
{
  const struct tm *utc = gmtime (&timestamp);

  if (utc == NULL)
    {
      return -1;
    }
  *result = *utc;
  return 0;
}

void
sws_socket_close (sws_socket_t sock)
{
  closesocket (sock);
}

int
sws_socket_set_nonblocking (sws_socket_t sock)
{
  u_long enabled = 1;
  return ioctlsocket (sock, FIONBIO, &enabled) == 0 ? 0 : -1;
}

int
sws_socket_set_timeouts (sws_socket_t sock, int seconds)
{
  DWORD timeout_ms = (DWORD)seconds * SWS_MILLIS_PER_SECOND;

  if (setsockopt (sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                  sizeof (timeout_ms))
      != 0)
    {
      return -1;
    }
  return setsockopt (sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms,
                     sizeof (timeout_ms));
}

int
sws_socket_set_nodelay (sws_socket_t sock)
{
  BOOL enabled = TRUE;
  return setsockopt (sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&enabled,
                     sizeof (enabled));
}

long
sws_socket_recv (sws_socket_t sock, void *buffer, size_t length)
{
  int chunk = length > INT_MAX ? INT_MAX : (int)length;
  return recv (sock, (char *)buffer, chunk, 0);
}

long
sws_socket_send (sws_socket_t sock, const void *buffer, size_t length)
{
  int chunk = length > INT_MAX ? INT_MAX : (int)length;
  return send (sock, (const char *)buffer, chunk, 0);
}

int
sws_socket_last_error (void)
{
  return WSAGetLastError ();
}

int
sws_socket_error_is_would_block (int error)
{
  return error == WSAEWOULDBLOCK;
}

int
sws_socket_error_is_interrupted (int error)
{
  return error == WSAEINTR;
}

int
sws_socket_error_is_fd_exhaustion (int error)
{
  return error == WSAEMFILE || error == WSAENOBUFS;
}

int
sws_socket_error_is_fatal_for_listener (int error)
{
  return error == WSAENOTSOCK || error == WSAEINVAL || error == WSAEOPNOTSUPP;
}

void
sws_socket_error_string (int error, char *buffer, size_t length)
{
  DWORD written = FormatMessageA (
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
      (DWORD)error, 0, buffer, (DWORD)length, NULL);

  if (written == 0)
    {
      snprintf (buffer, length, "error %d", error);
      return;
    }
  while (written > 0
         && (buffer[written - 1] == '\n' || buffer[written - 1] == '\r'
             || buffer[written - 1] == ' ' || buffer[written - 1] == '.'))
    {
      buffer[--written] = '\0';
    }
}

#else

#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>

#define SWS_NANOS_PER_MILLI 1000000L
#define SWS_MILLIS_PER_SECOND 1000U
#define SWS_DARWIN_MAX_FDS 10240

static volatile sig_atomic_t stop_flag = 0;
static volatile sig_atomic_t stop_socket = SWS_INVALID_SOCKET;

static void
on_stop_signal (int signal_number)
{
  sws_socket_t sock = stop_socket;

  (void)signal_number;
  stop_flag = 1;
  if (sock != SWS_INVALID_SOCKET)
    {
      shutdown (sock, SHUT_RDWR);
    }
}

int
sws_platform_init (void)
{
  struct sigaction action;

  memset (&action, 0, sizeof (action));
  action.sa_handler = SIG_IGN;
  sigemptyset (&action.sa_mask);
  return sigaction (SIGPIPE, &action, NULL);
}

void
sws_platform_cleanup (void)
{
}

long
sws_platform_raise_fd_limit (void)
{
  struct rlimit limit;

  if (getrlimit (RLIMIT_NOFILE, &limit) != 0)
    {
      return -1;
    }
  if (limit.rlim_cur < limit.rlim_max)
    {
      rlim_t previous = limit.rlim_cur;

      limit.rlim_cur = limit.rlim_max;
#ifdef __APPLE__
      if (limit.rlim_cur == RLIM_INFINITY
          || limit.rlim_cur > SWS_DARWIN_MAX_FDS)
        {
          limit.rlim_cur = SWS_DARWIN_MAX_FDS;
        }
#endif
      if (setrlimit (RLIMIT_NOFILE, &limit) != 0)
        {
          limit.rlim_cur = previous;
        }
    }
  return limit.rlim_cur == RLIM_INFINITY ? LONG_MAX : (long)limit.rlim_cur;
}

void
sws_platform_install_stop_handler (sws_socket_t listen_socket)
{
  struct sigaction action;

  stop_socket = listen_socket;
  memset (&action, 0, sizeof (action));
  action.sa_handler = on_stop_signal;
  sigemptyset (&action.sa_mask);
  action.sa_flags = 0;
  sigaction (SIGINT, &action, NULL);
  sigaction (SIGTERM, &action, NULL);
}

int
sws_platform_forget_stop_socket (void)
{
  stop_socket = SWS_INVALID_SOCKET;
  return 1;
}

int
sws_platform_stop_requested (void)
{
  return stop_flag != 0;
}

long
sws_platform_pid (void)
{
  return (long)getpid ();
}

void
sws_platform_sleep_ms (unsigned int milliseconds)
{
  struct timespec duration;

  duration.tv_sec = (time_t)(milliseconds / SWS_MILLIS_PER_SECOND);
  duration.tv_nsec
      = (long)(milliseconds % SWS_MILLIS_PER_SECOND) * SWS_NANOS_PER_MILLI;
  nanosleep (&duration, NULL);
}

int
sws_platform_gmtime (time_t timestamp, struct tm *result)
{
  return gmtime_r (&timestamp, result) == NULL ? -1 : 0;
}

void
sws_socket_close (sws_socket_t sock)
{
  close (sock);
}

int
sws_socket_set_nonblocking (sws_socket_t sock)
{
  int flags = fcntl (sock, F_GETFL, 0);

  if (flags < 0)
    {
      return -1;
    }
  return fcntl (sock, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

int
sws_socket_set_timeouts (sws_socket_t sock, int seconds)
{
  struct timeval timeout;

  timeout.tv_sec = seconds;
  timeout.tv_usec = 0;
  if (setsockopt (sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof (timeout))
      != 0)
    {
      return -1;
    }
  return setsockopt (sock, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof (timeout));
}

int
sws_socket_set_nodelay (sws_socket_t sock)
{
  int enabled = 1;
  return setsockopt (sock, IPPROTO_TCP, TCP_NODELAY, &enabled,
                     sizeof (enabled));
}

long
sws_socket_recv (sws_socket_t sock, void *buffer, size_t length)
{
  return (long)recv (sock, buffer, length, 0);
}

long
sws_socket_send (sws_socket_t sock, const void *buffer, size_t length)
{
#ifdef MSG_NOSIGNAL
  return (long)send (sock, buffer, length, MSG_NOSIGNAL);
#else
  return (long)send (sock, buffer, length, 0);
#endif
}

int
sws_socket_last_error (void)
{
  return errno;
}

int
sws_socket_error_is_would_block (int error)
{
  return error == EAGAIN || error == EWOULDBLOCK;
}

int
sws_socket_error_is_interrupted (int error)
{
  return error == EINTR;
}

int
sws_socket_error_is_fd_exhaustion (int error)
{
  return error == EMFILE || error == ENFILE || error == ENOBUFS
         || error == ENOMEM;
}

int
sws_socket_error_is_fatal_for_listener (int error)
{
  return error == EBADF || error == EINVAL || error == ENOTSOCK
         || error == EOPNOTSUPP;
}

void
sws_socket_error_string (int error, char *buffer, size_t length)
{
  if (strerror_r (error, buffer, length) != 0)
    {
      snprintf (buffer, length, "error %d", error);
    }
}

#endif
