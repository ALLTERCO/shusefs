#include "../include/device_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/mongoose.h"

/* ============================================================================
 * INITIALIZATION AND CLEANUP
 * ============================================================================
 */

int device_state_init(device_state_t *state) {
  if (!state) {
    return -1;
  }

  memset(state, 0, sizeof(device_state_t));

  /* Initialize single mutex for entire device state */
  if (pthread_mutex_init(&state->mutex, NULL) != 0) {
    return -1;
  }

  state->sys_config.raw_json = NULL;
  state->sys_config.json_len = 0;
  state->sys_config.valid = 0;
  state->sys_config.parsed.timezone = NULL;

  state->mqtt_config.raw_json = NULL;
  state->mqtt_config.json_len = 0;
  state->mqtt_config.valid = 0;

  /* Initialize scripts */
  state->scripts.count = 0;
  state->scripts.last_update = 0;
  state->scripts.retrieving_id = -1;
  state->scripts.current_offset = 0;
  state->scripts.chunk_buffer = NULL;
  state->scripts.chunk_buffer_size = 0;
  for (int i = 0; i < MAX_SCRIPTS; i++) {
    state->scripts.scripts[i] = (script_entry_t) {.id = -1,
                                                  .name = {0},
                                                  .enable = false,
                                                  .code = NULL,
                                                  .create_time = 0,
                                                  .modify_time = 0,
                                                  .valid = 0,
                                                  .running = false,
                                                  .mem_used = 0,
                                                  .mem_peak = 0,
                                                  .errors = NULL,
                                                  .last_status_update = 0,
                                                  .last_upload_req_id = -1};
  }

  /* Initialize switches */
  state->switches.count = 0;
  state->switches.last_update = 0;
  for (int i = 0; i < MAX_SWITCHES; i++) {
    memset(&state->switches.switches[i], 0, sizeof(switch_config_t));
    state->switches.switches[i].id = -1;
    state->switches.switches[i].valid = 0;
  }

  /* Initialize inputs */
  state->inputs.count = 0;
  state->inputs.last_update = 0;
  for (int i = 0; i < MAX_INPUTS; i++) {
    memset(&state->inputs.inputs[i], 0, sizeof(input_config_t));
    state->inputs.inputs[i].id = -1;
    state->inputs.inputs[i].valid = 0;
  }

  /* Initialize schedules */
  state->schedules.count = 0;
  state->schedules.rev = 0;
  state->schedules.last_update = 0;
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    memset(&state->schedules.schedules[i], 0, sizeof(schedule_entry_t));
    state->schedules.schedules[i].id = -1;
    state->schedules.schedules[i].valid = 0;
    for (int j = 0; j < MAX_SCHEDULE_CALLS; j++) {
      state->schedules.schedules[i].calls[j].params_json = NULL;
    }
  }

  return 0;
}

void device_state_destroy(device_state_t *state) {
  if (!state) {
    return;
  }

  pthread_mutex_lock(&state->mutex);

  /* Clean up sys_config */
  if (state->sys_config.raw_json) {
    free(state->sys_config.raw_json);
    state->sys_config.raw_json = NULL;
  }

  if (state->sys_config.parsed.timezone) {
    free(state->sys_config.parsed.timezone);
    state->sys_config.parsed.timezone = NULL;
  }

  /* Clean up mqtt_config */
  if (state->mqtt_config.raw_json) {
    free(state->mqtt_config.raw_json);
    state->mqtt_config.raw_json = NULL;
  }

  /* Clean up scripts */
  for (int i = 0; i < MAX_SCRIPTS; i++) {
    if (state->scripts.scripts[i].code) {
      free(state->scripts.scripts[i].code);
      state->scripts.scripts[i].code = NULL;
    }
    if (state->scripts.scripts[i].errors) {
      free(state->scripts.scripts[i].errors);
      state->scripts.scripts[i].errors = NULL;
    }
  }

  /* Clean up chunk buffer */
  if (state->scripts.chunk_buffer) {
    free(state->scripts.chunk_buffer);
    state->scripts.chunk_buffer = NULL;
  }

  /* Clean up switches */
  for (int i = 0; i < MAX_SWITCHES; i++) {
    if (state->switches.switches[i].raw_json) {
      free(state->switches.switches[i].raw_json);
      state->switches.switches[i].raw_json = NULL;
    }
  }

  /* Clean up inputs */
  for (int i = 0; i < MAX_INPUTS; i++) {
    if (state->inputs.inputs[i].raw_json) {
      free(state->inputs.inputs[i].raw_json);
      state->inputs.inputs[i].raw_json = NULL;
    }
  }

  /* Clean up schedules */
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    for (int j = 0; j < MAX_SCHEDULE_CALLS; j++) {
      if (state->schedules.schedules[i].calls[j].params_json) {
        free(state->schedules.schedules[i].calls[j].params_json);
        state->schedules.schedules[i].calls[j].params_json = NULL;
      }
    }
  }

  pthread_mutex_unlock(&state->mutex);
  pthread_mutex_destroy(&state->mutex);
}

/* ============================================================================
 * JSON-RPC UTILITIES
 * ============================================================================
 */

char *jsonrpc_build_request(const char *method, int id, const char *params) {
  if (!method) {
    return NULL;
  }

  /* Generate a consistent client ID for this session */
  /* Using a simple static ID - in production you might want a UUID */
  static const char *client_id = "shusefs-client";

  char *request = NULL;
  int result;

  if (params && strlen(params) > 0) {
    result = asprintf(&request,
                      "{\"jsonrpc\":\"2.0\",\"id\":%d,\"src\":\"%s\","
                      "\"method\":\"%s\",\"params\":%s}",
                      id, client_id, method, params);
  } else {
    result = asprintf(
        &request,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"src\":\"%s\",\"method\":\"%s\"}", id,
        client_id, method);
  }

  /* Check if allocation was successful */
  if (result < 0) {
    return NULL;
  }

  return request;
}

int jsonrpc_send_request(struct mg_connection *conn, const char *request) {
  if (!conn || !request) {
    return -1;
  }

  mg_ws_send(conn, request, strlen(request), WEBSOCKET_OP_TEXT);
  return 0;
}

int jsonrpc_is_error(const char *response_json, char *error_buf,
                     size_t error_buf_size) {
  if (!response_json) {
    return 0;
  }

  struct mg_str json_str = mg_str(response_json);

  /* Check if error field exists */
  int error_len = 0;
  int error_pos = mg_json_get(json_str, "$.error", &error_len);

  if (error_pos < 0) {
    /* No error field found */
    return 0;
  }

  /* Error found - optionally extract error message */
  if (error_buf && error_buf_size > 0) {
    /* Create substring view of error object */
    struct mg_str error_obj = mg_str_n(response_json + error_pos, error_len);

    /* Try to get error message */
    char *message = mg_json_get_str(error_obj, "$.message");
    if (message) {
      snprintf(error_buf, error_buf_size, "%s", message);
      free(message);
    } else {
      /* If no message field, copy the entire error object */
      size_t copy_len = (size_t) error_len < error_buf_size - 1
                            ? (size_t) error_len
                            : error_buf_size - 1;
      memcpy(error_buf, response_json + error_pos, copy_len);
      error_buf[copy_len] = '\0';
    }
  }

  return 1;
}

response_type_t device_state_get_response_type(const char *request_json) {
  if (!request_json) {
    return RESPONSE_TYPE_UNKNOWN;
  }

  struct mg_str json_str = mg_str(request_json);
  char *method = mg_json_get_str(json_str, "$.method");

  response_type_t result = RESPONSE_TYPE_UNKNOWN;

  if (method) {
    if (strstr(method, "Sys.GetConfig") != NULL) {
      result = RESPONSE_TYPE_SYS_GETCONFIG;
    } else if (strstr(method, "Sys.SetConfig") != NULL) {
      result = RESPONSE_TYPE_SYS_SETCONFIG;
    } else if (strstr(method, "MQTT.GetConfig") != NULL) {
      result = RESPONSE_TYPE_MQTT_GETCONFIG;
    } else if (strstr(method, "MQTT.SetConfig") != NULL) {
      result = RESPONSE_TYPE_MQTT_SETCONFIG;
    } else if (strstr(method, "Switch.GetConfig") != NULL) {
      result = RESPONSE_TYPE_SWITCH_GETCONFIG;
    } else if (strstr(method, "Switch.SetConfig") != NULL) {
      result = RESPONSE_TYPE_SWITCH_SETCONFIG;
    } else if (strstr(method, "Switch.Set") != NULL) {
      result = RESPONSE_TYPE_SWITCH_SET;
    } else if (strstr(method, "Switch.GetStatus") != NULL) {
      result = RESPONSE_TYPE_SWITCH_GETSTATUS;
    } else if (strstr(method, "Input.GetConfig") != NULL) {
      result = RESPONSE_TYPE_INPUT_GETCONFIG;
    } else if (strstr(method, "Input.SetConfig") != NULL) {
      result = RESPONSE_TYPE_INPUT_SETCONFIG;
    } else if (strstr(method, "Input.GetStatus") != NULL) {
      result = RESPONSE_TYPE_INPUT_GETSTATUS;
    } else if (strstr(method, "Script.List") != NULL) {
      result = RESPONSE_TYPE_SCRIPT_LIST;
    } else if (strstr(method, "Script.GetCode") != NULL) {
      result = RESPONSE_TYPE_SCRIPT_GETCODE;
    } else if (strstr(method, "Script.PutCode") != NULL) {
      result = RESPONSE_TYPE_SCRIPT_PUTCODE;
    } else if (strstr(method, "Schedule.List") != NULL) {
      result = RESPONSE_TYPE_SCHEDULE_LIST;
    } else if (strstr(method, "Schedule.Create") != NULL) {
      result = RESPONSE_TYPE_SCHEDULE_CREATE;
    } else if (strstr(method, "Schedule.Update") != NULL) {
      result = RESPONSE_TYPE_SCHEDULE_UPDATE;
    } else if (strstr(method, "Schedule.Delete") != NULL) {
      result = RESPONSE_TYPE_SCHEDULE_DELETE;
    }
    /* Add more response types here as needed */

    free(method);
  }

  return result;
}

int device_state_extract_script_id(const char *request_json) {
  if (!request_json) {
    return -1;
  }

  struct mg_str json_str = mg_str(request_json);

  /* Get params object position and length */
  int params_len = 0;
  int params_pos = mg_json_get(json_str, "$.params", &params_len);

  if (params_pos < 0) {
    return -1;
  }

  /* Create substring view of params object */
  struct mg_str params_str = mg_str_n(request_json + params_pos, params_len);

  double id_val = 0;
  int result = -1;

  if (mg_json_get_num(params_str, "$.id", &id_val)) {
    result = (int) id_val;
  }

  return result;
}

int device_state_extract_switch_id(const char *request_json) {
  if (!request_json) {
    return -1;
  }

  struct mg_str json_str = mg_str(request_json);

  /* Get params object position and length */
  int params_len = 0;
  int params_pos = mg_json_get(json_str, "$.params", &params_len);

  if (params_pos < 0) {
    return -1;
  }

  /* Create substring view of params object */
  struct mg_str params_str = mg_str_n(request_json + params_pos, params_len);

  double id_val = 0;
  int result = -1;

  if (mg_json_get_num(params_str, "$.id", &id_val)) {
    result = (int) id_val;
  }

  return result;
}

int device_state_extract_input_id(const char *request_json) {
  if (!request_json) {
    return -1;
  }

  struct mg_str json_str = mg_str(request_json);

  /* Get params object position and length */
  int params_len = 0;
  int params_pos = mg_json_get(json_str, "$.params", &params_len);

  if (params_pos < 0) {
    return -1;
  }

  /* Create substring view of params object */
  struct mg_str params_str = mg_str_n(request_json + params_pos, params_len);

  double id_val = 0;
  int result = -1;

  if (mg_json_get_num(params_str, "$.id", &id_val)) {
    result = (int) id_val;
  }

  return result;
}

/* ============================================================================
 * SYSTEM CONFIGURATION (Sys.GetConfig / Sys.SetConfig)
 * ============================================================================
 */

int device_state_request_sys_config(device_state_t *state,
                                    request_queue_t *queue,
                                    struct mg_connection *conn) {
  if (!state || !queue || !conn) {
    return -1;
  }

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build JSON-RPC request with correct ID */
  char *request = jsonrpc_build_request("Sys.GetConfig", req_id, NULL);
  if (!request) {
    fprintf(stderr, "Error: Failed to build Sys.GetConfig request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Sys.GetConfig to request queue\n");
    free(request);
    return -1;
  }

  printf("Requesting system configuration (ID: %d)...\n", req_id);

  /* Request is queued and will be sent by ws_thread_func */
  free(request);
  return req_id;
}

int device_state_update_sys_config(device_state_t *state, const char *json) {
  if (!state || !json) {
    return -1;
  }

  /* Parse common fields using mongoose JSON parser */
  struct mg_str json_str = mg_str(json);

  /* Extract the result object (not as string, but as JSON substring) */
  int result_len = 0;
  int result_pos = mg_json_get(json_str, "$.result", &result_len);

  if (result_pos < 0 || result_len <= 0) {
    fprintf(stderr, "Error: No result field in sys config response\n");
    return -1;
  }

  /* Allocate and copy the result portion */
  char *result_str = malloc(result_len + 1);
  if (!result_str) {
    return -1;
  }
  memcpy(result_str, json + result_pos, result_len);
  result_str[result_len] = '\0';

  pthread_mutex_lock(&state->mutex);

  /* Free old data */
  if (state->sys_config.raw_json) {
    free(state->sys_config.raw_json);
  }
  if (state->sys_config.parsed.timezone) {
    free(state->sys_config.parsed.timezone);
    state->sys_config.parsed.timezone = NULL;
  }

  /* Store only the result portion as raw JSON */
  size_t json_len = strlen(result_str);
  state->sys_config.raw_json = malloc(json_len + 1);
  if (!state->sys_config.raw_json) {
    free(result_str);
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }
  strcpy(state->sys_config.raw_json, result_str);
  state->sys_config.json_len = json_len;

  struct mg_str result = mg_str(result_str);

  /* Extract device name */
  char *name = mg_json_get_str(result, "$.device.name");
  if (name) {
    snprintf(state->sys_config.parsed.device_name, MAX_DEVICE_NAME, "%s", name);
    free(name);
  }

  /* Extract location */
  char *loc = mg_json_get_str(result, "$.location.tz");
  if (loc) {
    snprintf(state->sys_config.parsed.location, MAX_LOCATION, "%s", loc);
    free(loc);
  }

  /* Extract eco_mode */
  double eco_val = 0;
  if (mg_json_get_num(result, "$.device.eco_mode", &eco_val)) {
    state->sys_config.parsed.eco_mode = (eco_val != 0);
  }

  /* Extract SNTP enabled */
  double sntp_val = 0;
  if (mg_json_get_num(result, "$.sys.sntp.enable", &sntp_val)) {
    state->sys_config.parsed.sntp_enabled = (int) sntp_val;
  }

  free(result_str);

  state->sys_config.valid = 1;
  state->sys_config.last_update = time(NULL);

  printf("System configuration updated successfully\n");

  pthread_mutex_unlock(&state->mutex);
  return 0;
}

int device_state_get_sys_config_str(device_state_t *state, char **output) {
  if (!state || !output) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  if (!state->sys_config.valid || !state->sys_config.raw_json) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  *output = strdup(state->sys_config.raw_json);

  pthread_mutex_unlock(&state->mutex);

  return (*output) ? 0 : -1;
}

int device_state_serialize_sys_config(device_state_t *state) {
  if (!state) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  if (!state->sys_config.valid) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  /* Build JSON from parsed fields */
  char buffer[MAX_CONFIG_SIZE];
  int offset = snprintf(buffer, sizeof(buffer), "{\"result\":{\"device\":{");

  if (state->sys_config.parsed.device_name[0] != '\0') {
    offset +=
        snprintf(buffer + offset, sizeof(buffer) - offset, "\"name\":\"%s\",",
                 state->sys_config.parsed.device_name);
  }

  offset +=
      snprintf(buffer + offset, sizeof(buffer) - offset, "\"eco_mode\":%s",
               state->sys_config.parsed.eco_mode ? "true" : "false");

  offset += snprintf(buffer + offset, sizeof(buffer) - offset, "}");

  if (state->sys_config.parsed.location[0] != '\0') {
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       ",\"location\":{\"tz\":\"%s\"}",
                       state->sys_config.parsed.location);
  }

  offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                     ",\"sys\":{\"sntp\":{\"enable\":%d}}",
                     state->sys_config.parsed.sntp_enabled);

  offset += snprintf(buffer + offset, sizeof(buffer) - offset, "}}");

  /* Replace old raw_json with serialized version */
  if (state->sys_config.raw_json) {
    free(state->sys_config.raw_json);
  }

  state->sys_config.raw_json = strdup(buffer);
  if (!state->sys_config.raw_json) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }
  state->sys_config.json_len = strlen(buffer);

  pthread_mutex_unlock(&state->mutex);
  return 0;
}

int device_state_set_sys_config(device_state_t *state, request_queue_t *queue,
                                struct mg_connection *conn) {
  if (!state || !queue || !conn) {
    return -1;
  }

  /* Serialize parsed fields back to JSON before sending */
  if (device_state_serialize_sys_config(state) != 0) {
    fprintf(stderr, "Error: Failed to serialize system configuration\n");
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  if (!state->sys_config.valid || !state->sys_config.raw_json) {
    pthread_mutex_unlock(&state->mutex);
    fprintf(stderr, "Error: No valid system configuration to set\n");
    return -1;
  }

  /* Extract the config params from the stored JSON */
  struct mg_str json_str = mg_str(state->sys_config.raw_json);
  int result_len = 0;
  int result_pos = mg_json_get(json_str, "$.result", &result_len);
  char *result_str = NULL;

  if (result_pos >= 0 && result_len > 0) {
    result_str = malloc(result_len + 1);
    if (result_str) {
      memcpy(result_str, state->sys_config.raw_json + result_pos, result_len);
      result_str[result_len] = '\0';
    }
  }

  char *params = NULL;
  if (result_str) {
    params = strdup(result_str);
    free(result_str);
  } else {
    pthread_mutex_unlock(&state->mutex);
    fprintf(stderr, "Error: Failed to extract config from stored data\n");
    return -1;
  }

  pthread_mutex_unlock(&state->mutex);

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    free(params);
    return -1;
  }

  /* Build JSON-RPC request with correct ID */
  char *request = jsonrpc_build_request("Sys.SetConfig", req_id, params);
  free(params);

  if (!request) {
    fprintf(stderr, "Error: Failed to build Sys.SetConfig request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Sys.SetConfig to request queue\n");
    free(request);
    return -1;
  }

  printf("Setting system configuration (ID: %d)...\n", req_id);

  /* Request is queued and will be sent by ws_thread_func */
  free(request);
  return req_id;
}

int device_state_set_sys_config_from_json(const char *user_json,
                                          request_queue_t *queue,
                                          struct mg_connection *conn) {
  if (!user_json || !queue || !conn) {
    return -1;
  }

  /* Validate that user_json is valid JSON */
  struct mg_str json_str = mg_str(user_json);
  int dummy_len = 0;
  int validation = mg_json_get(json_str, "$", &dummy_len);
  if (validation < 0) {
    fprintf(stderr, "Error: Invalid JSON provided by user\n");
    return -1;
  }

  /* Build params: {"config": {user_json}} */
  size_t params_size = strlen(user_json) + 32;
  char *params = malloc(params_size);
  if (!params) {
    fprintf(stderr, "Error: Failed to allocate params buffer\n");
    return -1;
  }
  snprintf(params, params_size, "{\"config\":%s}", user_json);

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    free(params);
    return -1;
  }

  /* Build JSON-RPC request */
  char *request = jsonrpc_build_request("Sys.SetConfig", req_id, params);
  free(params);

  if (!request) {
    fprintf(stderr, "Error: Failed to build Sys.SetConfig request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Sys.SetConfig to request queue\n");
    free(request);
    return -1;
  }

  printf("Setting system configuration from user edit (ID: %d)...\n", req_id);

  /* Request is queued and will be sent by ws_thread_func */
  free(request);
  return req_id;
}

int device_state_is_sys_config_notification(const char *json, size_t len) {
  return device_state_is_component_notification(json, len, "sys");
}

/* ============================================================================
 * MQTT CONFIGURATION (MQTT.GetConfig / MQTT.SetConfig)
 * ============================================================================
 */

int device_state_request_mqtt_config(device_state_t *state,
                                     request_queue_t *queue,
                                     struct mg_connection *conn) {
  if (!state || !queue || !conn) {
    return -1;
  }

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build JSON-RPC request with correct ID */
  char *request = jsonrpc_build_request("MQTT.GetConfig", req_id, NULL);
  if (!request) {
    fprintf(stderr, "Error: Failed to build MQTT.GetConfig request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add MQTT.GetConfig to request queue\n");
    free(request);
    return -1;
  }

  printf("Requesting MQTT configuration (ID: %d)...\n", req_id);

  /* Request is queued and will be sent by ws_thread_func */
  free(request);
  return req_id;
}

int device_state_update_mqtt_config(device_state_t *state, const char *json) {
  if (!state || !json) {
    return -1;
  }

  /* Parse common fields using mongoose JSON parser */
  struct mg_str json_str = mg_str(json);

  /* Extract the result object (not as string, but as JSON substring) */
  int result_len = 0;
  int result_pos = mg_json_get(json_str, "$.result", &result_len);

  if (result_pos < 0 || result_len <= 0) {
    fprintf(stderr, "Error: No result field in mqtt config response\n");
    return -1;
  }

  /* Allocate and copy the result portion */
  char *result_str = malloc(result_len + 1);
  if (!result_str) {
    return -1;
  }
  memcpy(result_str, json + result_pos, result_len);
  result_str[result_len] = '\0';

  pthread_mutex_lock(&state->mutex);

  /* Free old data */
  if (state->mqtt_config.raw_json) {
    free(state->mqtt_config.raw_json);
  }

  /* Store only the result portion as raw JSON */
  size_t json_len = strlen(result_str);
  state->mqtt_config.raw_json = malloc(json_len + 1);
  if (!state->mqtt_config.raw_json) {
    free(result_str);
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }
  strcpy(state->mqtt_config.raw_json, result_str);
  state->mqtt_config.json_len = json_len;

  struct mg_str result = mg_str(result_str);

  /* Extract enable */
  double enable_val = 0;
  if (mg_json_get_num(result, "$.enable", &enable_val)) {
    state->mqtt_config.parsed.enable = (enable_val != 0);
  }

  /* Extract server */
  char *server = mg_json_get_str(result, "$.server");
  if (server) {
    snprintf(state->mqtt_config.parsed.server, MAX_SERVER_URL, "%s", server);
    free(server);
  }

  /* Extract client_id */
  char *client_id = mg_json_get_str(result, "$.client_id");
  if (client_id) {
    snprintf(state->mqtt_config.parsed.client_id, MAX_CLIENT_ID, "%s",
             client_id);
    free(client_id);
  }

  /* Extract user */
  char *user = mg_json_get_str(result, "$.user");
  if (user) {
    snprintf(state->mqtt_config.parsed.user, MAX_USER_ID, "%s", user);
    free(user);
  }

  /* Extract topic_prefix */
  char *topic_prefix = mg_json_get_str(result, "$.topic_prefix");
  if (topic_prefix) {
    snprintf(state->mqtt_config.parsed.topic_prefix, MAX_TOPIC_PREFIX, "%s",
             topic_prefix);
    free(topic_prefix);
  }

  /* Extract ssl_ca */
  char *ssl_ca_str = mg_json_get_str(result, "$.ssl_ca");
  if (ssl_ca_str) {
    if (strcmp(ssl_ca_str, "user_ca.pem") == 0) {
      state->mqtt_config.parsed.ssl_ca = SSL_CA_USER;
    } else if (strcmp(ssl_ca_str, "ca.pem") == 0) {
      state->mqtt_config.parsed.ssl_ca = SSL_CA_DEFAULT;
    } else {
      state->mqtt_config.parsed.ssl_ca = SSL_CA_NONE;
    }
    free(ssl_ca_str);
  }

  /* Extract enable_control */
  double enable_control_val = 0;
  if (mg_json_get_num(result, "$.enable_control", &enable_control_val)) {
    state->mqtt_config.parsed.enable_control = (enable_control_val != 0);
  }

  /* Extract rpc_ntf */
  double rpc_ntf_val = 0;
  if (mg_json_get_num(result, "$.rpc_ntf", &rpc_ntf_val)) {
    state->mqtt_config.parsed.rpc_ntf = (rpc_ntf_val != 0);
  }

  /* Extract status_ntf */
  double status_ntf_val = 0;
  if (mg_json_get_num(result, "$.status_ntf", &status_ntf_val)) {
    state->mqtt_config.parsed.status_ntf = (status_ntf_val != 0);
  }

  /* Extract use_client_cert */
  double use_client_cert_val = 0;
  if (mg_json_get_num(result, "$.use_client_cert", &use_client_cert_val)) {
    state->mqtt_config.parsed.use_client_cert = (use_client_cert_val != 0);
  }

  /* Extract enable_rpc */
  double enable_rpc_val = 0;
  if (mg_json_get_num(result, "$.enable_rpc", &enable_rpc_val)) {
    state->mqtt_config.parsed.enable_rpc = (enable_rpc_val != 0);
  }

  free(result_str);

  state->mqtt_config.valid = 1;
  state->mqtt_config.last_update = time(NULL);

  printf("MQTT configuration updated successfully\n");

  pthread_mutex_unlock(&state->mutex);
  return 0;
}

int device_state_get_mqtt_config_str(device_state_t *state, char **output) {
  if (!state || !output) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  if (!state->mqtt_config.valid || !state->mqtt_config.raw_json) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  *output = strdup(state->mqtt_config.raw_json);

  pthread_mutex_unlock(&state->mutex);

  return (*output) ? 0 : -1;
}

int device_state_serialize_mqtt_config(device_state_t *state) {
  if (!state) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  if (!state->mqtt_config.valid) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  /* Build JSON from parsed fields */
  char buffer[MAX_CONFIG_SIZE];
  int offset = snprintf(buffer, sizeof(buffer), "{\"result\":{");

  offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\"enable\":%s",
                     state->mqtt_config.parsed.enable ? "true" : "false");

  if (state->mqtt_config.parsed.server[0] != '\0') {
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       ",\"server\":\"%s\"", state->mqtt_config.parsed.server);
  }

  if (state->mqtt_config.parsed.client_id[0] != '\0') {
    offset +=
        snprintf(buffer + offset, sizeof(buffer) - offset,
                 ",\"client_id\":\"%s\"", state->mqtt_config.parsed.client_id);
  }

  if (state->mqtt_config.parsed.user[0] != '\0') {
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       ",\"user\":\"%s\"", state->mqtt_config.parsed.user);
  }

  if (state->mqtt_config.parsed.topic_prefix[0] != '\0') {
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       ",\"topic_prefix\":\"%s\"",
                       state->mqtt_config.parsed.topic_prefix);
  }

  /* Convert ssl_ca enum to string */
  const char *ssl_ca_str = NULL;
  switch (state->mqtt_config.parsed.ssl_ca) {
    case SSL_CA_USER: ssl_ca_str = "user_ca.pem"; break;
    case SSL_CA_DEFAULT: ssl_ca_str = "ca.pem"; break;
    case SSL_CA_NONE:
    default: ssl_ca_str = NULL; break;
  }

  if (ssl_ca_str) {
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       ",\"ssl_ca\":\"%s\"", ssl_ca_str);
  }

  offset += snprintf(
      buffer + offset, sizeof(buffer) - offset, ",\"enable_control\":%s",
      state->mqtt_config.parsed.enable_control ? "true" : "false");

  offset +=
      snprintf(buffer + offset, sizeof(buffer) - offset, ",\"rpc_ntf\":%s",
               state->mqtt_config.parsed.rpc_ntf ? "true" : "false");

  offset +=
      snprintf(buffer + offset, sizeof(buffer) - offset, ",\"status_ntf\":%s",
               state->mqtt_config.parsed.status_ntf ? "true" : "false");

  offset += snprintf(
      buffer + offset, sizeof(buffer) - offset, ",\"use_client_cert\":%s",
      state->mqtt_config.parsed.use_client_cert ? "true" : "false");

  offset +=
      snprintf(buffer + offset, sizeof(buffer) - offset, ",\"enable_rpc\":%s",
               state->mqtt_config.parsed.enable_rpc ? "true" : "false");

  offset += snprintf(buffer + offset, sizeof(buffer) - offset, "}}");

  /* Replace old raw_json with serialized version */
  if (state->mqtt_config.raw_json) {
    free(state->mqtt_config.raw_json);
  }

  state->mqtt_config.raw_json = strdup(buffer);
  if (!state->mqtt_config.raw_json) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }
  state->mqtt_config.json_len = strlen(buffer);

  pthread_mutex_unlock(&state->mutex);
  return 0;
}

int device_state_set_mqtt_config(device_state_t *state, request_queue_t *queue,
                                 struct mg_connection *conn) {
  if (!state || !queue || !conn) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  if (!state->mqtt_config.valid || !state->mqtt_config.raw_json) {
    pthread_mutex_unlock(&state->mutex);
    fprintf(stderr, "Error: No valid MQTT configuration to set\n");
    return -1;
  }

  /* Check if raw_json already has result field (from direct file write) */
  struct mg_str json_str = mg_str(state->mqtt_config.raw_json);
  int result_len = 0;
  int result_pos = mg_json_get(json_str, "$.result", &result_len);
  char *result_str = NULL;

  if (result_pos >= 0 && result_len > 0) {
    /* Extract the result object as a string */
    result_str = malloc(result_len + 1);
    if (result_str) {
      memcpy(result_str, state->mqtt_config.raw_json + result_pos, result_len);
      result_str[result_len] = '\0';
    }
  }

  pthread_mutex_unlock(&state->mutex);

  /* If no result field, serialize from parsed fields */
  if (!result_str) {
    if (device_state_serialize_mqtt_config(state) != 0) {
      fprintf(stderr, "Error: Failed to serialize MQTT configuration\n");
      return -1;
    }

    pthread_mutex_lock(&state->mutex);
    json_str = mg_str(state->mqtt_config.raw_json);
    result_pos = mg_json_get(json_str, "$.result", &result_len);

    if (result_pos >= 0 && result_len > 0) {
      result_str = malloc(result_len + 1);
      if (result_str) {
        memcpy(result_str, state->mqtt_config.raw_json + result_pos,
               result_len);
        result_str[result_len] = '\0';
      }
    }
    pthread_mutex_unlock(&state->mutex);

    if (!result_str) {
      fprintf(stderr, "Error: Failed to extract config from stored data\n");
      return -1;
    }
  }

  char *params = strdup(result_str);
  free(result_str);

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    free(params);
    return -1;
  }

  /* Build JSON-RPC request with correct ID */
  char *request = jsonrpc_build_request("MQTT.SetConfig", req_id, params);
  free(params);

  if (!request) {
    fprintf(stderr, "Error: Failed to build MQTT.SetConfig request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add MQTT.SetConfig to request queue\n");
    free(request);
    return -1;
  }

  printf("Setting MQTT configuration (ID: %d)...\n", req_id);

  /* Request is queued and will be sent by ws_thread_func */
  free(request);
  return req_id;
}

int device_state_set_mqtt_config_from_json(const char *user_json,
                                           request_queue_t *queue,
                                           struct mg_connection *conn) {
  if (!user_json || !queue || !conn) {
    return -1;
  }

  /* Validate that user_json is valid JSON */
  struct mg_str json_str = mg_str(user_json);
  int dummy_len = 0;
  int validation = mg_json_get(json_str, "$", &dummy_len);
  if (validation < 0) {
    fprintf(stderr, "Error: Invalid JSON provided by user\n");
    return -1;
  }

  /* Build params: {"config": {user_json}} */
  size_t params_size = strlen(user_json) + 32;
  char *params = malloc(params_size);
  if (!params) {
    fprintf(stderr, "Error: Failed to allocate params buffer\n");
    return -1;
  }
  snprintf(params, params_size, "{\"config\":%s}", user_json);

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    free(params);
    return -1;
  }

  /* Build JSON-RPC request */
  char *request = jsonrpc_build_request("MQTT.SetConfig", req_id, params);
  free(params);

  if (!request) {
    fprintf(stderr, "Error: Failed to build MQTT.SetConfig request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add MQTT.SetConfig to request queue\n");
    free(request);
    return -1;
  }

  printf("Setting MQTT configuration from user edit (ID: %d)...\n", req_id);

  /* Request is queued and will be sent by ws_thread_func */
  free(request);
  return req_id;
}

int device_state_is_mqtt_config_notification(const char *json, size_t len) {
  return device_state_is_component_notification(json, len, "mqtt");
}

/* ============================================================================
 * SWITCH CONFIGURATION (Switch.GetConfig / Switch.SetConfig)
 * ============================================================================
 */

static switch_in_mode_t parse_switch_in_mode(const char *mode_str) {
  if (!mode_str) return SWITCH_IN_MODE_UNKNOWN;
  if (strcmp(mode_str, "momentary") == 0) return SWITCH_IN_MODE_MOMENTARY;
  if (strcmp(mode_str, "follow") == 0) return SWITCH_IN_MODE_FOLLOW;
  if (strcmp(mode_str, "flip") == 0) return SWITCH_IN_MODE_FLIP;
  if (strcmp(mode_str, "detached") == 0) return SWITCH_IN_MODE_DETACHED;
  return SWITCH_IN_MODE_UNKNOWN;
}

static switch_initial_state_t parse_switch_initial_state(
    const char *state_str) {
  if (!state_str) return SWITCH_INITIAL_UNKNOWN;
  if (strcmp(state_str, "on") == 0) return SWITCH_INITIAL_ON;
  if (strcmp(state_str, "off") == 0) return SWITCH_INITIAL_OFF;
  if (strcmp(state_str, "restore_last") == 0)
    return SWITCH_INITIAL_RESTORE_LAST;
  if (strcmp(state_str, "match_input") == 0) return SWITCH_INITIAL_MATCH_INPUT;
  return SWITCH_INITIAL_UNKNOWN;
}

switch_config_t *device_state_get_switch(device_state_t *state, int switch_id) {
  if (!state || switch_id < 0 || switch_id >= MAX_SWITCHES) {
    return NULL;
  }
  return &state->switches.switches[switch_id];
}

int device_state_request_switch_config(device_state_t *state,
                                       request_queue_t *queue,
                                       struct mg_connection *conn,
                                       int switch_id) {
  if (!state || !queue || !conn || switch_id < 0 || switch_id >= MAX_SWITCHES) {
    return -1;
  }

  /* Build params: {"id": switch_id} */
  char params[64];
  snprintf(params, sizeof(params), "{\"id\":%d}", switch_id);

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build JSON-RPC request */
  char *request = jsonrpc_build_request("Switch.GetConfig", req_id, params);
  if (!request) {
    fprintf(stderr, "Error: Failed to build Switch.GetConfig request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Switch.GetConfig to request queue\n");
    free(request);
    return -1;
  }

  printf("Requesting switch %d configuration (ID: %d)...\n", switch_id, req_id);

  free(request);
  return req_id;
}

int device_state_update_switch_config(device_state_t *state, const char *json,
                                      int switch_id) {
  if (!state || !json || switch_id < 0 || switch_id >= MAX_SWITCHES) {
    return -1;
  }

  /* Parse JSON response to extract result */
  struct mg_str json_str = mg_str(json);

  /* Check if this is an error response (e.g., switch doesn't exist) */
  char error_msg[256];
  if (jsonrpc_is_error(json, error_msg, sizeof(error_msg))) {
    /* Silently ignore - switch probably doesn't exist on this device */
    return -1;
  }

  /* Extract the result object */
  int result_len = 0;
  int result_pos = mg_json_get(json_str, "$.result", &result_len);

  if (result_pos < 0 || result_len <= 0) {
    fprintf(stderr, "Error: No result field in switch %d config response\n",
            switch_id);
    return -1;
  }

  /* Allocate and copy the result portion */
  char *result_str = malloc(result_len + 1);
  if (!result_str) {
    return -1;
  }
  memcpy(result_str, json + result_pos, result_len);
  result_str[result_len] = '\0';

  pthread_mutex_lock(&state->mutex);

  switch_config_t *sw = &state->switches.switches[switch_id];

  /* Free old data */
  if (sw->raw_json) {
    free(sw->raw_json);
  }

  /* Store only the result portion as raw JSON */
  sw->raw_json = result_str;
  sw->json_len = result_len;
  sw->id = switch_id;

  struct mg_str result = mg_str(result_str);

  /* Parse fields */
  char *name = mg_json_get_str(result, "$.name");
  if (name) {
    snprintf(sw->parsed.name, MAX_SWITCH_NAME, "%s", name);
    free(name);
  } else {
    sw->parsed.name[0] = '\0';
  }

  char *in_mode_str = mg_json_get_str(result, "$.in_mode");
  sw->parsed.in_mode = parse_switch_in_mode(in_mode_str);
  if (in_mode_str) free(in_mode_str);

  double in_locked_val = 0;
  if (mg_json_get_num(result, "$.in_locked", &in_locked_val)) {
    sw->parsed.in_locked = (in_locked_val != 0);
  }

  char *initial_state_str = mg_json_get_str(result, "$.initial_state");
  sw->parsed.initial_state = parse_switch_initial_state(initial_state_str);
  if (initial_state_str) free(initial_state_str);

  double auto_on_val = 0;
  if (mg_json_get_num(result, "$.auto_on", &auto_on_val)) {
    sw->parsed.auto_on = (auto_on_val != 0);
  }

  mg_json_get_num(result, "$.auto_on_delay", &sw->parsed.auto_on_delay);

  double auto_off_val = 0;
  if (mg_json_get_num(result, "$.auto_off", &auto_off_val)) {
    sw->parsed.auto_off = (auto_off_val != 0);
  }

  mg_json_get_num(result, "$.auto_off_delay", &sw->parsed.auto_off_delay);

  double power_limit_val = 0;
  if (mg_json_get_num(result, "$.power_limit", &power_limit_val)) {
    sw->parsed.power_limit = (int) power_limit_val;
  }

  double voltage_limit_val = 0;
  if (mg_json_get_num(result, "$.voltage_limit", &voltage_limit_val)) {
    sw->parsed.voltage_limit = (int) voltage_limit_val;
  }

  double autorecover_val = 0;
  if (mg_json_get_num(result, "$.autorecover_voltage_errors",
                      &autorecover_val)) {
    sw->parsed.autorecover_voltage_errors = (autorecover_val != 0);
  }

  mg_json_get_num(result, "$.current_limit", &sw->parsed.current_limit);

  sw->valid = 1;
  sw->last_update = time(NULL);

  /* Update count if needed */
  if (switch_id >= state->switches.count) {
    state->switches.count = switch_id + 1;
  }

  printf("Switch %d configuration updated successfully\n", switch_id);

  pthread_mutex_unlock(&state->mutex);
  return 0;
}

int device_state_get_switch_config_str(device_state_t *state, int switch_id,
                                       char **output) {
  if (!state || !output || switch_id < 0 || switch_id >= MAX_SWITCHES) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  switch_config_t *sw = &state->switches.switches[switch_id];

  if (!sw->valid || !sw->raw_json) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  *output = strdup(sw->raw_json);

  pthread_mutex_unlock(&state->mutex);

  return (*output != NULL) ? 0 : -1;
}

int device_state_set_switch_config_from_json(const char *user_json,
                                             request_queue_t *queue,
                                             struct mg_connection *conn,
                                             int switch_id) {
  if (!user_json || !queue || !conn || switch_id < 0 ||
      switch_id >= MAX_SWITCHES) {
    return -1;
  }

  /* Validate that user_json is valid JSON */
  struct mg_str json_str = mg_str(user_json);
  int dummy_len = 0;
  int validation = mg_json_get(json_str, "$", &dummy_len);
  if (validation < 0) {
    fprintf(stderr, "Error: Invalid JSON provided by user\n");
    return -1;
  }

  /* Build params: {"id": switch_id, "config": {user_json}} */
  size_t params_size = strlen(user_json) + 64;
  char *params = malloc(params_size);
  if (!params) {
    fprintf(stderr, "Error: Failed to allocate params buffer\n");
    return -1;
  }
  snprintf(params, params_size, "{\"id\":%d,\"config\":%s}", switch_id,
           user_json);

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    free(params);
    return -1;
  }

  /* Build JSON-RPC request */
  char *request = jsonrpc_build_request("Switch.SetConfig", req_id, params);
  free(params);

  if (!request) {
    fprintf(stderr, "Error: Failed to build Switch.SetConfig request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Switch.SetConfig to request queue\n");
    free(request);
    return -1;
  }

  printf("Setting switch %d configuration from user edit (ID: %d)...\n",
         switch_id, req_id);

  /* Request is queued and will be sent by ws_thread_func */
  free(request);
  return req_id;
}

int device_state_is_switch_config_notification(const char *json, size_t len,
                                               int switch_id) {
  (void) switch_id; /* Currently we check for any switch notification */
  return device_state_is_component_notification(json, len, "switch");
}

/* ============================================================================
 * SWITCH CONTROL (Switch.Set / Switch.GetStatus)
 * ============================================================================
 */

int device_state_set_switch(device_state_t *state, request_queue_t *queue,
                            struct mg_connection *conn, int switch_id,
                            bool on) {
  if (!state || !queue || !conn || switch_id < 0 || switch_id >= MAX_SWITCHES) {
    return -1;
  }

  /* Build params: {"id": switch_id, "on": true/false} */
  char params[128];
  snprintf(params, sizeof(params), "{\"id\":%d,\"on\":%s}", switch_id,
           on ? "true" : "false");

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build JSON-RPC request */
  char *request = jsonrpc_build_request("Switch.Set", req_id, params);
  if (!request) {
    fprintf(stderr, "Error: Failed to build Switch.Set request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Switch.Set to request queue\n");
    free(request);
    return -1;
  }

  printf("Setting switch %d to %s (ID: %d)...\n", switch_id, on ? "ON" : "OFF",
         req_id);

  free(request);
  return req_id;
}

int device_state_request_switch_status(device_state_t *state,
                                       request_queue_t *queue,
                                       struct mg_connection *conn,
                                       int switch_id) {
  if (!state || !queue || !conn || switch_id < 0 || switch_id >= MAX_SWITCHES) {
    return -1;
  }

  /* Build params: {"id": switch_id} */
  char params[64];
  snprintf(params, sizeof(params), "{\"id\":%d}", switch_id);

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build JSON-RPC request */
  char *request = jsonrpc_build_request("Switch.GetStatus", req_id, params);
  if (!request) {
    fprintf(stderr, "Error: Failed to build Switch.GetStatus request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Switch.GetStatus to request queue\n");
    free(request);
    return -1;
  }

  printf("Requesting switch %d status (ID: %d)...\n", switch_id, req_id);

  free(request);
  return req_id;
}

int device_state_update_switch_status(device_state_t *state, const char *json,
                                      int switch_id) {
  if (!state || !json || switch_id < 0 || switch_id >= MAX_SWITCHES) {
    return -1;
  }

  /* Parse JSON response to extract result */
  struct mg_str json_str = mg_str(json);

  /* Check if this is an error response */
  char error_msg[256];
  if (jsonrpc_is_error(json, error_msg, sizeof(error_msg))) {
    fprintf(stderr, "Error getting switch %d status: %s\n", switch_id,
            error_msg);
    return -1;
  }

  /* Extract the result object */
  int result_len = 0;
  int result_pos = mg_json_get(json_str, "$.result", &result_len);

  if (result_pos < 0 || result_len <= 0) {
    fprintf(stderr, "Error: No result field in switch %d status response\n",
            switch_id);
    return -1;
  }

  /* Create substring for result */
  struct mg_str result_str = mg_str_n(json + result_pos, result_len);

  pthread_mutex_lock(&state->mutex);

  switch_config_t *sw = device_state_get_switch(state, switch_id);
  if (!sw || !sw->valid) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  time_t now = time(NULL);

  /* Parse id */
  double id_val = 0.0;
  if (mg_json_get_num(result_str, "$.id", &id_val)) {
    int new_id = (int) id_val;
    if (sw->status.id != new_id) {
      sw->status.id = new_id;
      sw->status.mtime_id = now;
    }
  }

  /* Parse source */
  char *source_str = mg_json_get_str(result_str, "$.source");
  if (source_str) {
    if (strcmp(sw->status.source, source_str) != 0) {
      strncpy(sw->status.source, source_str, sizeof(sw->status.source) - 1);
      sw->status.source[sizeof(sw->status.source) - 1] = '\0';
      sw->status.mtime_source = now;
    }
    free(source_str);
  }

  /* Parse output state */
  bool output_val = false;
  if (mg_json_get_bool(result_str, "$.output", &output_val)) {
    if (sw->status.output != output_val) {
      sw->status.output = output_val;
      sw->status.mtime_output = now;
    }
  }

  /* Parse power */
  double apower_val = 0.0;
  if (mg_json_get_num(result_str, "$.apower", &apower_val)) {
    if (sw->status.apower != apower_val) {
      sw->status.apower = apower_val;
      sw->status.mtime_apower = now;
    }
  }

  /* Parse voltage */
  double voltage_val = 0.0;
  if (mg_json_get_num(result_str, "$.voltage", &voltage_val)) {
    if (sw->status.voltage != voltage_val) {
      sw->status.voltage = voltage_val;
      sw->status.mtime_voltage = now;
    }
  }

  /* Parse current */
  double current_val = 0.0;
  if (mg_json_get_num(result_str, "$.current", &current_val)) {
    if (sw->status.current != current_val) {
      sw->status.current = current_val;
      sw->status.mtime_current = now;
    }
  }

  /* Parse frequency (optional) */
  double freq_val = 0.0;
  if (mg_json_get_num(result_str, "$.freq", &freq_val)) {
    if (sw->status.freq != freq_val) {
      sw->status.freq = freq_val;
      sw->status.mtime_freq = now;
    }
  }

  /* Parse energy total (aenergy.total) - access nested field directly */
  double energy_total = 0.0;
  if (mg_json_get_num(result_str, "$.aenergy.total", &energy_total)) {
    if (sw->status.energy_total != energy_total) {
      sw->status.energy_total = energy_total;
      sw->status.mtime_energy = now;
    }
  }

  /* Parse returned energy total (ret_aenergy.total, optional) - access nested
   * field directly */
  double ret_energy_total = 0.0;
  if (mg_json_get_num(result_str, "$.ret_aenergy.total", &ret_energy_total)) {
    if (sw->status.ret_energy_total != ret_energy_total) {
      sw->status.ret_energy_total = ret_energy_total;
      sw->status.mtime_ret_energy = now;
    }
  }

  /* Parse temperature - access nested fields directly */
  double temp_c = 0.0;
  double temp_f = 0.0;
  if (mg_json_get_num(result_str, "$.temperature.tC", &temp_c)) {
    if (sw->status.temperature_c != temp_c) {
      sw->status.temperature_c = temp_c;
      sw->status.mtime_temperature = now;
    }
  }
  if (mg_json_get_num(result_str, "$.temperature.tF", &temp_f)) {
    sw->status.temperature_f = temp_f;
  }

  /* Parse overtemperature flag - could be in errors array */
  /* For now, assume no overtemperature */
  sw->status.overtemperature = false;

  sw->status.last_status_update = now;

  pthread_mutex_unlock(&state->mutex);

  printf(
      "Switch %d status updated: output=%s, power=%.1fW, voltage=%.1fV, "
      "current=%.2fA, temp=%.1fC, energy=%.3fWh\n",
      switch_id, sw->status.output ? "ON" : "OFF", sw->status.apower,
      sw->status.voltage, sw->status.current, sw->status.temperature_c,
      sw->status.energy_total);

  return 0;
}

int device_state_is_switch_status_notification(const char *json, size_t len) {
  if (!json || len == 0) {
    return 0;
  }

  struct mg_str json_str = mg_str_n(json, len);

  /* Check for NotifyStatus or NotifyEvent with switch component */
  char *method = mg_json_get_str(json_str, "$.method");
  if (!method) {
    return 0;
  }

  int is_switch_notif = 0;

  if (strcmp(method, "NotifyStatus") == 0) {
    /* Check if params contain switch status - look in the raw JSON */
    if (strstr(json, "\"switch:") != NULL) {
      is_switch_notif = 1;
    }
  } else if (strcmp(method, "NotifyEvent") == 0) {
    /* Check for switch component events */
    is_switch_notif =
        device_state_is_component_notification(json, len, "switch");
  }

  free(method);
  return is_switch_notif;
}

int device_state_update_switch_status_from_notification(device_state_t *state,
                                                        const char *json) {
  if (!state || !json) {
    return -1;
  }

  struct mg_str json_str = mg_str(json);

  /* Get the params object */
  int params_len = 0;
  int params_pos = mg_json_get(json_str, "$.params", &params_len);
  if (params_pos < 0 || params_len <= 0) {
    return -1;
  }

  struct mg_str params_str = mg_str_n(json + params_pos, params_len);

  /* Iterate through all possible switches to find which ones are in the
   * notification */
  int updated_count = 0;
  for (int switch_id = 0; switch_id < MAX_SWITCHES; switch_id++) {
    /* Build the key name: switch:N */
    char key[32];
    snprintf(key, sizeof(key), "$.switch:%d", switch_id);

    /* Check if this switch is in the params */
    int switch_data_len = 0;
    int switch_data_pos = mg_json_get(params_str, key, &switch_data_len);

    if (switch_data_pos >= 0 && switch_data_len > 0) {
      pthread_mutex_lock(&state->mutex);

      switch_config_t *sw = device_state_get_switch(state, switch_id);
      if (!sw || !sw->valid) {
        pthread_mutex_unlock(&state->mutex);
        continue;
      }

      /* Create substring for this switch's status */
      struct mg_str switch_str =
          mg_str_n(params_str.buf + switch_data_pos, switch_data_len);

      time_t now = time(NULL);

      /* Parse all status fields - same as device_state_update_switch_status but
       * from switch_str */

      /* Parse id */
      double id_val = 0.0;
      if (mg_json_get_num(switch_str, "$.id", &id_val)) {
        int new_id = (int) id_val;
        if (sw->status.id != new_id) {
          sw->status.id = new_id;
          sw->status.mtime_id = now;
        }
      }

      /* Parse source */
      char *source_str = mg_json_get_str(switch_str, "$.source");
      if (source_str) {
        if (strcmp(sw->status.source, source_str) != 0) {
          strncpy(sw->status.source, source_str, sizeof(sw->status.source) - 1);
          sw->status.source[sizeof(sw->status.source) - 1] = '\0';
          sw->status.mtime_source = now;
        }
        free(source_str);
      }

      /* Parse output state */
      bool output_val = false;
      if (mg_json_get_bool(switch_str, "$.output", &output_val)) {
        if (sw->status.output != output_val) {
          sw->status.output = output_val;
          sw->status.mtime_output = now;
        }
      }

      /* Parse power */
      double apower_val = 0.0;
      if (mg_json_get_num(switch_str, "$.apower", &apower_val)) {
        if (sw->status.apower != apower_val) {
          sw->status.apower = apower_val;
          sw->status.mtime_apower = now;
        }
      }

      /* Parse voltage */
      double voltage_val = 0.0;
      if (mg_json_get_num(switch_str, "$.voltage", &voltage_val)) {
        if (sw->status.voltage != voltage_val) {
          sw->status.voltage = voltage_val;
          sw->status.mtime_voltage = now;
        }
      }

      /* Parse current */
      double current_val = 0.0;
      if (mg_json_get_num(switch_str, "$.current", &current_val)) {
        if (sw->status.current != current_val) {
          sw->status.current = current_val;
          sw->status.mtime_current = now;
        }
      }

      /* Parse frequency (optional) */
      double freq_val = 0.0;
      if (mg_json_get_num(switch_str, "$.freq", &freq_val)) {
        if (sw->status.freq != freq_val) {
          sw->status.freq = freq_val;
          sw->status.mtime_freq = now;
        }
      }

      /* Parse energy total (aenergy.total) */
      double energy_total = 0.0;
      if (mg_json_get_num(switch_str, "$.aenergy.total", &energy_total)) {
        if (sw->status.energy_total != energy_total) {
          sw->status.energy_total = energy_total;
          sw->status.mtime_energy = now;
        }
      }

      /* Parse returned energy total (ret_aenergy.total, optional) */
      double ret_energy_total = 0.0;
      if (mg_json_get_num(switch_str, "$.ret_aenergy.total",
                          &ret_energy_total)) {
        if (sw->status.ret_energy_total != ret_energy_total) {
          sw->status.ret_energy_total = ret_energy_total;
          sw->status.mtime_ret_energy = now;
        }
      }

      /* Parse temperature */
      double temp_c = 0.0;
      double temp_f = 0.0;
      if (mg_json_get_num(switch_str, "$.temperature.tC", &temp_c)) {
        if (sw->status.temperature_c != temp_c) {
          sw->status.temperature_c = temp_c;
          sw->status.mtime_temperature = now;
        }
      }
      if (mg_json_get_num(switch_str, "$.temperature.tF", &temp_f)) {
        sw->status.temperature_f = temp_f;
      }

      /* Parse overtemperature flag */
      sw->status.overtemperature = false;

      sw->status.last_status_update = now;

      printf(
          "Switch %d status updated from notification: output=%s, power=%.1fW, "
          "voltage=%.1fV, current=%.2fA, temp=%.1fC, energy=%.3fWh\n",
          switch_id, sw->status.output ? "ON" : "OFF", sw->status.apower,
          sw->status.voltage, sw->status.current, sw->status.temperature_c,
          sw->status.energy_total);

      pthread_mutex_unlock(&state->mutex);
      updated_count++;
    }
  }

  return updated_count > 0 ? 0 : -1;
}

/* ============================================================================
 * INPUT CONFIGURATION (Input.GetConfig / Input.SetConfig)
 * ============================================================================
 */

input_config_t *device_state_get_input(device_state_t *state, int input_id) {
  if (!state || input_id < 0 || input_id >= MAX_INPUTS) {
    return NULL;
  }
  return &state->inputs.inputs[input_id];
}

int device_state_request_input_config(device_state_t *state,
                                      request_queue_t *queue,
                                      struct mg_connection *conn,
                                      int input_id) {
  if (!state || !queue || !conn || input_id < 0 || input_id >= MAX_INPUTS) {
    return -1;
  }

  /* Build params: {"id": input_id} */
  char params[64];
  snprintf(params, sizeof(params), "{\"id\":%d}", input_id);

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build JSON-RPC request */
  char *request = jsonrpc_build_request("Input.GetConfig", req_id, params);
  if (!request) {
    fprintf(stderr, "Error: Failed to build Input.GetConfig request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Input.GetConfig to request queue\n");
    free(request);
    return -1;
  }

  printf("Requesting input %d configuration (ID: %d)...\n", input_id, req_id);

  free(request);
  return req_id;
}

int device_state_update_input_config(device_state_t *state, const char *json,
                                     int input_id) {
  if (!state || !json || input_id < 0 || input_id >= MAX_INPUTS) {
    return -1;
  }

  /* Parse JSON response to extract result */
  struct mg_str json_str = mg_str(json);

  /* Check if this is an error response (e.g., input doesn't exist) */
  char error_msg[256];
  if (jsonrpc_is_error(json, error_msg, sizeof(error_msg))) {
    /* Silently ignore - input probably doesn't exist on this device */
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  input_config_t *inp = device_state_get_input(state, input_id);
  if (!inp) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  /* Get result object */
  int result_len = 0;
  int result_pos = mg_json_get(json_str, "$.result", &result_len);
  if (result_pos < 0 || result_len <= 0) {
    fprintf(stderr, "Error: No result field in input %d config response\n",
            input_id);
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  /* Extract the result substring for raw JSON storage */
  char *result_json = strndup(json + result_pos, result_len);
  if (!result_json) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  /* Free old raw JSON if exists */
  if (inp->raw_json) {
    free(inp->raw_json);
  }

  inp->raw_json = result_json;
  inp->json_len = result_len;

  /* Parse fields */
  struct mg_str result_str = mg_str_n(json + result_pos, result_len);

  /* Parse id */
  double id_val = 0.0;
  if (mg_json_get_num(result_str, "$.id", &id_val)) {
    inp->id = (int) id_val;
  }

  /* Parse name */
  char *name_str = mg_json_get_str(result_str, "$.name");
  if (name_str) {
    strncpy(inp->parsed.name, name_str, sizeof(inp->parsed.name) - 1);
    inp->parsed.name[sizeof(inp->parsed.name) - 1] = '\0';
    free(name_str);
  } else {
    inp->parsed.name[0] = '\0';
  }

  /* Parse type */
  char *type_str = mg_json_get_str(result_str, "$.type");
  if (type_str) {
    if (strcmp(type_str, "switch") == 0) {
      inp->parsed.type = INPUT_TYPE_SWITCH;
    } else if (strcmp(type_str, "button") == 0) {
      inp->parsed.type = INPUT_TYPE_BUTTON;
    } else if (strcmp(type_str, "analog") == 0) {
      inp->parsed.type = INPUT_TYPE_ANALOG;
    } else {
      inp->parsed.type = INPUT_TYPE_UNKNOWN;
    }
    free(type_str);
  }

  /* Parse enable */
  mg_json_get_bool(result_str, "$.enable", &inp->parsed.enable);

  /* Parse invert */
  mg_json_get_bool(result_str, "$.invert", &inp->parsed.invert);

  /* Parse factory_reset */
  mg_json_get_bool(result_str, "$.factory_reset", &inp->parsed.factory_reset);

  inp->valid = 1;
  inp->last_update = time(NULL);

  pthread_mutex_unlock(&state->mutex);

  printf("Input %d config updated: name=\"%s\", type=%d, enable=%s\n", input_id,
         inp->parsed.name, inp->parsed.type,
         inp->parsed.enable ? "true" : "false");

  return 0;
}

int device_state_get_input_config_str(device_state_t *state, int input_id,
                                      char **output) {
  if (!state || !output) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  input_config_t *inp = device_state_get_input(state, input_id);
  if (!inp || !inp->valid || !inp->raw_json) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  *output = strdup(inp->raw_json);

  pthread_mutex_unlock(&state->mutex);

  return (*output != NULL) ? 0 : -1;
}

int device_state_set_input_config_from_json(const char *user_json,
                                            request_queue_t *queue,
                                            struct mg_connection *conn,
                                            int input_id) {
  if (!user_json || !queue || !conn || input_id < 0 || input_id >= MAX_INPUTS) {
    return -1;
  }

  /* Validate that user_json is valid JSON */
  struct mg_str json_str = mg_str(user_json);
  int dummy_len = 0;
  int validation = mg_json_get(json_str, "$", &dummy_len);
  if (validation < 0) {
    fprintf(stderr, "Error: Invalid JSON provided by user\n");
    return -1;
  }

  /* Build params: {"id": input_id, "config": {user_json}} */
  size_t params_size = strlen(user_json) + 64;
  char *params = malloc(params_size);
  if (!params) {
    fprintf(stderr, "Error: Failed to allocate params buffer\n");
    return -1;
  }
  snprintf(params, params_size, "{\"id\":%d,\"config\":%s}", input_id,
           user_json);

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    free(params);
    return -1;
  }

  /* Build JSON-RPC request */
  char *request = jsonrpc_build_request("Input.SetConfig", req_id, params);
  free(params);

  if (!request) {
    fprintf(stderr, "Error: Failed to build Input.SetConfig request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Input.SetConfig to request queue\n");
    free(request);
    return -1;
  }

  printf("Setting input %d configuration (ID: %d)...\n", input_id, req_id);

  free(request);
  return req_id;
}

int device_state_is_input_config_notification(const char *json, size_t len,
                                              int input_id) {
  if (!json || len == 0) {
    return 0;
  }

  struct mg_str json_str = mg_str_n(json, len);

  char *method = mg_json_get_str(json_str, "$.method");
  int is_input_config_notif = 0;

  if (method && strcmp(method, "NotifyEvent") == 0) {
    char key[64];
    snprintf(key, sizeof(key), "$.params.events[?(@.component=='input:%d')]",
             input_id);

    int event_len = 0;
    if (mg_json_get(json_str, key, &event_len) >= 0) {
      is_input_config_notif = 1;
    }
  }

  if (method) {
    free(method);
  }

  return is_input_config_notif;
}

/* ============================================================================
 * INPUT STATUS (Input.GetStatus)
 * ============================================================================
 */

int device_state_request_input_status(device_state_t *state,
                                      request_queue_t *queue,
                                      struct mg_connection *conn,
                                      int input_id) {
  if (!state || !queue || !conn || input_id < 0 || input_id >= MAX_INPUTS) {
    return -1;
  }

  /* Build params: {"id": input_id} */
  char params[64];
  snprintf(params, sizeof(params), "{\"id\":%d}", input_id);

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build JSON-RPC request */
  char *request = jsonrpc_build_request("Input.GetStatus", req_id, params);
  if (!request) {
    fprintf(stderr, "Error: Failed to build Input.GetStatus request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Input.GetStatus to request queue\n");
    free(request);
    return -1;
  }

  printf("Requesting input %d status (ID: %d)...\n", input_id, req_id);

  free(request);
  return req_id;
}

int device_state_update_input_status(device_state_t *state, const char *json,
                                     int input_id) {
  if (!state || !json || input_id < 0 || input_id >= MAX_INPUTS) {
    return -1;
  }

  /* Check if this is an error response (e.g., input doesn't exist) */
  char error_msg[256];
  if (jsonrpc_is_error(json, error_msg, sizeof(error_msg))) {
    /* Silently ignore - input probably doesn't exist on this device */
    return -1;
  }

  struct mg_str json_str = mg_str(json);

  pthread_mutex_lock(&state->mutex);

  input_config_t *inp = device_state_get_input(state, input_id);
  if (!inp || !inp->valid) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  /* Get result object */
  int result_len = 0;
  int result_pos = mg_json_get(json_str, "$.result", &result_len);
  if (result_pos < 0 || result_len <= 0) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  struct mg_str result_str = mg_str_n(json + result_pos, result_len);

  time_t now = time(NULL);

  /* Parse id */
  double id_val = 0.0;
  if (mg_json_get_num(result_str, "$.id", &id_val)) {
    int new_id = (int) id_val;
    if (inp->status.id != new_id) {
      inp->status.id = new_id;
      inp->status.mtime_id = now;
    }
  }

  /* Parse state */
  bool state_val = false;
  if (mg_json_get_bool(result_str, "$.state", &state_val)) {
    if (inp->status.state != state_val) {
      inp->status.state = state_val;
      inp->status.mtime_state = now;
    }
  }

  inp->status.last_status_update = now;

  pthread_mutex_unlock(&state->mutex);

  printf("Input %d status updated: state=%s\n", input_id,
         inp->status.state ? "true" : "false");

  return 0;
}

int device_state_is_input_status_notification(const char *json, size_t len) {
  if (!json || len == 0) {
    return 0;
  }

  struct mg_str json_str = mg_str_n(json, len);
  char *method = mg_json_get_str(json_str, "$.method");

  int is_input_notif = 0;

  if (method && strcmp(method, "NotifyStatus") == 0) {
    /* Check if params contain input status - look in the raw JSON */
    if (strstr(json, "\"input:") != NULL) {
      is_input_notif = 1;
    }
  }

  if (method) {
    free(method);
  }

  return is_input_notif;
}

int device_state_update_input_status_from_notification(device_state_t *state,
                                                       const char *json) {
  if (!state || !json) {
    return -1;
  }

  struct mg_str json_str = mg_str(json);

  /* Get the params object */
  int params_len = 0;
  int params_pos = mg_json_get(json_str, "$.params", &params_len);
  if (params_pos < 0 || params_len <= 0) {
    return -1;
  }

  struct mg_str params_str = mg_str_n(json + params_pos, params_len);

  /* Iterate through all possible inputs to find which ones are in the
   * notification */
  int updated_count = 0;
  for (int input_id = 0; input_id < MAX_INPUTS; input_id++) {
    /* Build the key name: input:N */
    char key[32];
    snprintf(key, sizeof(key), "$.input:%d", input_id);

    /* Check if this input is in the params */
    int input_data_len = 0;
    int input_data_pos = mg_json_get(params_str, key, &input_data_len);

    if (input_data_pos >= 0 && input_data_len > 0) {
      pthread_mutex_lock(&state->mutex);

      input_config_t *inp = device_state_get_input(state, input_id);
      if (!inp || !inp->valid) {
        pthread_mutex_unlock(&state->mutex);
        continue;
      }

      /* Create substring for this input's status */
      struct mg_str input_str =
          mg_str_n(params_str.buf + input_data_pos, input_data_len);

      time_t now = time(NULL);

      /* Parse id */
      double id_val = 0.0;
      if (mg_json_get_num(input_str, "$.id", &id_val)) {
        int new_id = (int) id_val;
        if (inp->status.id != new_id) {
          inp->status.id = new_id;
          inp->status.mtime_id = now;
        }
      }

      /* Parse state */
      bool state_val = false;
      if (mg_json_get_bool(input_str, "$.state", &state_val)) {
        if (inp->status.state != state_val) {
          inp->status.state = state_val;
          inp->status.mtime_state = now;
        }
      }

      inp->status.last_status_update = now;

      printf("Input %d status updated from notification: state=%s\n", input_id,
             inp->status.state ? "true" : "false");

      pthread_mutex_unlock(&state->mutex);
      updated_count++;
    }
  }

  return updated_count > 0 ? 0 : -1;
}

/* ============================================================================
 * SCRIPT LISTING (Script.List)
 * ============================================================================
 */

int device_state_request_script_list(device_state_t *state,
                                     request_queue_t *queue,
                                     struct mg_connection *conn) {
  if (!state || !queue || !conn) {
    return -1;
  }

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build JSON-RPC request with correct ID */
  char *request = jsonrpc_build_request("Script.List", req_id, NULL);
  if (!request) {
    fprintf(stderr, "Error: Failed to build Script.List request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Script.List to request queue\n");
    free(request);
    return -1;
  }

  printf("Requesting script list (ID: %d)...\n", req_id);

  /* Request is queued and will be sent by ws_thread_func */
  free(request);
  return req_id;
}

int device_state_update_script_list(device_state_t *state, const char *json) {
  if (!state || !json) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  /* Parse the response */
  struct mg_str json_str = mg_str(json);

  /* Find the result object position and length */
  int result_len = 0;
  int result_pos = mg_json_get(json_str, "$.result", &result_len);

  if (result_pos < 0) {
    pthread_mutex_unlock(&state->mutex);
    fprintf(stderr, "Error: No result in Script.List response\n");
    return -1;
  }

  /* Create substring view of result object */
  struct mg_str result = mg_str_n(json + result_pos, result_len);

  /* Find the scripts array position and length within result */
  int scripts_len = 0;
  int scripts_pos = mg_json_get(result, "$.scripts", &scripts_len);

  if (scripts_pos < 0) {
    pthread_mutex_unlock(&state->mutex);
    fprintf(stderr, "Error: No scripts array in Script.List response\n");
    return -1;
  }

  /* Create substring view of scripts array */
  struct mg_str scripts_array = mg_str_n(result.buf + scripts_pos, scripts_len);
  int count = 0;

  for (int i = 0; i < MAX_SCRIPTS; i++) {
    char path[64];

    /* Get script ID */
    snprintf(path, sizeof(path), "$[%d].id", i);
    double id_val = 0;
    if (!mg_json_get_num(scripts_array, path, &id_val)) {
      break; /* No more scripts */
    }
    int script_id = (int) id_val;

    if (script_id < 0 || script_id >= MAX_SCRIPTS) {
      continue;
    }

    /* Get script name */
    snprintf(path, sizeof(path), "$[%d].name", i);
    char *name = mg_json_get_str(scripts_array, path);
    if (name) {
      snprintf(state->scripts.scripts[script_id].name, MAX_SCRIPT_NAME, "%s",
               name);
      free(name);
    }

    /* Get enable flag */
    snprintf(path, sizeof(path), "$[%d].enable", i);
    double enable_val = 0;
    if (mg_json_get_num(scripts_array, path, &enable_val)) {
      state->scripts.scripts[script_id].enable = (enable_val != 0);
    }

    state->scripts.scripts[script_id].id = script_id;
    state->scripts.scripts[script_id].valid = 1;

    count++;
  }

  state->scripts.count = count;
  state->scripts.last_update = time(NULL);

  printf("Script list updated: %d scripts found\n", count);

  pthread_mutex_unlock(&state->mutex);

  return count;
}

/* ============================================================================
 * SCRIPT CODE MANAGEMENT (Script.GetCode / Script.PutCode)
 * ============================================================================
 */

script_entry_t *device_state_get_script(device_state_t *state, int script_id) {
  if (!state || script_id < 0 || script_id >= MAX_SCRIPTS) {
    return NULL;
  }

  pthread_mutex_lock(&state->mutex);

  script_entry_t *result = NULL;
  for (int i = 0; i < MAX_SCRIPTS; i++) {
    if (state->scripts.scripts[i].id == script_id &&
        state->scripts.scripts[i].valid) {
      result = &state->scripts.scripts[i];
      break;
    }
  }

  pthread_mutex_unlock(&state->mutex);
  return result;
}

int device_state_request_script_code(device_state_t *state,
                                     request_queue_t *queue,
                                     struct mg_connection *conn,
                                     int script_id) {
  if (!state || !queue || !conn || script_id < 0 || script_id >= MAX_SCRIPTS) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  /* If starting a new retrieval, initialize chunk state */
  if (state->scripts.retrieving_id != script_id) {
    /* Clean up any previous chunk buffer */
    if (state->scripts.chunk_buffer) {
      free(state->scripts.chunk_buffer);
      state->scripts.chunk_buffer = NULL;
    }

    /* Allocate maximum script size buffer */
    state->scripts.chunk_buffer = malloc(MAX_SCRIPT_CODE);
    if (!state->scripts.chunk_buffer) {
      pthread_mutex_unlock(&state->mutex);
      fprintf(stderr, "Error: Failed to allocate script buffer\n");
      return -1;
    }

    state->scripts.chunk_buffer[0] = '\0';
    state->scripts.chunk_buffer_size = 0;
    state->scripts.current_offset = 0;
    state->scripts.retrieving_id = script_id;
  }

  int offset = state->scripts.current_offset;

  pthread_mutex_unlock(&state->mutex);

  /* Build params: {"id": script_id, "offset": offset} */
  char params[128];
  snprintf(params, sizeof(params), "{\"id\":%d,\"offset\":%d}", script_id,
           offset);

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build JSON-RPC request with correct ID */
  char *request = jsonrpc_build_request("Script.GetCode", req_id, params);
  if (!request) {
    fprintf(stderr, "Error: Failed to build Script.GetCode request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Script.GetCode to request queue\n");
    free(request);
    return -1;
  }

  printf("Requesting script %d code at offset %d (ID: %d)...\n", script_id,
         offset, req_id);

  /* Request is queued and will be sent by ws_thread_func */
  free(request);
  return req_id;
}

int device_state_update_script_code(device_state_t *state, const char *json,
                                    int script_id) {
  if (!state || !json || script_id < 0 || script_id >= MAX_SCRIPTS) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  /* Parse the response */
  struct mg_str json_str = mg_str(json);

  /* Find the result object position and length */
  int result_len = 0;
  int result_pos = mg_json_get(json_str, "$.result", &result_len);

  if (result_pos < 0) {
    pthread_mutex_unlock(&state->mutex);
    fprintf(stderr, "Error: No result in Script.GetCode response\n");
    return -1;
  }

  /* Create substring view of result object */
  struct mg_str result = mg_str_n(json + result_pos, result_len);

  /* Extract code chunk (still needs allocation for JSON unescaping) */
  char *code_str = mg_json_get_str(result, "$.data");

  /* Extract left bytes */
  double left_val = 0;
  int has_left = mg_json_get_num(result, "$.left", &left_val);

  if (!code_str) {
    pthread_mutex_unlock(&state->mutex);
    fprintf(stderr, "Error: No code data in Script.GetCode response\n");
    return -1;
  }

  size_t code_len = strlen(code_str);

  /* Check buffer overflow */
  if (state->scripts.chunk_buffer_size + code_len >= MAX_SCRIPT_CODE) {
    free(code_str);
    pthread_mutex_unlock(&state->mutex);
    fprintf(stderr, "Error: Script code exceeds maximum size\n");
    return -1;
  }

  /* Append chunk to buffer */
  memcpy(state->scripts.chunk_buffer + state->scripts.chunk_buffer_size,
         code_str, code_len);
  state->scripts.chunk_buffer_size += code_len;

  /* Move null terminator */
  state->scripts.chunk_buffer[state->scripts.chunk_buffer_size] = '\0';

  state->scripts.current_offset += code_len;

  int left = has_left ? (int) left_val : 0;

  printf("Received script %d chunk: %zu bytes, %d bytes left\n", script_id,
         code_len, left);

  free(code_str);
  pthread_mutex_unlock(&state->mutex);

  /* Return positive value if more chunks needed, 0 if complete, -1 on error */
  return left;
}

int device_state_finalize_script_code(device_state_t *state, int script_id) {
  if (!state || script_id < 0 || script_id >= MAX_SCRIPTS) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  if (state->scripts.retrieving_id != script_id ||
      !state->scripts.chunk_buffer) {
    pthread_mutex_unlock(&state->mutex);
    fprintf(stderr, "Error: No script retrieval in progress for script %d\n",
            script_id);
    return -1;
  }

  /* Free old code if any */
  if (state->scripts.scripts[script_id].code) {
    free(state->scripts.scripts[script_id].code);
  }

  /* Move chunk buffer to script entry */
  state->scripts.scripts[script_id].code = state->scripts.chunk_buffer;
  state->scripts.scripts[script_id].id = script_id;
  state->scripts.scripts[script_id].valid = 1;
  state->scripts.scripts[script_id].modify_time = time(NULL);

  /* Reset chunk state */
  state->scripts.chunk_buffer = NULL;
  state->scripts.chunk_buffer_size = 0;
  state->scripts.current_offset = 0;
  state->scripts.retrieving_id = -1;

  printf("Script %d code retrieval complete (%zu bytes)\n", script_id,
         strlen(state->scripts.scripts[script_id].code));

  pthread_mutex_unlock(&state->mutex);
  return 0;
}

int device_state_get_script_code_str(device_state_t *state, int script_id,
                                     char **output) {
  if (!state || !output || script_id < 0 || script_id >= MAX_SCRIPTS) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  int slot = -1;
  for (int i = 0; i < MAX_SCRIPTS; i++) {
    if (state->scripts.scripts[i].id == script_id &&
        state->scripts.scripts[i].valid) {
      slot = i;
      break;
    }
  }

  if (slot == -1 || !state->scripts.scripts[slot].code) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  *output = strdup(state->scripts.scripts[slot].code);

  pthread_mutex_unlock(&state->mutex);

  return (*output) ? 0 : -1;
}

/* Helper function to escape JSON string */
static char *json_escape_string(const char *str, size_t len) {
  if (!str) {
    return NULL;
  }

  /* Allocate worst case: every character needs escaping */
  char *escaped = malloc(len * 2 + 1);
  if (!escaped) {
    return NULL;
  }

  size_t escaped_idx = 0;
  for (size_t i = 0; i < len; i++) {
    if (str[i] == '"') {
      escaped[escaped_idx++] = '\\';
      escaped[escaped_idx++] = '"';
    } else if (str[i] == '\\') {
      escaped[escaped_idx++] = '\\';
      escaped[escaped_idx++] = '\\';
    } else if (str[i] == '\n') {
      escaped[escaped_idx++] = '\\';
      escaped[escaped_idx++] = 'n';
    } else if (str[i] == '\r') {
      escaped[escaped_idx++] = '\\';
      escaped[escaped_idx++] = 'r';
    } else if (str[i] == '\t') {
      escaped[escaped_idx++] = '\\';
      escaped[escaped_idx++] = 't';
    } else if ((unsigned char) str[i] < 32) {
      /* Skip other control characters */
      continue;
    } else {
      escaped[escaped_idx++] = str[i];
    }
  }
  escaped[escaped_idx] = '\0';

  return escaped;
}

int device_state_put_script_code(device_state_t *state, request_queue_t *queue,
                                 struct mg_connection *conn, int script_id,
                                 const char *code) {
  if (!state || !queue || !conn || script_id < 0 || script_id >= MAX_SCRIPTS ||
      !code) {
    return -1;
  }

  size_t code_len = strlen(code);
  size_t offset = 0;
  int chunk_num = 0;
  int last_req_id = -1;

  printf("Uploading script %d to device (%zu bytes in chunks of %d)\n",
         script_id, code_len, SCRIPT_CHUNK_SIZE);

  while (offset < code_len) {
    size_t chunk_size = (code_len - offset > SCRIPT_CHUNK_SIZE)
                            ? SCRIPT_CHUNK_SIZE
                            : (code_len - offset);

    /* Extract chunk */
    char *chunk = strndup(code + offset, chunk_size);
    if (!chunk) {
      fprintf(stderr, "Error: Failed to allocate chunk buffer\n");
      return -1;
    }

    /* Escape the chunk for JSON */
    char *escaped_chunk = json_escape_string(chunk, chunk_size);
    free(chunk);

    if (!escaped_chunk) {
      fprintf(stderr, "Error: Failed to escape chunk\n");
      return -1;
    }

    /* Build params: {"id": script_id, "code": "...", "append": true/false} */
    bool append = (chunk_num > 0);
    size_t params_size = strlen(escaped_chunk) + 256;
    char *params = malloc(params_size);
    if (!params) {
      free(escaped_chunk);
      return -1;
    }

    snprintf(params, params_size, "{\"id\":%d,\"code\":\"%s\",\"append\":%s}",
             script_id, escaped_chunk, append ? "true" : "false");

    free(escaped_chunk);

    /* Get next request ID */
    int req_id = request_queue_peek_next_id(queue);
    if (req_id < 0) {
      free(params);
      fprintf(stderr, "Error: Failed to get next request ID\n");
      return -1;
    }

    /* Build JSON-RPC request with correct ID */
    char *request = jsonrpc_build_request("Script.PutCode", req_id, params);
    free(params);

    if (!request) {
      fprintf(stderr, "Error: Failed to build Script.PutCode request\n");
      return -1;
    }

    /* Add to request queue */
    int added_id = request_queue_add(queue, request);
    if (added_id < 0) {
      free(request);
      fprintf(stderr, "Error: Failed to add Script.PutCode to request queue\n");
      return -1;
    }

    printf("  Chunk %d: offset=%zu, size=%zu, append=%s (req ID: %d)\n",
           chunk_num, offset, chunk_size, append ? "true" : "false", req_id);

    /* Request is queued and will be sent by ws_thread_func */
    free(request);
    last_req_id = req_id;

    offset += chunk_size;
    chunk_num++;

    /* Small delay between chunks to allow queue to process */
    usleep(50000); /* 50ms */
  }

  printf("Script %d upload complete: %d chunks sent\n", script_id, chunk_num);

  /* Update local script code and track last upload request ID */
  pthread_mutex_lock(&state->mutex);

  /* Find the script by ID */
  int slot = -1;
  for (int i = 0; i < MAX_SCRIPTS; i++) {
    if (state->scripts.scripts[i].id == script_id &&
        state->scripts.scripts[i].valid) {
      slot = i;
      break;
    }
  }

  if (slot >= 0) {
    if (state->scripts.scripts[slot].code) {
      free(state->scripts.scripts[slot].code);
    }
    state->scripts.scripts[slot].code = strdup(code);
    state->scripts.scripts[slot].modify_time = time(NULL);
    state->scripts.scripts[slot].last_upload_req_id = last_req_id;
  }

  pthread_mutex_unlock(&state->mutex);

  return last_req_id;
}

/* ============================================================================
 * SCRIPT STATUS NOTIFICATIONS
 * ============================================================================
 */

int device_state_is_script_status_notification(const char *json, size_t len) {
  if (!json || len == 0) {
    return 0;
  }

  struct mg_str json_str = mg_str_n(json, len);

  /* Check for NotifyStatus notification */
  char *method = mg_json_get_str(json_str, "$.method");

  if (method) {
    if (strstr(method, "NotifyStatus") != NULL) {
      /* Check if params contain any script:N component */
      char *params = mg_json_get_str(json_str, "$.params");
      if (params) {
        struct mg_str params_str = mg_str(params);

        /* Check for script:0 through script:9 */
        for (int i = 0; i < MAX_SCRIPTS; i++) {
          char path[32];
          snprintf(path, sizeof(path), "$.script:%d", i);

          char *script_status = mg_json_get_str(params_str, path);
          if (script_status) {
            free(script_status);
            free(params);
            free(method);
            return 1;
          }
        }
        free(params);
      }
    }
    free(method);
  }

  return 0;
}

int device_state_update_script_status(device_state_t *state, const char *json,
                                      size_t len) {
  if (!state || !json || len == 0) {
    return -1;
  }

  struct mg_str json_str = mg_str_n(json, len);

  /* Extract params from notification */
  char *params = mg_json_get_str(json_str, "$.params");
  if (!params) {
    return -1;
  }

  struct mg_str params_str = mg_str(params);
  int updated_count = 0;

  pthread_mutex_lock(&state->mutex);

  /* Check each script slot for status updates */
  for (int i = 0; i < MAX_SCRIPTS; i++) {
    char path[64];
    snprintf(path, sizeof(path), "$.script:%d", i);

    char *script_status = mg_json_get_str(params_str, path);
    if (!script_status) {
      continue; /* No status for this script */
    }

    struct mg_str status_str = mg_str(script_status);

    /* Extract running flag */
    double running_val = 0;
    if (mg_json_get_num(status_str, "$.running", &running_val)) {
      state->scripts.scripts[i].running = (running_val != 0);
    }

    /* Extract mem_used */
    double mem_used_val = 0;
    if (mg_json_get_num(status_str, "$.mem_used", &mem_used_val)) {
      state->scripts.scripts[i].mem_used = (int) mem_used_val;
    }

    /* Extract mem_peak */
    double mem_peak_val = 0;
    if (mg_json_get_num(status_str, "$.mem_peak", &mem_peak_val)) {
      state->scripts.scripts[i].mem_peak = (int) mem_peak_val;
    }

    /* Extract errors array */
    char *errors_str = mg_json_get_str(status_str, "$.errors");
    if (errors_str) {
      /* Free old errors if any */
      if (state->scripts.scripts[i].errors) {
        free(state->scripts.scripts[i].errors);
        state->scripts.scripts[i].errors = NULL;
      }

      /* Store errors as JSON array string */
      state->scripts.scripts[i].errors = strdup(errors_str);

      /* Log errors if present */
      if (strlen(errors_str) > 2) { /* More than just "[]" */
        printf("Script %d errors: %s\n", i, errors_str);
      }

      free(errors_str);
    }

    state->scripts.scripts[i].last_status_update = time(NULL);

    printf("Script %d status: running=%d, mem_used=%d, mem_peak=%d\n", i,
           state->scripts.scripts[i].running,
           state->scripts.scripts[i].mem_used,
           state->scripts.scripts[i].mem_peak);

    free(script_status);
    updated_count++;
  }

  pthread_mutex_unlock(&state->mutex);
  free(params);

  return updated_count;
}

/* ============================================================================
 * NOTIFICATION UTILITIES
 * ============================================================================
 */

int device_state_is_component_notification(const char *json, size_t len,
                                           const char *component) {
  if (!json || len == 0 || !component) {
    return 0;
  }

  struct mg_str json_str = mg_str_n(json, len);

  /* Check for NotifyEvent notifications */
  char *method = mg_json_get_str(json_str, "$.method");

  if (method) {
    if (strcmp(method, "NotifyEvent") == 0) {
      /* Parse NotifyEvent format:
       * {"method":"NotifyEvent","params":{"events":[{"component":"mqtt","event":"config_changed"}]}}
       */

      /* Get the events array */
      int events_len = 0;
      int events_pos = mg_json_get(json_str, "$.params.events", &events_len);

      if (events_pos >= 0 && events_len > 0) {
        /* Extract events array as string */
        struct mg_str events_str = mg_str_n(json + events_pos, events_len);

        /* Iterate through events array looking for config_changed events */
        /* Format: [{"component":"mqtt","event":"config_changed",...},...]  */
        for (int i = 0;; i++) {
          char path[32];
          snprintf(path, sizeof(path), "$[%d]", i);

          int event_len = 0;
          int event_pos = mg_json_get(events_str, path, &event_len);

          if (event_pos < 0) {
            /* No more events */
            break;
          }

          /* Check this event */
          struct mg_str event_str =
              mg_str_n(json + events_pos + event_pos, event_len);

          char *comp = mg_json_get_str(event_str, "$.component");
          char *event_type = mg_json_get_str(event_str, "$.event");

          if (comp && event_type) {
            if (strcmp(comp, component) == 0 &&
                strcmp(event_type, "config_changed") == 0) {
              free(comp);
              free(event_type);
              free(method);
              return 1;
            }
          }

          if (comp) free(comp);
          if (event_type) free(event_type);
        }
      }
    }
    /* Also check for legacy NotifyStatus format for backwards compatibility */
    else if (strstr(method, "NotifyStatus") != NULL) {
      /* Check if params contain the specified component directly */
      char *params = mg_json_get_str(json_str, "$.params");
      if (params) {
        char path[128];
        snprintf(path, sizeof(path), "$.%s", component);

        struct mg_str params_str = mg_str(params);
        char *comp_field = mg_json_get_str(params_str, path);
        if (comp_field) {
          free(comp_field);
          free(params);
          free(method);
          return 1;
        }
        free(params);
      }
    }
    free(method);
  }

  return 0;
}

/* ============================================================================
 * SCHEDULE MANAGEMENT (Schedule.List / Schedule.Create / Schedule.Update /
 * Schedule.Delete)
 * ============================================================================
 */

schedule_entry_t *device_state_get_schedule(device_state_t *state,
                                            int schedule_id) {
  if (!state || schedule_id < 0) {
    return NULL;
  }

  /* Find schedule with matching ID */
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    if (state->schedules.schedules[i].valid &&
        state->schedules.schedules[i].id == schedule_id) {
      return &state->schedules.schedules[i];
    }
  }

  return NULL;
}

int device_state_request_schedule_list(device_state_t *state,
                                       request_queue_t *queue,
                                       struct mg_connection *conn) {
  if (!state || !queue || !conn) {
    return -1;
  }

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build JSON-RPC request - Schedule.List takes no params */
  char *request = jsonrpc_build_request("Schedule.List", req_id, NULL);
  if (!request) {
    fprintf(stderr, "Error: Failed to build Schedule.List request\n");
    return -1;
  }

  /* Add to request queue */
  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Schedule.List to request queue\n");
    free(request);
    return -1;
  }

  printf("Requesting schedule list (ID: %d)...\n", req_id);

  free(request);
  return req_id;
}

int device_state_update_schedule_list(device_state_t *state, const char *json) {
  if (!state || !json) {
    return -1;
  }

  /* Check if this is an error response */
  char error_msg[256];
  if (jsonrpc_is_error(json, error_msg, sizeof(error_msg))) {
    fprintf(stderr, "Error getting schedule list: %s\n", error_msg);
    return -1;
  }

  struct mg_str json_str = mg_str(json);

  pthread_mutex_lock(&state->mutex);

  /* Clear existing schedules */
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    for (int j = 0; j < MAX_SCHEDULE_CALLS; j++) {
      if (state->schedules.schedules[i].calls[j].params_json) {
        free(state->schedules.schedules[i].calls[j].params_json);
        state->schedules.schedules[i].calls[j].params_json = NULL;
      }
    }
    state->schedules.schedules[i].valid = 0;
  }
  state->schedules.count = 0;

  /* Get revision number */
  double rev_val = 0.0;
  if (mg_json_get_num(json_str, "$.result.rev", &rev_val)) {
    state->schedules.rev = (int) rev_val;
  }

  /* Parse jobs array */
  int jobs_len = 0;
  int jobs_pos = mg_json_get(json_str, "$.result.jobs", &jobs_len);
  if (jobs_pos < 0 || jobs_len <= 0) {
    /* No jobs - this is valid, just means no schedules */
    pthread_mutex_unlock(&state->mutex);
    printf("No schedules found on device\n");
    return 0;
  }

  struct mg_str jobs_str = mg_str_n(json + jobs_pos, jobs_len);

  /* Iterate through jobs array */
  int schedule_count = 0;
  int offset = 0;
  struct mg_str key, val;
  while ((offset = mg_json_next(jobs_str, offset, &key, &val)) > 0) {
    if (schedule_count >= MAX_SCHEDULES) {
      break;
    }

    schedule_entry_t *sched = &state->schedules.schedules[schedule_count];

    /* Parse schedule fields from val */
    double id_val = 0.0;
    if (mg_json_get_num(val, "$.id", &id_val)) {
      sched->id = (int) id_val;
    }

    mg_json_get_bool(val, "$.enable", &sched->enable);

    char *timespec_str = mg_json_get_str(val, "$.timespec");
    if (timespec_str) {
      strncpy(sched->timespec, timespec_str, sizeof(sched->timespec) - 1);
      sched->timespec[sizeof(sched->timespec) - 1] = '\0';
      free(timespec_str);
    }

    /* Parse calls array */
    int calls_len = 0;
    int calls_pos = mg_json_get(val, "$.calls", &calls_len);
    if (calls_pos >= 0 && calls_len > 0) {
      struct mg_str calls_str = mg_str_n(val.buf + calls_pos, calls_len);
      int call_offset = 0;
      struct mg_str call_key, call_val;
      int call_count = 0;

      while ((call_offset = mg_json_next(calls_str, call_offset, &call_key,
                                         &call_val)) > 0) {
        if (call_count >= MAX_SCHEDULE_CALLS) {
          break;
        }

        schedule_call_t *call = &sched->calls[call_count];

        char *method_str = mg_json_get_str(call_val, "$.method");
        if (method_str) {
          strncpy(call->method, method_str, sizeof(call->method) - 1);
          call->method[sizeof(call->method) - 1] = '\0';
          free(method_str);
        }

        /* Get params as raw JSON string */
        int params_len = 0;
        int params_pos = mg_json_get(call_val, "$.params", &params_len);
        if (params_pos >= 0 && params_len > 0) {
          call->params_json = strndup(call_val.buf + params_pos, params_len);
        }

        call_count++;
      }
      sched->call_count = call_count;
    }

    sched->valid = 1;
    schedule_count++;
  }

  state->schedules.count = schedule_count;
  state->schedules.last_update = time(NULL);

  pthread_mutex_unlock(&state->mutex);

  printf("Loaded %d schedules (rev: %d)\n", schedule_count,
         state->schedules.rev);

  return schedule_count;
}

int device_state_get_crontab_str(device_state_t *state, char **output) {
  if (!state || !output) {
    return -1;
  }

  pthread_mutex_lock(&state->mutex);

  /* Estimate buffer size: header + entries */
  size_t buf_size = 256; /* Header */
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    if (state->schedules.schedules[i].valid) {
      /* Each schedule: comment + timespec + calls */
      buf_size += 128; /* Comment line */
      for (int j = 0; j < state->schedules.schedules[i].call_count; j++) {
        buf_size += 256; /* Each call line */
      }
    }
  }

  char *buf = malloc(buf_size);
  if (!buf) {
    pthread_mutex_unlock(&state->mutex);
    return -1;
  }

  size_t pos = 0;

  /* Write header */
  pos += snprintf(buf + pos, buf_size - pos,
                  "# Shelly device schedules (rev: %d)\n"
                  "# Format: sec min hour dom month dow method [params]\n"
                  "# Use '#!' prefix for disabled entries\n\n",
                  state->schedules.rev);

  /* Write each schedule */
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    schedule_entry_t *sched = &state->schedules.schedules[i];
    if (!sched->valid) {
      continue;
    }

    /* Write schedule ID comment */
    if (sched->enable) {
      pos += snprintf(buf + pos, buf_size - pos, "# id:%d\n", sched->id);
    } else {
      pos += snprintf(buf + pos, buf_size - pos, "# id:%d (disabled)\n",
                      sched->id);
    }

    /* Write each call */
    for (int j = 0; j < sched->call_count; j++) {
      schedule_call_t *call = &sched->calls[j];

      /* Disabled entries get #! prefix */
      const char *prefix = sched->enable ? "" : "#! ";

      if (call->params_json && strlen(call->params_json) > 0) {
        pos += snprintf(buf + pos, buf_size - pos, "%s%s %s %s\n", prefix,
                        sched->timespec, call->method, call->params_json);
      } else {
        pos += snprintf(buf + pos, buf_size - pos, "%s%s %s\n", prefix,
                        sched->timespec, call->method);
      }
    }

    pos += snprintf(buf + pos, buf_size - pos, "\n");
  }

  pthread_mutex_unlock(&state->mutex);

  *output = buf;
  return 0;
}

int device_state_create_schedule(device_state_t *state, request_queue_t *queue,
                                 struct mg_connection *conn, bool enable,
                                 const char *timespec, const char *method,
                                 const char *params) {
  if (!state || !queue || !conn || !timespec || !method) {
    return -1;
  }

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build params JSON */
  size_t params_size = 512 + (params ? strlen(params) : 0);
  char *rpc_params = malloc(params_size);
  if (!rpc_params) {
    return -1;
  }

  if (params && strlen(params) > 0) {
    snprintf(rpc_params, params_size,
             "{\"enable\":%s,\"timespec\":\"%s\",\"calls\":[{\"method\":\"%s\","
             "\"params\":%s}]}",
             enable ? "true" : "false", timespec, method, params);
  } else {
    snprintf(
        rpc_params, params_size,
        "{\"enable\":%s,\"timespec\":\"%s\",\"calls\":[{\"method\":\"%s\"}]"
        "}",
        enable ? "true" : "false", timespec, method);
  }

  char *request = jsonrpc_build_request("Schedule.Create", req_id, rpc_params);
  free(rpc_params);

  if (!request) {
    fprintf(stderr, "Error: Failed to build Schedule.Create request\n");
    return -1;
  }

  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Schedule.Create to request queue\n");
    free(request);
    return -1;
  }

  printf("Creating schedule: %s %s (ID: %d)...\n", timespec, method, req_id);

  free(request);
  return req_id;
}

int device_state_update_schedule(device_state_t *state, request_queue_t *queue,
                                 struct mg_connection *conn, int schedule_id,
                                 bool enable, const char *timespec,
                                 const char *method, const char *params) {
  if (!state || !queue || !conn || schedule_id < 0) {
    return -1;
  }

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build params JSON - only include fields that are being updated */
  size_t params_size = 512 + (params ? strlen(params) : 0) +
                       (timespec ? strlen(timespec) : 0) +
                       (method ? strlen(method) : 0);
  char *rpc_params = malloc(params_size);
  if (!rpc_params) {
    return -1;
  }

  size_t pos = 0;
  pos +=
      snprintf(rpc_params + pos, params_size - pos, "{\"id\":%d", schedule_id);
  pos += snprintf(rpc_params + pos, params_size - pos, ",\"enable\":%s",
                  enable ? "true" : "false");

  if (timespec) {
    pos += snprintf(rpc_params + pos, params_size - pos, ",\"timespec\":\"%s\"",
                    timespec);
  }

  if (method) {
    if (params && strlen(params) > 0) {
      pos += snprintf(rpc_params + pos, params_size - pos,
                      ",\"calls\":[{\"method\":\"%s\",\"params\":%s}]", method,
                      params);
    } else {
      pos += snprintf(rpc_params + pos, params_size - pos,
                      ",\"calls\":[{\"method\":\"%s\"}]", method);
    }
  }

  pos += snprintf(rpc_params + pos, params_size - pos, "}");

  char *request = jsonrpc_build_request("Schedule.Update", req_id, rpc_params);
  free(rpc_params);

  if (!request) {
    fprintf(stderr, "Error: Failed to build Schedule.Update request\n");
    return -1;
  }

  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Schedule.Update to request queue\n");
    free(request);
    return -1;
  }

  printf("Updating schedule %d (ID: %d)...\n", schedule_id, req_id);

  free(request);
  return req_id;
}

int device_state_delete_schedule(device_state_t *state, request_queue_t *queue,
                                 struct mg_connection *conn, int schedule_id) {
  if (!state || !queue || !conn || schedule_id < 0) {
    return -1;
  }

  /* Get next request ID */
  int req_id = request_queue_peek_next_id(queue);
  if (req_id < 0) {
    fprintf(stderr, "Error: Failed to get next request ID\n");
    return -1;
  }

  /* Build params JSON */
  char rpc_params[64];
  snprintf(rpc_params, sizeof(rpc_params), "{\"id\":%d}", schedule_id);

  char *request = jsonrpc_build_request("Schedule.Delete", req_id, rpc_params);
  if (!request) {
    fprintf(stderr, "Error: Failed to build Schedule.Delete request\n");
    return -1;
  }

  int added_id = request_queue_add(queue, request);
  if (added_id < 0) {
    fprintf(stderr, "Error: Failed to add Schedule.Delete to request queue\n");
    free(request);
    return -1;
  }

  printf("Deleting schedule %d (ID: %d)...\n", schedule_id, req_id);

  free(request);
  return req_id;
}

/* Parsed schedule entry from crontab content */
typedef struct {
  int id;      /* Schedule ID from "# id:N" comment, or -1 for new */
  bool enable; /* Enabled (no #! prefix) or disabled (#! prefix) */
  char timespec[MAX_SCHEDULE_TIMESPEC];
  char method[MAX_SCHEDULE_METHOD];
  char *params; /* JSON params or NULL */
} parsed_schedule_t;

/* Parse a single crontab line into a parsed schedule entry.
 * Returns 1 if a schedule entry was parsed, 0 if skipped (comment/empty),
 * -1 on error. */
static int parse_crontab_line(const char *line, size_t len,
                              parsed_schedule_t *parsed, int *current_id) {
  /* Skip empty lines */
  while (len > 0 && (*line == ' ' || *line == '\t')) {
    line++;
    len--;
  }
  if (len == 0 || *line == '\n') {
    return 0;
  }

  /* Check for ID comment: "# id:N" */
  if (len >= 5 && strncmp(line, "# id:", 5) == 0) {
    const char *id_str = line + 5;
    char *endptr;
    long id = strtol(id_str, &endptr, 10);
    if (endptr > id_str) {
      *current_id = (int) id;
    }
    return 0;
  }

  /* Skip other comments (but not #! which marks disabled entries) */
  if (*line == '#' && (len < 2 || line[1] != '!')) {
    return 0;
  }

  /* Parse schedule entry */
  bool disabled = false;
  if (len >= 2 && line[0] == '#' && line[1] == '!') {
    disabled = true;
    line += 2;
    len -= 2;
    /* Skip whitespace after #! */
    while (len > 0 && (*line == ' ' || *line == '\t')) {
      line++;
      len--;
    }
  }

  /* Parse: sec min hour dom month dow method [params]
   * Shelly timespec has 6 fields: second, minute, hour, day-of-month, month,
   * day-of-week */
  parsed->id = *current_id;
  parsed->enable = !disabled;

  /* Parse 6 timespec fields
   * Each field can be up to 20 chars (e.g., "0,1,2,3,4,5,6" for dow) */
  char timespec_parts[6][21];
  int part = 0;
  size_t pos = 0;

  while (part < 6 && pos < len) {
    /* Skip whitespace */
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) {
      pos++;
    }
    if (pos >= len) {
      break;
    }

    /* Read token */
    size_t tok_start = pos;
    while (pos < len && line[pos] != ' ' && line[pos] != '\t' &&
           line[pos] != '\n') {
      pos++;
    }

    size_t tok_len = pos - tok_start;
    if (tok_len > 0 && tok_len < sizeof(timespec_parts[0])) {
      memcpy(timespec_parts[part], line + tok_start, tok_len);
      timespec_parts[part][tok_len] = '\0';
      part++;
    }
  }

  if (part < 6) {
    /* Not enough timespec fields */
    return -1;
  }

  /* Build timespec string */
  snprintf(parsed->timespec, sizeof(parsed->timespec), "%s %s %s %s %s %s",
           timespec_parts[0], timespec_parts[1], timespec_parts[2],
           timespec_parts[3], timespec_parts[4], timespec_parts[5]);

  /* Skip whitespace before method */
  while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) {
    pos++;
  }

  /* Parse method */
  size_t method_start = pos;
  while (pos < len && line[pos] != ' ' && line[pos] != '\t' &&
         line[pos] != '\n') {
    pos++;
  }
  size_t method_len = pos - method_start;
  if (method_len == 0 || method_len >= sizeof(parsed->method)) {
    return -1;
  }
  memcpy(parsed->method, line + method_start, method_len);
  parsed->method[method_len] = '\0';

  /* Skip whitespace before params */
  while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) {
    pos++;
  }

  /* Parse params (rest of line until newline) */
  parsed->params = NULL;
  if (pos < len && line[pos] != '\n') {
    size_t params_start = pos;
    while (pos < len && line[pos] != '\n') {
      pos++;
    }
    size_t params_len = pos - params_start;
    /* Trim trailing whitespace */
    while (params_len > 0 && (line[params_start + params_len - 1] == ' ' ||
                              line[params_start + params_len - 1] == '\t')) {
      params_len--;
    }
    if (params_len > 0) {
      parsed->params = strndup(line + params_start, params_len);
    }
  }

  /* Reset current_id after using it */
  *current_id = -1;

  return 1;
}

int device_state_sync_crontab(device_state_t *state, request_queue_t *queue,
                              struct mg_connection *conn, const char *content,
                              size_t content_len) {
  if (!state || !queue || !conn || !content) {
    return -1;
  }

  /* Parse crontab content into list of schedules */
  parsed_schedule_t parsed_schedules[MAX_SCHEDULES];
  int parsed_count = 0;
  int current_id = -1;

  const char *line_start = content;
  const char *content_end = content + content_len;

  while (line_start < content_end && parsed_count < MAX_SCHEDULES) {
    /* Find end of line */
    const char *line_end = line_start;
    while (line_end < content_end && *line_end != '\n') {
      line_end++;
    }

    size_t line_len = line_end - line_start;
    parsed_schedule_t *parsed = &parsed_schedules[parsed_count];
    memset(parsed, 0, sizeof(*parsed));

    int result = parse_crontab_line(line_start, line_len, parsed, &current_id);
    if (result == 1) {
      parsed_count++;
    } else if (result < 0) {
      fprintf(stderr, "Warning: Failed to parse crontab line: '%.*s'\n",
              (int) line_len, line_start);
    }

    /* Move to next line */
    line_start = line_end;
    if (line_start < content_end && *line_start == '\n') {
      line_start++;
    }
  }

  printf("Parsed %d schedules from crontab\n", parsed_count);

  /* Track which existing schedules are still present */
  bool existing_seen[MAX_SCHEDULES] = {false};
  int ops_queued = 0;

  /* Process parsed schedules: create or update */
  for (int i = 0; i < parsed_count; i++) {
    parsed_schedule_t *p = &parsed_schedules[i];

    if (p->id >= 0) {
      /* Schedule has ID - find and update existing */
      schedule_entry_t *existing = device_state_get_schedule(state, p->id);
      if (existing) {
        /* Mark as seen */
        for (int j = 0; j < MAX_SCHEDULES; j++) {
          if (state->schedules.schedules[j].valid &&
              state->schedules.schedules[j].id == p->id) {
            existing_seen[j] = true;
            break;
          }
        }

        /* Check if update is needed */
        bool needs_update = false;
        if (existing->enable != p->enable) {
          needs_update = true;
        }
        if (strcmp(existing->timespec, p->timespec) != 0) {
          needs_update = true;
        }
        if (existing->call_count > 0) {
          if (strcmp(existing->calls[0].method, p->method) != 0) {
            needs_update = true;
          }
          const char *existing_params = existing->calls[0].params_json
                                            ? existing->calls[0].params_json
                                            : "";
          const char *new_params = p->params ? p->params : "";
          if (strcmp(existing_params, new_params) != 0) {
            needs_update = true;
          }
        } else {
          needs_update = true;
        }

        if (needs_update) {
          int ret =
              device_state_update_schedule(state, queue, conn, p->id, p->enable,
                                           p->timespec, p->method, p->params);
          if (ret >= 0) {
            ops_queued++;
          }
        }
      } else {
        fprintf(stderr,
                "Warning: Schedule ID %d not found on device, skipping "
                "(cannot create with specific ID)\n",
                p->id);
      }
    } else {
      /* No ID - create new schedule */
      int ret = device_state_create_schedule(state, queue, conn, p->enable,
                                             p->timespec, p->method, p->params);
      if (ret >= 0) {
        printf("Creating schedule (ID: %d)...\n", ret);
        ops_queued++;
      }
    }

    /* Free allocated params */
    if (p->params) {
      free(p->params);
      p->params = NULL;
    }
  }

  /* Delete schedules that were removed from crontab */
  pthread_mutex_lock(&state->mutex);
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    if (state->schedules.schedules[i].valid && !existing_seen[i]) {
      int sched_id = state->schedules.schedules[i].id;
      pthread_mutex_unlock(&state->mutex);

      int ret = device_state_delete_schedule(state, queue, conn, sched_id);
      if (ret >= 0) {
        ops_queued++;
      }

      pthread_mutex_lock(&state->mutex);
    }
  }
  pthread_mutex_unlock(&state->mutex);

  printf("Queued %d schedule operations\n", ops_queued);

  return ops_queued;
}
