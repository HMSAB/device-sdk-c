/*
 * Copyright (c) 2018
 * IoTech Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include "discovery.h"
#include "service.h"
#include "profiles.h"
#include "metadata.h"
#include "edgex_rest.h"

#include <string.h>
#include <stdlib.h>

#include <microhttpd.h>

int edgex_device_handler_discovery
(
  void *ctx,
  char *url,
  edgex_http_method method,
  const char *upload_data,
  size_t upload_data_size,
  char **reply,
  const char **reply_type
)
{
  edgex_device_service *svc = (edgex_device_service *) ctx;

  if (svc->adminstate == LOCKED || svc->opstate == DISABLED)
  {
    return MHD_HTTP_LOCKED;
  }

  if (!svc->config.device.discovery)
  {
    *reply = strdup ("Discovery disabled by configuration\n");
    return MHD_HTTP_SERVICE_UNAVAILABLE;
  }

  if (pthread_mutex_trylock (&svc->discolock) == 0)
  {
    thpool_add_work (svc->thpool, edgex_device_handler_do_discovery, svc);
    pthread_mutex_unlock (&svc->discolock);
  }
  // else discovery was already running; ignore this request

  *reply = strdup ("Running discovery\n");
  return MHD_HTTP_OK;
}

void edgex_device_handler_do_discovery (void *p)
{
  edgex_device_service *svc = (edgex_device_service *) p;

  pthread_mutex_lock (&svc->discolock);
  svc->userfns.discover (svc->userdata);
  pthread_mutex_unlock (&svc->discolock);
}
