#ifndef SWS_PLATFORM_H
#define SWS_PLATFORM_H

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif

#include <stddef.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef SOCKET sws_socket_t;
#define SWS_INVALID_SOCKET INVALID_SOCKET
#define SWS_PLATFORM_NAME "Windows"

#else

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int sws_socket_t;
#define SWS_INVALID_SOCKET (-1)
#if defined(__linux__)
#define SWS_PLATFORM_NAME "Linux"
#elif defined(__APPLE__)
#define SWS_PLATFORM_NAME "macOS"
#else
#define SWS_PLATFORM_NAME "POSIX"
#endif

#endif

#ifdef _WIN32
typedef struct _stat64 sws_stat_t;
#define sws_stat(path, info) _stat64 ((path), (info))
#define SWS_IS_DIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#define SWS_IS_REG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#else
typedef struct stat sws_stat_t;
#define sws_stat(path, info) stat ((path), (info))
#define SWS_IS_DIR(mode) S_ISDIR (mode)
#define SWS_IS_REG(mode) S_ISREG (mode)
#endif

typedef enum
{
  SWS_IO_ERROR = -1,
  SWS_IO_WOULD_BLOCK = 0,
  SWS_IO_DONE = 1
} sws_io_status_t;

int sws_platform_init (void);
void sws_platform_cleanup (void);

long sws_platform_raise_fd_limit (void);

void sws_platform_install_stop_handler (sws_socket_t listen_socket);
int sws_platform_stop_requested (void);

int sws_platform_forget_stop_socket (void);

long sws_platform_pid (void);

void sws_platform_sleep_ms (unsigned int milliseconds);

int sws_platform_gmtime (time_t timestamp, struct tm *result);

void sws_socket_close (sws_socket_t sock);
int sws_socket_set_nonblocking (sws_socket_t sock);
int sws_socket_set_timeouts (sws_socket_t sock, int seconds);
int sws_socket_set_nodelay (sws_socket_t sock);
long sws_socket_recv (sws_socket_t sock, void *buffer, size_t length);
long sws_socket_send (sws_socket_t sock, const void *buffer, size_t length);

int sws_socket_last_error (void);
int sws_socket_error_is_would_block (int error);
int sws_socket_error_is_interrupted (int error);
int sws_socket_error_is_fd_exhaustion (int error);
int sws_socket_error_is_fatal_for_listener (int error);
void sws_socket_error_string (int error, char *buffer, size_t length);

#endif
