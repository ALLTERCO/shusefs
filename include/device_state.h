#ifndef DEVICE_STATE_H
#define DEVICE_STATE_H

#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include "request_queue.h"

/* Forward declaration for mongoose types */
struct mg_connection;

/* ============================================================================
 * CONSTANTS AND CONFIGURATION
 * ============================================================================
 */

#define MAX_CONFIG_SIZE 8192
#define MAX_DEVICE_NAME 64
#define MAX_LOCATION 128
#define MAX_SERVER_URL 256
#define MAX_CLIENT_ID 64
#define MAX_USER_ID 64
#define MAX_TOPIC_PREFIX 128
#define MAX_SCRIPTS 10
#define MAX_SCRIPT_NAME 64
#define MAX_SCRIPT_CODE 20480
#define SCRIPT_CHUNK_SIZE 2048
#define MAX_SWITCHES 16
#define MAX_SWITCH_NAME 64
#define MAX_INPUTS 16
#define MAX_INPUT_NAME 64
#define MAX_SCHEDULES 20
#define MAX_SCHEDULE_CALLS 5
#define MAX_SCHEDULE_METHOD 64
#define MAX_SCHEDULE_TIMESPEC 128

/* SSL CA verification scheme for MQTT */
typedef enum {
  SSL_CA_NONE = 0, /* No verification */
  SSL_CA_USER,     /* User-provided CA (user_ca.pem) */
  SSL_CA_DEFAULT   /* Default CA bundle (ca.pem) */
} ssl_ca_t;

/* Switch input mode */
typedef enum {
  SWITCH_IN_MODE_MOMENTARY = 0,
  SWITCH_IN_MODE_FOLLOW,
  SWITCH_IN_MODE_FLIP,
  SWITCH_IN_MODE_DETACHED,
  SWITCH_IN_MODE_UNKNOWN
} switch_in_mode_t;

/* Switch initial state */
typedef enum {
  SWITCH_INITIAL_ON = 0,
  SWITCH_INITIAL_OFF,
  SWITCH_INITIAL_RESTORE_LAST,
  SWITCH_INITIAL_MATCH_INPUT,
  SWITCH_INITIAL_UNKNOWN
} switch_initial_state_t;

/* Response type enumeration */
typedef enum {
  RESPONSE_TYPE_UNKNOWN = 0,
  RESPONSE_TYPE_SYS_GETCONFIG,
  RESPONSE_TYPE_SYS_SETCONFIG,
  RESPONSE_TYPE_MQTT_GETCONFIG,
  RESPONSE_TYPE_MQTT_SETCONFIG,
  RESPONSE_TYPE_SWITCH_GETCONFIG,
  RESPONSE_TYPE_SWITCH_SETCONFIG,
  RESPONSE_TYPE_SWITCH_SET,
  RESPONSE_TYPE_SWITCH_GETSTATUS,
  RESPONSE_TYPE_INPUT_GETCONFIG,
  RESPONSE_TYPE_INPUT_SETCONFIG,
  RESPONSE_TYPE_INPUT_GETSTATUS,
  RESPONSE_TYPE_SCRIPT_LIST,
  RESPONSE_TYPE_SCRIPT_GETCODE,
  RESPONSE_TYPE_SCRIPT_PUTCODE,
  RESPONSE_TYPE_SCRIPT_CREATE,
  RESPONSE_TYPE_SCRIPT_DELETE,
  RESPONSE_TYPE_SCHEDULE_LIST,
  RESPONSE_TYPE_SCHEDULE_CREATE,
  RESPONSE_TYPE_SCHEDULE_UPDATE,
  RESPONSE_TYPE_SCHEDULE_DELETE,
  RESPONSE_TYPE_OTHER
} response_type_t;

/* ============================================================================
 * DATA STRUCTURES
 * ============================================================================
 */

/* System configuration state */
typedef struct {
  char *raw_json; /* Raw JSON from sys.getconfig */
  size_t json_len;

  /* Parsed fields (optional, for convenience) */
  struct {
    char device_name[MAX_DEVICE_NAME];
    char location[MAX_LOCATION];
    bool eco_mode;
    int sntp_enabled;
    char *timezone;
  } parsed;

  int valid;          /* 1 if configuration is valid/loaded */
  time_t last_update; /* Timestamp of last update */
} sys_config_t;

/* MQTT configuration state */
typedef struct {
  char *raw_json; /* Raw JSON from MQTT.GetConfig */
  size_t json_len;

  /* Parsed fields (optional, for convenience) */
  struct {
    bool enable;
    char server[MAX_SERVER_URL];
    char client_id[MAX_CLIENT_ID];
    char user[MAX_USER_ID];
    char topic_prefix[MAX_TOPIC_PREFIX];
    ssl_ca_t ssl_ca;
    bool enable_control;
    bool rpc_ntf;
    bool status_ntf;
    bool use_client_cert;
    bool enable_rpc;
  } parsed;

  int valid;          /* 1 if configuration is valid/loaded */
  time_t last_update; /* Timestamp of last update */
} mqtt_config_t;

/* Individual switch configuration */
typedef struct {
  int id;         /* Switch ID (0-15) */
  char *raw_json; /* Raw JSON from Switch.GetConfig */
  size_t json_len;

  /* Parsed fields */
  struct {
    char name[MAX_SWITCH_NAME];
    switch_in_mode_t in_mode;
    bool in_locked;
    switch_initial_state_t initial_state;
    bool auto_on;
    double auto_on_delay;
    bool auto_off;
    double auto_off_delay;
    int power_limit;
    int voltage_limit;
    bool autorecover_voltage_errors;
    double current_limit;
  } parsed;

  /* Runtime status */
  struct {
    int id;                  /* Switch ID */
    char source[32];         /* Power source (e.g., "WS_in", "init") */
    bool output;             /* Current output state (on/off) */
    double apower;           /* Active power in watts */
    double voltage;          /* Voltage in volts */
    double current;          /* Current in amperes */
    double freq;             /* AC frequency in Hz (if available) */
    double energy_total;     /* Total energy consumed in Wh (aenergy.total) */
    double ret_energy_total; /* Total returned energy in Wh (ret_aenergy.total,
                                if available) */
    double temperature_c;    /* Temperature in Celsius */
    double temperature_f;    /* Temperature in Fahrenheit */
    bool overtemperature;    /* Overtemperature flag */
    time_t last_status_update; /* Timestamp of last status update */

    /* Per-field modification times for inotify support */
    time_t mtime_id;
    time_t mtime_source;
    time_t mtime_output;
    time_t mtime_apower;
    time_t mtime_voltage;
    time_t mtime_current;
    time_t mtime_freq;
    time_t mtime_energy;
    time_t mtime_ret_energy;
    time_t mtime_temperature;
  } status;

  int valid;          /* 1 if configuration is valid/loaded */
  time_t last_update; /* Timestamp of last update */
} switch_config_t;

/* Switches state */
typedef struct {
  switch_config_t switches[MAX_SWITCHES];
  int count;          /* Number of valid switches */
  time_t last_update; /* Timestamp of last update */
} switches_state_t;

/* Input type */
typedef enum {
  INPUT_TYPE_SWITCH = 0,
  INPUT_TYPE_BUTTON,
  INPUT_TYPE_ANALOG,
  INPUT_TYPE_UNKNOWN
} input_type_t;

/* Individual input configuration */
typedef struct {
  int id;         /* Input ID (0-15) */
  char *raw_json; /* Raw JSON from Input.GetConfig */
  size_t json_len;

  /* Parsed fields */
  struct {
    char name[MAX_INPUT_NAME];
    input_type_t type;
    bool enable;
    bool invert;
    bool factory_reset;
  } parsed;

  /* Runtime status */
  struct {
    int id;                    /* Input ID */
    bool state;                /* Current input state */
    time_t last_status_update; /* Timestamp of last status update */
    time_t mtime_id;           /* Per-field mtime for inotify */
    time_t mtime_state;        /* Per-field mtime for inotify */
  } status;

  int valid;          /* 1 if configuration is valid/loaded */
  time_t last_update; /* Timestamp of last update */
} input_config_t;

/* Inputs state */
typedef struct {
  input_config_t inputs[MAX_INPUTS];
  int count;          /* Number of valid inputs */
  time_t last_update; /* Timestamp of last update */
} inputs_state_t;

/* Individual script entry */
typedef struct {
  int id;                     /* Script ID (0-9) */
  char name[MAX_SCRIPT_NAME]; /* Script name */
  bool enable;                /* Script enabled flag */
  char *code;                 /* Script code (dynamically allocated) */
  time_t create_time;         /* Script creation timestamp */
  time_t modify_time;         /* Script modification timestamp */
  int valid;                  /* 1 if script slot is populated */

  /* Runtime status */
  bool running;              /* Script is currently running */
  int mem_used;              /* Memory used in bytes */
  int mem_peak;              /* Peak memory used in bytes */
  char *errors;              /* Error messages (dynamically allocated) */
  time_t last_status_update; /* Timestamp of last status update */

  /* Upload tracking */
  int last_upload_req_id; /* Request ID of last chunk in upload (-1 if none) */
} script_entry_t;

/* Scripts state */
typedef struct {
  script_entry_t scripts[MAX_SCRIPTS];
  int count;          /* Number of valid scripts */
  time_t last_update; /* Timestamp of last update */

  /* Chunk retrieval state */
  int retrieving_id;  /* Script ID currently being retrieved (-1 if none) */
  int current_offset; /* Current offset for chunked retrieval */
  char *chunk_buffer; /* Temporary buffer for accumulating chunks */
  size_t chunk_buffer_size; /* Current size of chunk buffer */
} scripts_state_t;

/* Schedule call (RPC method to execute) */
typedef struct {
  char method[MAX_SCHEDULE_METHOD]; /* RPC method name */
  char *params_json; /* JSON params string (dynamically allocated) */
} schedule_call_t;

/* Individual schedule entry */
typedef struct {
  int id;                                    /* Schedule ID */
  bool enable;                               /* Schedule enabled flag */
  char timespec[MAX_SCHEDULE_TIMESPEC];      /* Cron-like time specification */
  schedule_call_t calls[MAX_SCHEDULE_CALLS]; /* RPC calls to execute */
  int call_count; /* Number of calls in this schedule */
  int valid;      /* 1 if schedule slot is populated */
} schedule_entry_t;

/* Schedules state */
typedef struct {
  schedule_entry_t schedules[MAX_SCHEDULES];
  int count;          /* Number of valid schedules */
  int rev;            /* Revision number for change tracking */
  time_t last_update; /* Timestamp of last update */
} schedules_state_t;

/* Overall device state */
typedef struct {
  sys_config_t sys_config;
  mqtt_config_t mqtt_config;
  switches_state_t switches;
  inputs_state_t inputs;
  scripts_state_t scripts;
  schedules_state_t schedules;

  pthread_mutex_t mutex; /* Single mutex for entire device state */

  /* Future: add more state components here
   * - status
   * - other configs
   */
} device_state_t;

/* ============================================================================
 * INITIALIZATION AND CLEANUP
 * ============================================================================
 */

/* Initialize device state */
int device_state_init(device_state_t *state);

/* Destroy device state and free resources */
void device_state_destroy(device_state_t *state);

/* ============================================================================
 * JSON-RPC UTILITIES
 * ============================================================================
 */

/* Build JSON-RPC request */
char *jsonrpc_build_request(const char *method, int id, const char *params);

/* Send JSON-RPC request over WebSocket */
int jsonrpc_send_request(struct mg_connection *conn, const char *request);

/* Check if JSON-RPC response contains an error */
int jsonrpc_is_error(const char *response_json, char *error_buf,
                     size_t error_buf_size);

/* Determine response type from JSON-RPC request */
response_type_t device_state_get_response_type(const char *request_json);

/* Extract script ID from JSON-RPC request params */
int device_state_extract_script_id(const char *request_json);

/* Extract switch ID from JSON-RPC request params */
int device_state_extract_switch_id(const char *request_json);

/* ============================================================================
 * SYSTEM CONFIGURATION (Sys.GetConfig / Sys.SetConfig)
 * ============================================================================
 */

/* Request system configuration from device */
int device_state_request_sys_config(device_state_t *state,
                                    request_queue_t *queue,
                                    struct mg_connection *conn);

/* Update system configuration with received data */
int device_state_update_sys_config(device_state_t *state, const char *json);

/* Get system configuration as string (for file operations) */
int device_state_get_sys_config_str(device_state_t *state, char **output);

/* Serialize parsed sys_config back to JSON (rebuilds raw_json from parsed
 * fields) */
int device_state_serialize_sys_config(device_state_t *state);

/* Set system configuration on device */
int device_state_set_sys_config(device_state_t *state, request_queue_t *queue,
                                struct mg_connection *conn);

/* Set system configuration on device from plain user-written JSON */
int device_state_set_sys_config_from_json(const char *user_json,
                                          request_queue_t *queue,
                                          struct mg_connection *conn);

/* Check if notification is about system configuration change */
int device_state_is_sys_config_notification(const char *json, size_t len);

/* ============================================================================
 * MQTT CONFIGURATION (MQTT.GetConfig / MQTT.SetConfig)
 * ============================================================================
 */

/* Request MQTT configuration from device */
int device_state_request_mqtt_config(device_state_t *state,
                                     request_queue_t *queue,
                                     struct mg_connection *conn);

/* Update MQTT configuration with received data */
int device_state_update_mqtt_config(device_state_t *state, const char *json);

/* Get MQTT configuration as string (for file operations) */
int device_state_get_mqtt_config_str(device_state_t *state, char **output);

/* Serialize parsed mqtt_config back to JSON (rebuilds raw_json from parsed
 * fields) */
int device_state_serialize_mqtt_config(device_state_t *state);

/* Set MQTT configuration on device */
int device_state_set_mqtt_config(device_state_t *state, request_queue_t *queue,
                                 struct mg_connection *conn);

/* Set MQTT configuration on device from plain user-written JSON */
int device_state_set_mqtt_config_from_json(const char *user_json,
                                           request_queue_t *queue,
                                           struct mg_connection *conn);

/* Check if notification is about MQTT configuration change */
int device_state_is_mqtt_config_notification(const char *json, size_t len);

/* ============================================================================
 * SWITCH CONFIGURATION (Switch.GetConfig / Switch.SetConfig)
 * ============================================================================
 */

/* Request switch configuration from device */
int device_state_request_switch_config(device_state_t *state,
                                       request_queue_t *queue,
                                       struct mg_connection *conn,
                                       int switch_id);

/* Update switch configuration with received data */
int device_state_update_switch_config(device_state_t *state, const char *json,
                                      int switch_id);

/* Get switch configuration as string (for file operations) */
int device_state_get_switch_config_str(device_state_t *state, int switch_id,
                                       char **output);

/* Set switch configuration on device from plain user-written JSON */
int device_state_set_switch_config_from_json(const char *user_json,
                                             request_queue_t *queue,
                                             struct mg_connection *conn,
                                             int switch_id);

/* Check if notification is about switch configuration change */
int device_state_is_switch_config_notification(const char *json, size_t len,
                                               int switch_id);

/* Helper: Get switch by ID */
switch_config_t *device_state_get_switch(device_state_t *state, int switch_id);

/* ============================================================================
 * SWITCH CONTROL (Switch.Set / Switch.GetStatus)
 * ============================================================================
 */

/* Set switch state (on/off) */
int device_state_set_switch(device_state_t *state, request_queue_t *queue,
                            struct mg_connection *conn, int switch_id, bool on);

/* Request switch status from device */
int device_state_request_switch_status(device_state_t *state,
                                       request_queue_t *queue,
                                       struct mg_connection *conn,
                                       int switch_id);

/* Update switch status with received data */
int device_state_update_switch_status(device_state_t *state, const char *json,
                                      int switch_id);

/* Check if notification is about switch status change */
int device_state_is_switch_status_notification(const char *json, size_t len);

/* Update switch status from NotifyStatus notification */
int device_state_update_switch_status_from_notification(device_state_t *state,
                                                        const char *json);

/* ============================================================================
 * INPUT CONFIGURATION (Input.GetConfig / Input.SetConfig)
 * ============================================================================
 */

/* Request input configuration from device */
int device_state_request_input_config(device_state_t *state,
                                      request_queue_t *queue,
                                      struct mg_connection *conn, int input_id);

/* Update input configuration with received data */
int device_state_update_input_config(device_state_t *state, const char *json,
                                     int input_id);

/* Get input configuration as string (for file operations) */
int device_state_get_input_config_str(device_state_t *state, int input_id,
                                      char **output);

/* Set input configuration on device from plain user-written JSON */
int device_state_set_input_config_from_json(const char *user_json,
                                            request_queue_t *queue,
                                            struct mg_connection *conn,
                                            int input_id);

/* Check if notification is about input configuration change */
int device_state_is_input_config_notification(const char *json, size_t len,
                                              int input_id);

/* Helper: Get input by ID */
input_config_t *device_state_get_input(device_state_t *state, int input_id);

/* ============================================================================
 * INPUT STATUS (Input.GetStatus)
 * ============================================================================
 */

/* Request input status from device */
int device_state_request_input_status(device_state_t *state,
                                      request_queue_t *queue,
                                      struct mg_connection *conn, int input_id);

/* Update input status with received data */
int device_state_update_input_status(device_state_t *state, const char *json,
                                     int input_id);

/* Check if notification is about input status change */
int device_state_is_input_status_notification(const char *json, size_t len);

/* Update input status from NotifyStatus notification */
int device_state_update_input_status_from_notification(device_state_t *state,
                                                       const char *json);

/* Extract input ID from JSON-RPC request params */
int device_state_extract_input_id(const char *request_json);

/* ============================================================================
 * SCRIPT LISTING (Script.List)
 * ============================================================================
 */

/* Request script list from device */
int device_state_request_script_list(device_state_t *state,
                                     request_queue_t *queue,
                                     struct mg_connection *conn);

/* Update script list with received data */
int device_state_update_script_list(device_state_t *state, const char *json);

/* ============================================================================
 * SCRIPT CODE MANAGEMENT (Script.GetCode / Script.PutCode)
 * ============================================================================
 */

/* Get script by ID (helper) */
script_entry_t *device_state_get_script(device_state_t *state, int script_id);

/* Request specific script code by ID */
int device_state_request_script_code(device_state_t *state,
                                     request_queue_t *queue,
                                     struct mg_connection *conn, int script_id);

/* Update specific script code with received chunk */
int device_state_update_script_code(device_state_t *state, const char *json,
                                    int script_id);

/* Finalize script code retrieval (move from chunk buffer to script entry) */
int device_state_finalize_script_code(device_state_t *state, int script_id);

/* Get script code as string (for file operations) */
int device_state_get_script_code_str(device_state_t *state, int script_id,
                                     char **output);

/* Put (upload) script code to device */
int device_state_put_script_code(device_state_t *state, request_queue_t *queue,
                                 struct mg_connection *conn, int script_id,
                                 const char *code);

/* ============================================================================
 * SCRIPT STATUS NOTIFICATIONS
 * ============================================================================
 */

/* Check if notification contains script status updates */
int device_state_is_script_status_notification(const char *json, size_t len);

/* Update script runtime status from notification */
int device_state_update_script_status(device_state_t *state, const char *json,
                                      size_t len);

/* ============================================================================
 * NOTIFICATION UTILITIES
 * ============================================================================
 */

/* Check if notification is about a specific component change */
int device_state_is_component_notification(const char *json, size_t len,
                                           const char *component);

/* ============================================================================
 * SCHEDULE MANAGEMENT (Schedule.List / Schedule.Create / Schedule.Update /
 * Schedule.Delete)
 * ============================================================================
 */

/* Request schedule list from device */
int device_state_request_schedule_list(device_state_t *state,
                                       request_queue_t *queue,
                                       struct mg_connection *conn);

/* Update schedule list with received data */
int device_state_update_schedule_list(device_state_t *state, const char *json);

/* Get crontab-format string representation of schedules */
int device_state_get_crontab_str(device_state_t *state, char **output);

/* Parse crontab content and sync changes to device.
 * Returns number of schedule operations queued, or -1 on error. */
int device_state_sync_crontab(device_state_t *state, request_queue_t *queue,
                              struct mg_connection *conn, const char *content,
                              size_t content_len);

/* Create a new schedule on device */
int device_state_create_schedule(device_state_t *state, request_queue_t *queue,
                                 struct mg_connection *conn, bool enable,
                                 const char *timespec, const char *method,
                                 const char *params);

/* Update an existing schedule on device */
int device_state_update_schedule(device_state_t *state, request_queue_t *queue,
                                 struct mg_connection *conn, int schedule_id,
                                 bool enable, const char *timespec,
                                 const char *method, const char *params);

/* Delete a schedule from device */
int device_state_delete_schedule(device_state_t *state, request_queue_t *queue,
                                 struct mg_connection *conn, int schedule_id);

/* Helper: Get schedule by ID */
schedule_entry_t *device_state_get_schedule(device_state_t *state,
                                            int schedule_id);

#endif /* DEVICE_STATE_H */
