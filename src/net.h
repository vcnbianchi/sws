#ifndef SWS_NET_H
#define SWS_NET_H

#include "platform.h"

#include <stddef.h>

sws_socket_t sws_net_listen (unsigned short port, int backlog);

void sws_net_prepare_client (sws_socket_t client);

int sws_net_send_all (sws_socket_t sock, const char *data, size_t length);

int sws_net_handle_accept_error (int may_sleep);

#endif
