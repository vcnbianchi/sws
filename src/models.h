#ifndef SWS_MODELS_H
#define SWS_MODELS_H

#include "config.h"
#include "platform.h"

int sws_model_fork_run (sws_socket_t listen_socket, const sws_config_t *cfg);

int sws_model_fork_worker (const sws_config_t *cfg);

int sws_model_thread_run (sws_socket_t listen_socket,
                          const sws_config_t *cfg);

int sws_model_event_run (sws_socket_t listen_socket, const sws_config_t *cfg);

const char *sws_model_mechanism (sws_model_t model);

#endif
