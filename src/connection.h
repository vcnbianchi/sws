#ifndef SWS_CONNECTION_H
#define SWS_CONNECTION_H

#include "config.h"
#include "platform.h"

void sws_connection_serve (sws_socket_t client, const sws_config_t *cfg);

#endif
