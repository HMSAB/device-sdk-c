/*
 * Copyright (c) 2018
 * IoTech Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include "consul.h"
#include "edgex/registry.h"
#include "edgex_rest.h"
#include "rest.h"
#include "errorlist.h"
#include "config.h"
#include "parson.h"
#include "base64.h"

#define CONF_PREFIX "config/V2/"

static edgex_nvpairs *read_pairs
(
  iot_logging_client *lc,
  const char *json,
  edgex_error *err
)
{
  const char *key;
  const char *keyindex;
  const char *enc;
  size_t nconfs;
  size_t rsize;
  JSON_Object *obj;
  edgex_nvpairs *pair;
  edgex_nvpairs *result = NULL;

  JSON_Value *val = json_parse_string (json);
  JSON_Array *configs = json_value_get_array (val);

  nconfs = json_array_get_count (configs);
  for (size_t i = 0; i < nconfs; i++)
  {
    obj = json_array_get_object (configs, i);
    pair = malloc (sizeof (edgex_nvpairs));
    key = json_object_get_string (obj, "Key");
    if (key)
    {
      keyindex = strchr (key, '/');
      if (keyindex)
      {
        keyindex = strchr (keyindex + 1, '/');
      }
      if (keyindex)
      {
        keyindex = strchr (keyindex + 1, '/');
      }
      if (keyindex)
      {
        pair->name = strdup (keyindex + 1);
      }
      else
      {
        iot_log_error (lc, "Unexpected Key %s returned from consul", key);
        *err = EDGEX_CONSUL_RESPONSE;
        free (pair);
        break;
      }
    }
    else
    {
      iot_log_error (lc, "No Key field in consul response. JSON was %s", json);
      *err = EDGEX_CONSUL_RESPONSE;
      break;
    }
    enc = json_object_get_string (obj, "Value");
    if (enc)
    {
      rsize = edgex_b64_maxdecodesize (enc);
      pair->value = malloc (rsize + 1);
      if (edgex_b64_decode (enc, pair->value, &rsize))
      {
        (pair->value)[rsize] = '\0';
      }
      else
      {
        iot_log_error
          (lc, "Unable to decode Value %s (for config key %s)", enc, key);
        *err = EDGEX_CONSUL_RESPONSE;
        free (pair->value);
        free (pair->name);
        free (pair);
        break;
      }
    }
    else
    {
      iot_log_error
        (lc, "No Value field in consul response. JSON was %s", json);
      *err = EDGEX_CONSUL_RESPONSE;
      free (pair->name);
      free (pair);
      break;
    }

    pair->next = result;
    result = pair;
  }
  json_value_free (val);
  return result;
}

edgex_nvpairs *edgex_consul_client_get_config
(
  iot_logging_client *lc,
  void *location,
  const char *servicename,
  const char *profile,
  edgex_error *err
)
{
  edgex_ctx ctx;
  char url[URL_BUF_SIZE];
  edgex_nvpairs *result = NULL;
  edgex_registry_hostport *endpoint = (edgex_registry_hostport *)location;

  memset (&ctx, 0, sizeof (edgex_ctx));
  if (profile && *profile)
  {
    snprintf
    (
      url, URL_BUF_SIZE - 1,
      "http://%s:%u/v1/kv/" CONF_PREFIX "%s;%s?recurse",
      endpoint->host, endpoint->port,
      servicename, profile
    );
  }
  else
  {
    snprintf
    (
      url, URL_BUF_SIZE - 1,
      "http://%s:%u/v1/kv/" CONF_PREFIX "%s?recurse",
      endpoint->host, endpoint->port, servicename
    );
  }
  edgex_http_get (lc, &ctx, url, edgex_http_write_cb, err);

  if (err->code == 0)
  {
    result = read_pairs (lc, ctx.buff, err);
    if (err->code)
    {
      edgex_nvpairs_free (result);
      result = NULL;
    }
  }

  free (ctx.buff);
  return result;
}

void edgex_consul_client_write_config
(
  iot_logging_client *lc,
  void *location,
  const char *servicename,
  const char *profile,
  const edgex_nvpairs *config,
  edgex_error *err
)
{
  edgex_ctx ctx;
  char url[URL_BUF_SIZE];
  edgex_registry_hostport *endpoint = (edgex_registry_hostport *)location;

  memset (&ctx, 0, sizeof (edgex_ctx));
  snprintf
  (
    url, URL_BUF_SIZE - 1, "http://%s:%u/v1/txn",
    endpoint->host, endpoint->port
  );

  JSON_Value *jresult = json_value_init_array ();
  JSON_Array *jarray = json_value_get_array (jresult);

  const edgex_nvpairs *iter = config;
  while (iter)
  {
    size_t valsz = strlen (iter->value);
    size_t base64sz = edgex_b64_encodesize (valsz);
    char *base64val = malloc (base64sz);
    edgex_b64_encode (iter->value, valsz, base64val, base64sz);

    size_t keysz =
      strlen (CONF_PREFIX) + strlen (servicename) + strlen (iter->name) + 2;
    if (profile && *profile)
    {
      keysz += (strlen (profile) + 1);
    }
    char *keystr = malloc (keysz);
    if (profile && *profile)
    {
      sprintf
        (keystr, "%s%s;%s/%s", CONF_PREFIX, servicename, profile, iter->name);
    }
    else
    {
      sprintf (keystr, "%s%s/%s", CONF_PREFIX, servicename, iter->name);
    }

    JSON_Value *kvfields = json_value_init_object ();
    JSON_Object *obj = json_value_get_object (kvfields);
    json_object_set_string (obj, "Verb", "set");
    json_object_set_string (obj, "Key", keystr);
    json_object_set_string (obj, "Value", base64val);

    JSON_Value *kvcmd = json_value_init_object ();
    json_object_set_value (json_value_get_object (kvcmd), "KV", kvfields);
    json_array_append_value (jarray, kvcmd);

    free (base64val);
    free (keystr);
    iter = iter->next;
  }

  char *json = json_serialize_to_string (jresult);
  json_value_free (jresult);

  edgex_http_put (lc, &ctx, url, json, edgex_http_write_cb, err);

  free (json);
  free (ctx.buff);
}

void edgex_consul_client_register_service
(
  iot_logging_client *lc,
  void *location,
  const char *servicename,
  const char *host,
  uint16_t port,
  const char *checkInterval,
  edgex_error *err
)
{
  edgex_ctx ctx;
  char url[URL_BUF_SIZE];
  char myUrl[URL_BUF_SIZE];
  char checkName[URL_BUF_SIZE];
  edgex_registry_hostport *endpoint = (edgex_registry_hostport *)location;

  memset (&ctx, 0, sizeof (edgex_ctx));
  snprintf
  (
    url, URL_BUF_SIZE - 1, "http://%s:%u/v1/agent/service/register",
    endpoint->host, endpoint->port
  );
  snprintf
    (myUrl, URL_BUF_SIZE - 1, "http://%s:%u/api/v1/ping", host, port);
  snprintf (checkName, URL_BUF_SIZE - 1, "Health Check: %s", servicename);

  JSON_Value *checkval = json_value_init_object ();
  JSON_Object *obj = json_value_get_object (checkval);
  json_object_set_string (obj, "Name", checkName);
  json_object_set_string (obj, "Interval", checkInterval);
  json_object_set_string (obj, "HTTP", myUrl);

  JSON_Value *params = json_value_init_object ();
  obj = json_value_get_object (params);
  json_object_set_string (obj, "Name", servicename);
  json_object_set_string (obj, "Address", host);
  json_object_set_number (obj, "Port", port);
  json_object_set_value (obj, "Check", checkval);

  char *json = json_serialize_to_string (params);
  json_value_free (params);

  edgex_http_put (lc, &ctx, url, json, edgex_http_write_cb, err);

  if (err->code)
  {
    iot_log_error (lc, "Register service failed: %s", ctx.buff);
  }
  free (json);
  free (ctx.buff);
}

bool edgex_consul_client_ping
(
  iot_logging_client *lc,
  void *location,
  edgex_error *err
)
{
  edgex_ctx ctx;
  char url[URL_BUF_SIZE];
  edgex_registry_hostport *endpoint = (edgex_registry_hostport *)location;

  memset (&ctx, 0, sizeof (edgex_ctx));
  snprintf
  (
    url,
    URL_BUF_SIZE - 1,
    "http://%s:%u/v1/status/leader",
    endpoint->host,
    endpoint->port
  );

  edgex_http_get (lc, &ctx, url, edgex_http_write_cb, err);
  free (ctx.buff);

  return (err->code == 0);
}
