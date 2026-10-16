#ifndef SWS_CLI_H
#define SWS_CLI_H

#include "config.h"

typedef enum
{
  SWS_CLI_RUN,
  SWS_CLI_EXIT_OK,
  SWS_CLI_EXIT_FAILURE
} sws_cli_result_t;

sws_cli_result_t sws_cli_parse (int argc, char **argv, sws_config_t *cfg);

#endif
