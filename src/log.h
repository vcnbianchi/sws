#ifndef SWS_LOG_H
#define SWS_LOG_H

#if defined(__GNUC__) || defined(__clang__)
#define SWS_PRINTF_FORMAT(fmt_index, args_index)                              \
  __attribute__ ((format (printf, fmt_index, args_index)))
#else
#define SWS_PRINTF_FORMAT(fmt_index, args_index)
#endif

void sws_log_init (void);

void sws_log_info (const char *format, ...) SWS_PRINTF_FORMAT (1, 2);
void sws_log_error (const char *format, ...) SWS_PRINTF_FORMAT (1, 2);

void sws_log_socket_error (const char *what);

#endif
