#ifndef SWS_HTTP_H
#define SWS_HTTP_H

#include <stddef.h>

#include "config.h"

#define SWS_HTTP_MAX_REQUEST_SIZE 8192

#define SWS_HTTP_STATUS_HEADER_TOO_LARGE 431

#define SWS_HTTP_MAX_FILE_SIZE (64L * 1024L * 1024L)

typedef struct
{
  char *data;
  size_t length;
  int status;
  int keep_alive;
} sws_http_response_t;

size_t sws_http_find_request_end (const char *buffer, size_t length);

int sws_http_handle_request (const char *request, size_t length,
                             const sws_config_t *cfg,
                             sws_http_response_t *response);

int sws_http_build_error (int status, sws_http_response_t *response);

void sws_http_response_free (sws_http_response_t *response);

#endif
