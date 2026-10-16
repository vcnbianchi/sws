#include "platform.h"

#include "http.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SWS_HTTP_MAX_PATH 1024
#define SWS_HTTP_HEADER_SIZE (2 * SWS_HTTP_MAX_PATH)
#define SWS_HTTP_DATE_SIZE 64
#define SWS_HTTP_ERROR_BODY_SIZE 256
#define SWS_HTTP_VERSION_LENGTH 8
#define SWS_HEX_LETTER_OFFSET 10
#define SWS_HEX_SHIFT 4

enum
{
  HTTP_OK = 200,
  HTTP_MOVED_PERMANENTLY = 301,
  HTTP_BAD_REQUEST = 400,
  HTTP_FORBIDDEN = 403,
  HTTP_NOT_FOUND = 404,
  HTTP_METHOD_NOT_ALLOWED = 405,
  HTTP_URI_TOO_LONG = 414,
  HTTP_HEADER_FIELDS_TOO_LARGE = 431,
  HTTP_INTERNAL_SERVER_ERROR = 500,
  HTTP_NOT_IMPLEMENTED = 501,
  HTTP_VERSION_NOT_SUPPORTED = 505
};

typedef struct
{
  const char *method;
  size_t method_length;
  const char *target;
  size_t target_length;
  int minor_version;
  int host_count;
  int wants_close;
  int wants_keepalive;
  int has_body;
} request_t;

typedef struct
{
  const char *memory;
  FILE *file;
  size_t length;
  int omit;
} body_t;

typedef struct
{
  const char *extension;
  const char *type;
} mime_type_t;

static const mime_type_t MIME_TYPES[] = {
  { "html", "text/html; charset=utf-8" },
  { "htm", "text/html; charset=utf-8" },
  { "css", "text/css; charset=utf-8" },
  { "js", "text/javascript; charset=utf-8" },
  { "mjs", "text/javascript; charset=utf-8" },
  { "json", "application/json" },
  { "txt", "text/plain; charset=utf-8" },
  { "csv", "text/csv; charset=utf-8" },
  { "xml", "application/xml" },
  { "svg", "image/svg+xml" },
  { "png", "image/png" },
  { "jpg", "image/jpeg" },
  { "jpeg", "image/jpeg" },
  { "gif", "image/gif" },
  { "webp", "image/webp" },
  { "ico", "image/x-icon" },
  { "pdf", "application/pdf" },
  { "wasm", "application/wasm" },
  { "woff", "font/woff" },
  { "woff2", "font/woff2" },
  { "mp4", "video/mp4" },
};

#define SWS_DEFAULT_MIME_TYPE "application/octet-stream"

static int
to_lower (int character)
{
  if (character >= 'A' && character <= 'Z')
    {
      return character - 'A' + 'a';
    }
  return character;
}

static int
token_equals (const char *token, size_t length, const char *literal)
{
  size_t index;

  for (index = 0; index < length; index++)
    {
      if (literal[index] == '\0'
          || to_lower ((unsigned char)token[index]) != literal[index])
        {
          return 0;
        }
    }
  return literal[length] == '\0';
}

static int
is_space (char character)
{
  return character == ' ' || character == '\t';
}

static int
hex_value (char character)
{
  if (character >= '0' && character <= '9')
    {
      return character - '0';
    }
  character = (char)to_lower ((unsigned char)character);
  if (character >= 'a' && character <= 'f')
    {
      return character - 'a' + SWS_HEX_LETTER_OFFSET;
    }
  return -1;
}

static const char *
reason_phrase (int status)
{
  switch (status)
    {
    case HTTP_OK:
      return "OK";
    case HTTP_MOVED_PERMANENTLY:
      return "Moved Permanently";
    case HTTP_BAD_REQUEST:
      return "Bad Request";
    case HTTP_FORBIDDEN:
      return "Forbidden";
    case HTTP_NOT_FOUND:
      return "Not Found";
    case HTTP_METHOD_NOT_ALLOWED:
      return "Method Not Allowed";
    case HTTP_URI_TOO_LONG:
      return "URI Too Long";
    case HTTP_HEADER_FIELDS_TOO_LARGE:
      return "Request Header Fields Too Large";
    case HTTP_NOT_IMPLEMENTED:
      return "Not Implemented";
    case HTTP_VERSION_NOT_SUPPORTED:
      return "HTTP Version Not Supported";
    case HTTP_INTERNAL_SERVER_ERROR:
    default:
      return "Internal Server Error";
    }
}

static const char *
mime_type_for (const char *path)
{
  const char *dot = strrchr (path, '.');
  const char *slash = strrchr (path, '/');
  size_t index;

  if (dot == NULL || (slash != NULL && dot < slash))
    {
      return SWS_DEFAULT_MIME_TYPE;
    }
  dot++;
  for (index = 0; index < sizeof (MIME_TYPES) / sizeof (MIME_TYPES[0]);
       index++)
    {
      if (token_equals (dot, strlen (dot), MIME_TYPES[index].extension))
        {
          return MIME_TYPES[index].type;
        }
    }
  return SWS_DEFAULT_MIME_TYPE;
}

static void
format_date (char *buffer, size_t size)
{
  struct tm utc;

  buffer[0] = '\0';
  if (sws_platform_gmtime (time (NULL), &utc) == 0)
    {
      strftime (buffer, size, "%a, %d %b %Y %H:%M:%S GMT", &utc);
    }
}

size_t
sws_http_find_request_end (const char *buffer, size_t length)
{
  size_t start = 0;
  size_t index;

  while (start < length && (buffer[start] == '\r' || buffer[start] == '\n'))
    {
      start++;
    }
  for (index = start; index < length; index++)
    {
      if (buffer[index] != '\n')
        {
          continue;
        }
      if (index + 1 < length && buffer[index + 1] == '\n')
        {
          return index + 2;
        }
      if (index + 2 < length && buffer[index + 1] == '\r'
          && buffer[index + 2] == '\n')
        {
          return index + 3;
        }
    }
  return 0;
}

static size_t
next_line (const char *cursor, const char *end, const char **next)
{
  const char *newline = memchr (cursor, '\n', (size_t)(end - cursor));
  size_t length;

  if (newline == NULL)
    {
      *next = end;
      return (size_t)(end - cursor);
    }
  *next = newline + 1;
  length = (size_t)(newline - cursor);
  if (length > 0 && cursor[length - 1] == '\r')
    {
      length--;
    }
  return length;
}

static int
parse_request_line (const char *line, size_t length, request_t *request)
{
  const char *end = line + length;
  const char *first_space = memchr (line, ' ', length);
  const char *second_space;
  const char *version;

  if (first_space == NULL || first_space == line)
    {
      return HTTP_BAD_REQUEST;
    }
  second_space
      = memchr (first_space + 1, ' ', (size_t)(end - first_space - 1));
  if (second_space == NULL || second_space == first_space + 1)
    {
      return HTTP_BAD_REQUEST;
    }

  request->method = line;
  request->method_length = (size_t)(first_space - line);
  request->target = first_space + 1;
  request->target_length = (size_t)(second_space - first_space - 1);

  version = second_space + 1;
  if ((size_t)(end - version) != SWS_HTTP_VERSION_LENGTH
      || strncmp (version, "HTTP/", strlen ("HTTP/")) != 0
      || version[5] < '0' || version[5] > '9' || version[6] != '.'
      || version[7] < '0' || version[7] > '9')
    {
      return HTTP_BAD_REQUEST;
    }
  if (version[5] != '1' || (version[7] != '0' && version[7] != '1'))
    {
      return HTTP_VERSION_NOT_SUPPORTED;
    }
  request->minor_version = version[7] - '0';
  return 0;
}

static void
parse_connection_header (const char *value, size_t length,
                         request_t *request)
{
  const char *cursor = value;
  const char *end = value + length;

  while (cursor < end)
    {
      const char *comma = memchr (cursor, ',', (size_t)(end - cursor));
      const char *token_end = comma != NULL ? comma : end;
      const char *token_start = cursor;

      while (token_start < token_end && is_space (*token_start))
        {
          token_start++;
        }
      while (token_end > token_start && is_space (token_end[-1]))
        {
          token_end--;
        }
      if (token_equals (token_start, (size_t)(token_end - token_start),
                        "close"))
        {
          request->wants_close = 1;
        }
      else if (token_equals (token_start, (size_t)(token_end - token_start),
                             "keep-alive"))
        {
          request->wants_keepalive = 1;
        }
      cursor = comma != NULL ? comma + 1 : end;
    }
}

static int
parse_header_field (const char *line, size_t length, request_t *request)
{
  const char *colon = memchr (line, ':', length);
  const char *value;
  const char *value_end = line + length;
  size_t name_length;

  if (colon == NULL || colon == line || is_space (colon[-1]))
    {
      return HTTP_BAD_REQUEST;
    }
  name_length = (size_t)(colon - line);
  value = colon + 1;
  while (value < value_end && is_space (*value))
    {
      value++;
    }
  while (value_end > value && is_space (value_end[-1]))
    {
      value_end--;
    }

  if (token_equals (line, name_length, "host"))
    {
      request->host_count++;
    }
  else if (token_equals (line, name_length, "connection"))
    {
      parse_connection_header (value, (size_t)(value_end - value), request);
    }
  else if (token_equals (line, name_length, "content-length"))
    {
      const char *digit;

      if (value == value_end)
        {
          return HTTP_BAD_REQUEST;
        }
      for (digit = value; digit < value_end; digit++)
        {
          if (*digit < '0' || *digit > '9')
            {
              return HTTP_BAD_REQUEST;
            }
          if (*digit != '0')
            {
              request->has_body = 1;
            }
        }
    }
  else if (token_equals (line, name_length, "transfer-encoding"))
    {
      request->has_body = 1;
    }
  return 0;
}

static int
parse_request (const char *data, size_t length, request_t *request)
{
  const char *cursor = data;
  const char *end = data + length;
  const char *next = NULL;
  size_t line_length;
  int status;

  memset (request, 0, sizeof (*request));
  while (cursor < end && (*cursor == '\r' || *cursor == '\n'))
    {
      cursor++;
    }

  line_length = next_line (cursor, end, &next);
  status = parse_request_line (cursor, line_length, request);
  if (status != 0)
    {
      return status;
    }

  for (cursor = next; cursor < end; cursor = next)
    {
      line_length = next_line (cursor, end, &next);
      if (line_length == 0)
        {
          break;
        }
      if (is_space (*cursor))
        {
          return HTTP_BAD_REQUEST;
        }
      status = parse_header_field (cursor, line_length, request);
      if (status != 0)
        {
          return status;
        }
    }

  if ((request->minor_version == 1 && request->host_count == 0)
      || request->host_count > 1)
    {
      return HTTP_BAD_REQUEST;
    }
  return 0;
}

static int
assemble_response (sws_http_response_t *response, int status,
                   const char *content_type, const char *extra_headers,
                   const body_t *body, int keep_alive)
{
  char header[SWS_HTTP_HEADER_SIZE];
  char date[SWS_HTTP_DATE_SIZE];
  size_t body_bytes = body->omit ? 0 : body->length;
  int header_length;

  format_date (date, sizeof (date));
  header_length = snprintf (
      header, sizeof (header),
      "HTTP/1.1 %d %s\r\n"
      "Server: " SWS_SERVER_NAME "\r\n"
      "%s%s%s"
      "Content-Type: %s\r\n"
      "Content-Length: %lu\r\n"
      "Connection: %s\r\n"
      "%s"
      "\r\n",
      status, reason_phrase (status), date[0] != '\0' ? "Date: " : "", date,
      date[0] != '\0' ? "\r\n" : "", content_type,
      (unsigned long)body->length, keep_alive ? "keep-alive" : "close",
      extra_headers != NULL ? extra_headers : "");
  if (header_length < 0 || (size_t)header_length >= sizeof (header))
    {
      return -1;
    }

  response->data = malloc ((size_t)header_length + body_bytes);
  if (response->data == NULL)
    {
      return -1;
    }
  memcpy (response->data, header, (size_t)header_length);

  if (body_bytes > 0 && body->memory != NULL)
    {
      memcpy (response->data + header_length, body->memory, body_bytes);
    }
  else if (body_bytes > 0 && body->file != NULL)
    {
      if (fread (response->data + header_length, 1, body_bytes, body->file)
          != body_bytes)
        {
          free (response->data);
          response->data = NULL;
          return -1;
        }
    }

  response->length = (size_t)header_length + body_bytes;
  response->status = status;
  response->keep_alive = keep_alive;
  return 0;
}

static int
error_response (sws_http_response_t *response, int status,
                const char *extra_headers, int omit_body, int keep_alive)
{
  char page[SWS_HTTP_ERROR_BODY_SIZE];
  int page_length;
  body_t body;

  page_length = snprintf (page, sizeof (page),
                          "<!DOCTYPE html>\n<html><head><title>%d %s</title>"
                          "</head><body><h1>%d %s</h1><hr>"
                          "<p>" SWS_SERVER_NAME "</p></body></html>\n",
                          status, reason_phrase (status), status,
                          reason_phrase (status));
  if (page_length < 0 || (size_t)page_length >= sizeof (page))
    {
      return -1;
    }

  body.memory = page;
  body.file = NULL;
  body.length = (size_t)page_length;
  body.omit = omit_body;
  return assemble_response (response, status, "text/html; charset=utf-8",
                            extra_headers, &body, keep_alive);
}

int
sws_http_build_error (int status, sws_http_response_t *response)
{
  return error_response (response, status, NULL, 0, 0);
}

void
sws_http_response_free (sws_http_response_t *response)
{
  free (response->data);
  response->data = NULL;
  response->length = 0;
}

static int
decode_target (const char *target, size_t length, char *decoded,
               size_t size)
{
  size_t in = 0;
  size_t out = 0;

  if (length == 0 || target[0] != '/')
    {
      return HTTP_BAD_REQUEST;
    }
  for (in = 0; in < length && target[in] != '?' && target[in] != '#'; in++)
    {
      char character = target[in];

      if (character == '%')
        {
          int high;
          int low;

          if (in + 2 >= length)
            {
              return HTTP_BAD_REQUEST;
            }
          high = hex_value (target[in + 1]);
          low = hex_value (target[in + 2]);
          if (high < 0 || low < 0)
            {
              return HTTP_BAD_REQUEST;
            }
          character = (char)((high << SWS_HEX_SHIFT) | low);
          in += 2;
        }
      if (character == '\0' || character == '\\')
        {
          return HTTP_BAD_REQUEST;
        }
      if (out + 1 >= size)
        {
          return HTTP_URI_TOO_LONG;
        }
      decoded[out++] = character;
    }
  decoded[out] = '\0';
  return 0;
}

static int
is_safe_path (const char *path)
{
  const char *segment = path;

  while (*segment != '\0')
    {
      const char *slash;
      size_t length;

      while (*segment == '/')
        {
          segment++;
        }
      slash = strchr (segment, '/');
      length = slash != NULL ? (size_t)(slash - segment) : strlen (segment);
      if (length == 2 && segment[0] == '.' && segment[1] == '.')
        {
          return 0;
        }
#ifdef _WIN32
      if (length > 0
          && (segment[length - 1] == '.' || segment[length - 1] == ' '))
        {
          return 0;
        }
#endif
      segment += length;
    }
#ifdef _WIN32
  if (strchr (path, ':') != NULL)
    {
      return 0;
    }
#endif
  return 1;
}

static size_t
target_path_length (const request_t *request)
{
  size_t length = 0;

  while (length < request->target_length
         && request->target[length] != '?' && request->target[length] != '#')
    {
      length++;
    }
  return length;
}

static int
serve_file (const request_t *request, const sws_config_t *cfg,
            int keep_alive, int head_only, sws_http_response_t *response)
{
  char decoded[SWS_HTTP_MAX_PATH];
  char path[SWS_HTTP_MAX_PATH + SWS_HTTP_MAX_PATH];
  sws_stat_t info;
  body_t body;
  int status;
  int written;
  int result;

  status = decode_target (request->target, request->target_length, decoded,
                          sizeof (decoded));
  if (status != 0)
    {
      return error_response (response, status, NULL, head_only, 0);
    }
  if (!is_safe_path (decoded))
    {
      return error_response (response, HTTP_FORBIDDEN, NULL, head_only,
                             keep_alive);
    }

  written = snprintf (path, sizeof (path), "%s%s%s", cfg->root, decoded,
                      decoded[strlen (decoded) - 1] == '/' ? "index.html"
                                                            : "");
  if (written < 0 || (size_t)written >= sizeof (path))
    {
      return error_response (response, HTTP_URI_TOO_LONG, NULL, head_only,
                             keep_alive);
    }

  if (sws_stat (path, &info) != 0)
    {
      return error_response (response, HTTP_NOT_FOUND, NULL, head_only,
                             keep_alive);
    }
  if (SWS_IS_DIR (info.st_mode))
    {
      char location[SWS_HTTP_MAX_PATH + SWS_HTTP_MAX_PATH / 2];
      int location_length
          = snprintf (location, sizeof (location), "Location: %.*s/\r\n",
                      (int)target_path_length (request), request->target);

      if (location_length < 0 || (size_t)location_length >= sizeof (location))
        {
          return error_response (response, HTTP_URI_TOO_LONG, NULL, head_only,
                                 keep_alive);
        }
      return error_response (response, HTTP_MOVED_PERMANENTLY, location,
                             head_only, keep_alive);
    }
  if (!SWS_IS_REG (info.st_mode))
    {
      return error_response (response, HTTP_FORBIDDEN, NULL, head_only,
                             keep_alive);
    }
  if ((long long)info.st_size > SWS_HTTP_MAX_FILE_SIZE)
    {
      return error_response (response, HTTP_INTERNAL_SERVER_ERROR, NULL,
                             head_only, keep_alive);
    }

  body.memory = NULL;
  body.length = (size_t)info.st_size;
  body.omit = head_only;
  body.file = NULL;
  if (!head_only)
    {
      body.file = fopen (path, "rb");
      if (body.file == NULL)
        {
          return error_response (response,
                                 errno == ENOENT ? HTTP_NOT_FOUND
                                                 : HTTP_FORBIDDEN,
                                 NULL, head_only, keep_alive);
        }
    }

  result = assemble_response (response, HTTP_OK, mime_type_for (path), NULL,
                              &body, keep_alive);
  if (body.file != NULL)
    {
      fclose (body.file);
    }
  if (result != 0)
    {
      return error_response (response, HTTP_INTERNAL_SERVER_ERROR, NULL,
                             head_only, 0);
    }
  return 0;
}

static int
dispatch_request (const request_t *request, const sws_config_t *cfg,
                  int keep_alive, sws_http_response_t *response)
{
  if (request->method_length == strlen ("GET")
      && memcmp (request->method, "GET", request->method_length) == 0)
    {
      return serve_file (request, cfg, keep_alive, 0, response);
    }
  if (request->method_length == strlen ("HEAD")
      && memcmp (request->method, "HEAD", request->method_length) == 0)
    {
      return serve_file (request, cfg, keep_alive, 1, response);
    }
  return error_response (response, HTTP_METHOD_NOT_ALLOWED,
                         "Allow: GET, HEAD\r\n", 0, keep_alive);
}

int
sws_http_handle_request (const char *request_data, size_t length,
                         const sws_config_t *cfg,
                         sws_http_response_t *response)
{
  request_t request;
  int status;
  int keep_alive;
  int result;

  response->data = NULL;
  response->length = 0;

  status = parse_request (request_data, length, &request);
  if (status != 0)
    {
      result = error_response (response, status, NULL, 0, 0);
      if (cfg->verbose && result == 0)
        {
          sws_log_info ("[pid %ld] malformed request -> %d",
                        sws_platform_pid (), status);
        }
      return result;
    }

  keep_alive = cfg->keepalive_timeout > 0 && !request.has_body
               && (request.minor_version == 1 ? !request.wants_close
                                              : request.wants_keepalive);

  result = dispatch_request (&request, cfg, keep_alive, response);
  if (cfg->verbose && result == 0)
    {
      sws_log_info ("[pid %ld] %.*s %.*s HTTP/1.%d -> %d (%lu bytes)",
                    sws_platform_pid (), (int)request.method_length,
                    request.method, (int)request.target_length,
                    request.target, request.minor_version, response->status,
                    (unsigned long)response->length);
    }
  return result;
}
