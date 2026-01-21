#ifndef REQUEST_QUEUE_H
#define REQUEST_QUEUE_H

#include <pthread.h>
#include <time.h>

#define MAX_PENDING_REQUESTS 64
#define REQUEST_TIMEOUT_SEC 30

typedef enum {
  REQ_STATE_QUEUED,  /* Request queued, not yet sent */
  REQ_STATE_PENDING, /* Request sent, awaiting response */
  REQ_STATE_COMPLETED,
  REQ_STATE_TIMEOUT,
  REQ_STATE_ERROR
} request_state_t;

typedef struct {
  int id;
  request_state_t state;
  char *request_data;
  char *response_data;
  time_t timestamp;
  pthread_cond_t cond;
} request_entry_t;

typedef struct {
  request_entry_t entries[MAX_PENDING_REQUESTS];
  pthread_mutex_t mutex;
  int next_id;
} request_queue_t;

typedef void (*unsolicited_msg_handler_t)(const char *msg, size_t len,
                                          void *user_data);

/* Initialize the request queue */
int request_queue_init(request_queue_t *queue);

/* Destroy the request queue and free resources */
void request_queue_destroy(request_queue_t *queue);

/* Get the next request ID without consuming it */
int request_queue_peek_next_id(request_queue_t *queue);

/* Add a new request to the queue, returns request ID or -1 on error */
int request_queue_add(request_queue_t *queue, const char *request_data);

/* Handle incoming response by matching it to a pending request */
int request_queue_handle_response(request_queue_t *queue, int req_id,
                                  const char *response_data);

/* Handle unsolicited message (no matching request ID) */
void request_queue_handle_unsolicited(const char *msg, size_t len,
                                      unsolicited_msg_handler_t handler,
                                      void *user_data);

/* Clean up timed-out requests */
void request_queue_cleanup_timeouts(request_queue_t *queue);

/* Get original request data by request ID (returns NULL if not found) */
const char *request_queue_get_request_data(request_queue_t *queue, int req_id);

/* Get next queued request that needs to be sent (returns request data and ID
 * via out params) */
int request_queue_get_next_to_send(request_queue_t *queue, char **request_data,
                                   int *req_id);

/* Mark a request as sent (transitions from QUEUED to PENDING) */
int request_queue_mark_sent(request_queue_t *queue, int req_id);

/* Parse JSON-RPC message and extract ID */
int jsonrpc_parse_id(const char *json, size_t len);

/* Check if JSON-RPC message is a request or response */
int jsonrpc_is_response(const char *json, size_t len);

#endif /* REQUEST_QUEUE_H */
