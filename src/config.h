#ifndef SWS_CONFIG_H
#define SWS_CONFIG_H

#define SWS_VERSION "1.0.0"
#define SWS_SERVER_NAME "sws/" SWS_VERSION

#define SWS_DEFAULT_PORT 4545
#define SWS_DEFAULT_BACKLOG 1024
#define SWS_DEFAULT_ROOT "./www"
#define SWS_DEFAULT_KEEPALIVE_TIMEOUT 5

#define SWS_REQUEST_TIMEOUT 5

typedef enum
{
  SWS_MODEL_NONE = 0,
  SWS_MODEL_FORK,
  SWS_MODEL_THREAD,
  SWS_MODEL_EVENT
} sws_model_t;

typedef struct
{
  sws_model_t model;
  unsigned short port;
  int backlog;
  const char *root;
  int keepalive_timeout;
  int verbose;
  int worker;
} sws_config_t;

const char *sws_model_name (sws_model_t model);

#endif
