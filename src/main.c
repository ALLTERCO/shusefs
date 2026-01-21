#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/device_state.h"
#include "../include/fuse_ops.h"
#include "../include/mongoose.h"
#include "../include/request_queue.h"

#define WS_URL_MAX 256

struct ws_context {
  char url[WS_URL_MAX];
  struct mg_mgr *mgr;
  struct mg_connection *conn;
  request_queue_t *req_queue;
  device_state_t *dev_state;
  int connected;
  int error;
  int fuse_started;
  char *mountpoint;
  pthread_t *fuse_thread;
};

static int s_signo = 0;
static struct ws_context *g_ctx = NULL;

static void signal_handler(int signo) {
  s_signo = signo;

  /* Unmount FUSE if it was started, so the FUSE thread can exit */
  if (g_ctx && g_ctx->fuse_started && g_ctx->mountpoint) {
    printf("\nReceived signal %d, unmounting FUSE...\n", signo);
    fuse_stop(g_ctx->mountpoint);
  }
}

static void handle_unsolicited_message(const char *msg, size_t len,
                                       void *user_data) {
  struct ws_context *ctx = (struct ws_context *) user_data;

  /* Check if this is a system configuration change notification */
  if (device_state_is_sys_config_notification(msg, len)) {
    printf("System configuration changed, refreshing...\n");
    /* Request updated configuration from device */
    device_state_request_sys_config(ctx->dev_state, ctx->req_queue, ctx->conn);
  }

  /* Check if this is an MQTT configuration change notification */
  if (device_state_is_mqtt_config_notification(msg, len)) {
    printf("MQTT configuration changed, refreshing...\n");
    /* Request updated configuration from device */
    device_state_request_mqtt_config(ctx->dev_state, ctx->req_queue, ctx->conn);
  }

  /* Check if this is a switch configuration change notification */
  if (device_state_is_switch_config_notification(msg, len, -1)) {
    /* For now, refresh all switches when any switch changes
     * TODO: Extract specific switch ID from notification and refresh only that
     * switch */
    printf("Switch configuration changed, refreshing all switches...\n");
    for (int i = 0; i < MAX_SWITCHES; i++) {
      switch_config_t *sw = device_state_get_switch(ctx->dev_state, i);
      if (sw && sw->valid) {
        device_state_request_switch_config(ctx->dev_state, ctx->req_queue,
                                           ctx->conn, i);
      }
    }
  }

  /* Check if this is a script status notification */
  if (device_state_is_script_status_notification(msg, len)) {
    /* Update script runtime status */
    device_state_update_script_status(ctx->dev_state, msg, len);
  }

  /* Check if this is a switch status notification */
  if (device_state_is_switch_status_notification(msg, len)) {
    /* Update switch status directly from notification */
    device_state_update_switch_status_from_notification(ctx->dev_state, msg);
  }

  /* Check if this is an input status notification */
  if (device_state_is_input_status_notification(msg, len)) {
    /* Update input status directly from notification */
    device_state_update_input_status_from_notification(ctx->dev_state, msg);
  }
}

static void ws_event_handler(struct mg_connection *c, int ev, void *ev_data) {
  struct ws_context *ctx = (struct ws_context *) c->fn_data;

  switch (ev) {
    case MG_EV_ERROR:
      fprintf(stderr, "Error: %s\n", (char *) ev_data);
      ctx->error = 1;
      ctx->connected = 0;
      break;

    case MG_EV_WS_OPEN:
      printf("WebSocket connection established to %s\n", ctx->url);
      ctx->connected = 1;
      ctx->error = 0;
      ctx->conn = c;

      /* Update FUSE context with connection pointer */
      if (ctx->fuse_started) {
        fuse_ops_update_conn(c);
      }

      /* Request initial device configuration state */
      printf("Requesting initial device configuration...\n");
      device_state_request_sys_config(ctx->dev_state, ctx->req_queue, c);
      device_state_request_mqtt_config(ctx->dev_state, ctx->req_queue, c);
      device_state_request_script_list(ctx->dev_state, ctx->req_queue, c);
      device_state_request_schedule_list(ctx->dev_state, ctx->req_queue, c);

      /* Request switch configurations for common switch IDs
       * Most Shelly devices have 1-4 switches, but we'll try 0-3 */
      for (int i = 0; i < 4; i++) {
        device_state_request_switch_config(ctx->dev_state, ctx->req_queue, c,
                                           i);
        /* Also request initial switch status */
        device_state_request_switch_status(ctx->dev_state, ctx->req_queue, c,
                                           i);
      }

      /* Request input configurations for common input IDs
       * Most Shelly devices have 0-3 inputs */
      for (int i = 0; i < 4; i++) {
        device_state_request_input_config(ctx->dev_state, ctx->req_queue, c, i);
        /* Also request initial input status */
        device_state_request_input_status(ctx->dev_state, ctx->req_queue, c, i);
      }
      break;

    case MG_EV_WS_MSG: {
      struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;

      /* Parse the JSON-RPC message */
      int msg_id = jsonrpc_parse_id(wm->data.buf, wm->data.len);
      int is_response = jsonrpc_is_response(wm->data.buf, wm->data.len);

      if (msg_id >= 0 && is_response) {
        /* This is a response to a previous request */
        char *msg_copy = strndup(wm->data.buf, wm->data.len);

        /* Determine response type and update appropriate device state */
        const char *request_data =
            request_queue_get_request_data(ctx->req_queue, msg_id);
        if (request_data) {
          response_type_t resp_type =
              device_state_get_response_type(request_data);

          switch (resp_type) {
            case RESPONSE_TYPE_SYS_GETCONFIG:
              device_state_update_sys_config(ctx->dev_state, msg_copy);
              break;

            case RESPONSE_TYPE_SYS_SETCONFIG: {
              /* Check if response contains error */
              char error_msg[256];
              if (jsonrpc_is_error(msg_copy, error_msg, sizeof(error_msg))) {
                fprintf(stderr, "Error setting system configuration: %s\n",
                        error_msg);
                fprintf(stderr, "Original configuration preserved.\n");
              } else {
                printf("System configuration set successfully\n");
                /* Re-request config to get updated state from device */
                device_state_request_sys_config(ctx->dev_state, ctx->req_queue,
                                                ctx->conn);
              }
            } break;

            case RESPONSE_TYPE_MQTT_GETCONFIG:
              device_state_update_mqtt_config(ctx->dev_state, msg_copy);
              break;

            case RESPONSE_TYPE_MQTT_SETCONFIG: {
              /* Check if response contains error */
              char error_msg[256];
              if (jsonrpc_is_error(msg_copy, error_msg, sizeof(error_msg))) {
                fprintf(stderr, "Error setting MQTT configuration: %s\n",
                        error_msg);
                fprintf(stderr, "Original configuration preserved.\n");
              } else {
                printf("MQTT configuration set successfully\n");
                /* Re-request config to get updated state from device */
                device_state_request_mqtt_config(ctx->dev_state, ctx->req_queue,
                                                 ctx->conn);
              }
            } break;

            case RESPONSE_TYPE_SWITCH_GETCONFIG: {
              /* Extract switch ID from original request */
              int switch_id = device_state_extract_switch_id(request_data);
              if (switch_id >= 0) {
                device_state_update_switch_config(ctx->dev_state, msg_copy,
                                                  switch_id);
              }
            } break;

            case RESPONSE_TYPE_SWITCH_SETCONFIG: {
              /* Extract switch ID from original request */
              int switch_id = device_state_extract_switch_id(request_data);
              if (switch_id >= 0) {
                /* Check if response contains error */
                char error_msg[256];
                if (jsonrpc_is_error(msg_copy, error_msg, sizeof(error_msg))) {
                  fprintf(stderr, "Error setting switch %d configuration: %s\n",
                          switch_id, error_msg);
                  fprintf(stderr, "Original configuration preserved.\n");
                } else {
                  printf("Switch %d configuration set successfully\n",
                         switch_id);
                  /* Re-request config to get updated state from device */
                  device_state_request_switch_config(
                      ctx->dev_state, ctx->req_queue, ctx->conn, switch_id);
                }
              }
            } break;

            case RESPONSE_TYPE_SCRIPT_GETCODE: {
              /* Extract script ID from original request */
              int script_id = device_state_extract_script_id(request_data);
              if (script_id >= 0) {
                /* Update with this chunk and get bytes left */
                int left = device_state_update_script_code(ctx->dev_state,
                                                           msg_copy, script_id);

                if (left > 0) {
                  /* More chunks needed, request next chunk */
                  device_state_request_script_code(
                      ctx->dev_state, ctx->req_queue, ctx->conn, script_id);
                } else if (left == 0) {
                  /* Complete, finalize the script */
                  device_state_finalize_script_code(ctx->dev_state, script_id);

                  /* Request next script if any */
                  int found_next = 0;
                  for (int i = script_id + 1; i < MAX_SCRIPTS; i++) {
                    script_entry_t *script =
                        device_state_get_script(ctx->dev_state, i);
                    if (script && script->valid && !script->code) {
                      device_state_request_script_code(
                          ctx->dev_state, ctx->req_queue, ctx->conn, i);
                      found_next = 1;
                      break;
                    }
                  }
                  if (!found_next) {
                    printf("All script code retrieved successfully\n");
                  }
                }
                /* left < 0 means error, already logged */
              }
            } break;

            case RESPONSE_TYPE_SCRIPT_LIST: {
              /* Update script list and request code for first script only */
              int script_count =
                  device_state_update_script_list(ctx->dev_state, msg_copy);
              if (script_count > 0) {
                printf("Found %d scripts, requesting code sequentially...\n",
                       script_count);
                /* Request code for first script only - others will be requested
                 * after completion */
                for (int i = 0; i < MAX_SCRIPTS; i++) {
                  script_entry_t *script =
                      device_state_get_script(ctx->dev_state, i);
                  if (script && script->valid) {
                    device_state_request_script_code(
                        ctx->dev_state, ctx->req_queue, ctx->conn, i);
                    break; /* Only request first script */
                  }
                }
              }
            } break;

            case RESPONSE_TYPE_SCRIPT_PUTCODE: {
              /* Extract script ID from original request */
              int script_id = device_state_extract_script_id(request_data);
              if (script_id >= 0) {
                /* Check if response contains error */
                char error_msg[256];
                if (jsonrpc_is_error(msg_copy, error_msg, sizeof(error_msg))) {
                  fprintf(stderr, "Error uploading script %d chunk: %s\n",
                          script_id, error_msg);
                } else {
                  printf("Script %d chunk uploaded successfully\n", script_id);

                  /* Check if this is the last chunk by comparing with stored
                   * request ID */
                  script_entry_t *script =
                      device_state_get_script(ctx->dev_state, script_id);
                  if (script && script->last_upload_req_id == msg_id) {
                    printf(
                        "Script %d upload complete, refreshing from "
                        "device...\n",
                        script_id);
                    /* Re-request script code to get canonical version from
                     * device
                     */
                    device_state_request_script_code(
                        ctx->dev_state, ctx->req_queue, ctx->conn, script_id);
                  }
                }
              }
            } break;

            case RESPONSE_TYPE_SWITCH_SET: {
              /* Extract switch ID from original request */
              int switch_id = device_state_extract_switch_id(request_data);
              if (switch_id >= 0) {
                /* Check if response contains error */
                char error_msg[256];
                if (jsonrpc_is_error(msg_copy, error_msg, sizeof(error_msg))) {
                  fprintf(stderr, "Error setting switch %d state: %s\n",
                          switch_id, error_msg);
                } else {
                  printf("Switch %d state set successfully\n", switch_id);
                  /* Response contains the current switch status, update it */
                  device_state_update_switch_status(ctx->dev_state, msg_copy,
                                                    switch_id);
                }
              }
            } break;

            case RESPONSE_TYPE_SWITCH_GETSTATUS: {
              /* Extract switch ID from original request */
              int switch_id = device_state_extract_switch_id(request_data);
              if (switch_id >= 0) {
                device_state_update_switch_status(ctx->dev_state, msg_copy,
                                                  switch_id);
              }
            } break;

            case RESPONSE_TYPE_INPUT_GETCONFIG: {
              /* Extract input ID from original request */
              int input_id = device_state_extract_input_id(request_data);
              if (input_id >= 0) {
                device_state_update_input_config(ctx->dev_state, msg_copy,
                                                 input_id);
              }
            } break;

            case RESPONSE_TYPE_INPUT_SETCONFIG: {
              /* Extract input ID from original request */
              int input_id = device_state_extract_input_id(request_data);
              if (input_id >= 0) {
                /* Check if response contains error */
                char error_msg[256];
                if (jsonrpc_is_error(msg_copy, error_msg, sizeof(error_msg))) {
                  fprintf(stderr, "Error setting input %d configuration: %s\n",
                          input_id, error_msg);
                  fprintf(stderr, "Original configuration preserved.\n");
                } else {
                  printf("Input %d configuration set successfully\n", input_id);
                  /* Re-request config to get updated state from device */
                  device_state_request_input_config(
                      ctx->dev_state, ctx->req_queue, ctx->conn, input_id);
                }
              }
            } break;

            case RESPONSE_TYPE_INPUT_GETSTATUS: {
              /* Extract input ID from original request */
              int input_id = device_state_extract_input_id(request_data);
              if (input_id >= 0) {
                device_state_update_input_status(ctx->dev_state, msg_copy,
                                                 input_id);
              }
            } break;

            case RESPONSE_TYPE_SCRIPT_CREATE:
            case RESPONSE_TYPE_SCRIPT_DELETE:
              /* Script operations - to be implemented */
              break;

            case RESPONSE_TYPE_SCHEDULE_LIST: {
              int schedule_count =
                  device_state_update_schedule_list(ctx->dev_state, msg_copy);
              if (schedule_count >= 0) {
                printf("Loaded %d schedules\n", schedule_count);
              }
            } break;

            case RESPONSE_TYPE_SCHEDULE_CREATE:
            case RESPONSE_TYPE_SCHEDULE_UPDATE:
            case RESPONSE_TYPE_SCHEDULE_DELETE: {
              /* Check for errors */
              char error_msg[256];
              if (jsonrpc_is_error(msg_copy, error_msg, sizeof(error_msg))) {
                fprintf(stderr, "Schedule operation failed: %s\n", error_msg);
              } else {
                printf("Schedule modified, refreshing list...\n");
              }
              /* Refresh the schedule list regardless */
              device_state_request_schedule_list(ctx->dev_state, ctx->req_queue,
                                                 ctx->conn);
            } break;

            case RESPONSE_TYPE_OTHER:
              /* Handle other response types here */
              break;

            case RESPONSE_TYPE_UNKNOWN:
              /* Unknown response type, no state update */
              break;

            default:
              /* Unreachable: all enum values handled above */
              assert(0 && "Unreachable: unknown response type");
              break;
          }
        }

        if (request_queue_handle_response(ctx->req_queue, msg_id, msg_copy) !=
            0) {
          fprintf(stderr,
                  "Warning: Received response for unknown request ID %d\n",
                  msg_id);
          free(msg_copy);
        }
      } else {
        /* This is an unsolicited message (notification) */
        handle_unsolicited_message(wm->data.buf, wm->data.len, ctx);
      }
    } break;

    case MG_EV_CLOSE:
      if (ctx->connected) {
        printf("WebSocket connection closed\n");
      }
      ctx->connected = 0;
      break;

    default: break;
  }
}

static void *ws_thread_func(void *arg) {
  struct ws_context *ctx = (struct ws_context *) arg;

  printf("Starting WebSocket thread for %s\n", ctx->url);

  ctx->mgr = malloc(sizeof(struct mg_mgr));
  if (!ctx->mgr) {
    fprintf(stderr, "Error: Failed to allocate memory for mongoose manager\n");
    ctx->error = 1;
    return NULL;
  }

  mg_mgr_init(ctx->mgr);

  ctx->conn = mg_ws_connect(ctx->mgr, ctx->url, ws_event_handler, ctx, NULL);
  if (!ctx->conn) {
    fprintf(stderr, "Error: Failed to create WebSocket connection to %s\n",
            ctx->url);
    ctx->error = 1;
    mg_mgr_free(ctx->mgr);
    free(ctx->mgr);
    return NULL;
  }

  int cleanup_counter = 0;
  while (s_signo == 0) {
    mg_mgr_poll(ctx->mgr, 1000);

    /* Check for queued requests and send them */
    if (ctx->conn && ctx->connected) {
      char *request_data = NULL;
      int req_id = 0;

      while (request_queue_get_next_to_send(ctx->req_queue, &request_data,
                                            &req_id) == 0) {
        if (request_data) {
          /* Send the request over WebSocket */
          if (jsonrpc_send_request(ctx->conn, request_data) == 0) {
            /* Mark as sent (transitions to PENDING state) */
            request_queue_mark_sent(ctx->req_queue, req_id);
          } else {
            fprintf(stderr, "Error: Failed to send request ID %d\n", req_id);
            break; /* Stop trying to send more if one fails */
          }
        }
      }
    }

    /* Periodically clean up timed-out requests (every 10 seconds) */
    if (++cleanup_counter >= 10) {
      request_queue_cleanup_timeouts(ctx->req_queue);
      cleanup_counter = 0;
    }
  }

  printf("Shutting down WebSocket connection...\n");
  mg_mgr_free(ctx->mgr);
  free(ctx->mgr);
  ctx->mgr = NULL;
  ctx->conn = NULL;

  return NULL;
}

static void print_usage(const char *prog_name) {
  printf(
      "Shelly FUSE Filesystem - Mount Shelly Gen2+ devices as a "
      "filesystem\n\n");
  printf("Usage: %s <device_url> <mountpoint>\n\n", prog_name);
  printf("Arguments:\n");
  printf(
      "  device_url   WebSocket URL of the Shelly device (ws:// or wss://)\n");
  printf("  mountpoint   Directory where the device will be mounted\n\n");
  printf("Example:\n");
  printf("  %s ws://192.168.1.100:80/rpc /tmp/shelly\n\n", prog_name);
  printf("After mounting, you can access:\n");
  printf("  - System config:  <mountpoint>/sys_config.json\n");
  printf("  - MQTT config:    <mountpoint>/mqtt_config.json\n");
  printf("  - Switch control: <mountpoint>/proc/switch/N/output\n");
  printf(
      "  - Switch metrics: "
      "<mountpoint>/proc/switch/N/"
      "{apower,voltage,current,energy,temperature}\n");
  printf("  - Scripts:        <mountpoint>/scripts/script_N.js\n\n");
  printf("To unmount:\n");
  printf("  fusermount -u <mountpoint>\n");
  printf("  or press Ctrl+C in the terminal running shusefs\n");
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  struct ws_context ctx = {0};
  pthread_t ws_thread;
  pthread_t fuse_thread;
  request_queue_t req_queue;
  device_state_t dev_state;

  /* Initialize request queue */
  if (request_queue_init(&req_queue) != 0) {
    fprintf(stderr, "Error: Failed to initialize request queue\n");
    return EXIT_FAILURE;
  }

  /* Initialize device state */
  if (device_state_init(&dev_state) != 0) {
    fprintf(stderr, "Error: Failed to initialize device state\n");
    request_queue_destroy(&req_queue);
    return EXIT_FAILURE;
  }

  ctx.req_queue = &req_queue;
  ctx.dev_state = &dev_state;
  ctx.mountpoint = argv[2];
  ctx.fuse_thread = &fuse_thread;
  ctx.fuse_started = 0;

  strncpy(ctx.url, argv[1], WS_URL_MAX - 1);
  ctx.url[WS_URL_MAX - 1] = '\0';

  if (strncmp(ctx.url, "ws://", 5) != 0 && strncmp(ctx.url, "wss://", 6) != 0) {
    fprintf(stderr, "Error: URL must start with ws:// or wss://\n");
    device_state_destroy(&dev_state);
    request_queue_destroy(&req_queue);
    return EXIT_FAILURE;
  }

  /* Set global context for signal handler */
  g_ctx = &ctx;

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  /* Start FUSE filesystem immediately */
  printf("Starting FUSE filesystem at %s...\n", ctx.mountpoint);

  /* Prepare FUSE arguments */
  char **fuse_argv = malloc(sizeof(char *) * 5);
  if (!fuse_argv) {
    fprintf(stderr, "Error: Failed to allocate memory for FUSE arguments\n");
    device_state_destroy(&dev_state);
    request_queue_destroy(&req_queue);
    return EXIT_FAILURE;
  }

  /* Only pass program name, mountpoint will be passed separately */
  fuse_argv[0] = strdup("shusefs");
  fuse_argv[1] = strdup("-o");
  fuse_argv[2] = strdup("default_permissions");
  fuse_argv[3] = strdup(ctx.mountpoint);
  fuse_argv[4] = NULL;

  if (fuse_start((const char *) fuse_argv, &dev_state, &req_queue, &ctx.conn,
                 &fuse_thread) != 0) {
    fprintf(stderr, "Error: Failed to start FUSE filesystem\n");
    for (int i = 0; fuse_argv[i] != NULL; i++) {
      free(fuse_argv[i]);
    }
    free(fuse_argv);
    device_state_destroy(&dev_state);
    request_queue_destroy(&req_queue);
    return EXIT_FAILURE;
  }
  ctx.fuse_started = 1;

  printf("Connecting to %s\n", ctx.url);

  if (pthread_create(&ws_thread, NULL, ws_thread_func, &ctx) != 0) {
    fprintf(stderr, "Error: Failed to create WebSocket thread\n");
    fuse_stop(ctx.mountpoint);
    pthread_join(fuse_thread, NULL);
    device_state_destroy(&dev_state);
    request_queue_destroy(&req_queue);
    return EXIT_FAILURE;
  }

  pthread_join(ws_thread, NULL);

  /* Clean up FUSE if it was started */
  if (ctx.fuse_started) {
    /* Only call fuse_stop if signal handler didn't already unmount */
    if (s_signo == 0) {
      printf("Unmounting FUSE filesystem...\n");
      fuse_stop(ctx.mountpoint);
    }
    pthread_join(fuse_thread, NULL);
  }

  /* Clear global context pointer */
  g_ctx = NULL;

  /* Clean up */
  device_state_destroy(&dev_state);
  request_queue_destroy(&req_queue);

  if (ctx.error) {
    fprintf(stderr, "WebSocket connection terminated with errors\n");
    return EXIT_FAILURE;
  }

  printf("Disconnected successfully\n");
  return EXIT_SUCCESS;
}
