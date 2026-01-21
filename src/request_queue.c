#include "../include/request_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/mongoose.h"

int request_queue_init(request_queue_t *queue) {
  if (!queue) {
    return -1;
  }

  memset(queue, 0, sizeof(request_queue_t));

  if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
    return -1;
  }

  for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
    queue->entries[i].id = -1;
    queue->entries[i].state = REQ_STATE_PENDING;
    queue->entries[i].request_data = NULL;
    queue->entries[i].response_data = NULL;
    if (pthread_cond_init(&queue->entries[i].cond, NULL) != 0) {
      pthread_mutex_destroy(&queue->mutex);
      return -1;
    }
  }

  queue->next_id = 1;
  return 0;
}

void request_queue_destroy(request_queue_t *queue) {
  if (!queue) {
    return;
  }

  pthread_mutex_lock(&queue->mutex);

  for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
    if (queue->entries[i].request_data) {
      free(queue->entries[i].request_data);
    }
    if (queue->entries[i].response_data) {
      free(queue->entries[i].response_data);
    }
    pthread_cond_destroy(&queue->entries[i].cond);
  }

  pthread_mutex_unlock(&queue->mutex);
  pthread_mutex_destroy(&queue->mutex);
}

int request_queue_peek_next_id(request_queue_t *queue) {
  if (!queue) {
    return -1;
  }

  pthread_mutex_lock(&queue->mutex);
  int next_id = queue->next_id;
  pthread_mutex_unlock(&queue->mutex);

  return next_id;
}

int request_queue_add(request_queue_t *queue, const char *request_data) {
  if (!queue || !request_data) {
    return -1;
  }

  pthread_mutex_lock(&queue->mutex);

  int slot = -1;
  for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
    if (queue->entries[i].id == -1) {
      slot = i;
      break;
    }
  }

  if (slot == -1) {
    pthread_mutex_unlock(&queue->mutex);
    fprintf(stderr, "Error: Request queue is full\n");
    return -1;
  }

  int req_id = queue->next_id++;
  queue->entries[slot].id = req_id;
  queue->entries[slot].state =
      REQ_STATE_QUEUED; /* Request is queued, not yet sent */
  queue->entries[slot].request_data = strdup(request_data);
  queue->entries[slot].response_data = NULL;
  queue->entries[slot].timestamp = time(NULL);

  pthread_mutex_unlock(&queue->mutex);

  return req_id;
}

int request_queue_handle_response(request_queue_t *queue, int req_id,
                                  const char *response_data) {
  if (!queue || !response_data) {
    return -1;
  }

  pthread_mutex_lock(&queue->mutex);

  int slot = -1;
  for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
    if (queue->entries[i].id == req_id &&
        queue->entries[i].state == REQ_STATE_PENDING) {
      slot = i;
      break;
    }
  }

  if (slot == -1) {
    pthread_mutex_unlock(&queue->mutex);
    return -1;
  }

  queue->entries[slot].response_data = strdup(response_data);
  queue->entries[slot].state = REQ_STATE_COMPLETED;
  pthread_cond_signal(&queue->entries[slot].cond);

  pthread_mutex_unlock(&queue->mutex);
  return 0;
}

void request_queue_handle_unsolicited(const char *msg, size_t len,
                                      unsolicited_msg_handler_t handler,
                                      void *user_data) {
  if (!msg || !handler) {
    return;
  }

  handler(msg, len, user_data);
}

void request_queue_cleanup_timeouts(request_queue_t *queue) {
  if (!queue) {
    return;
  }

  pthread_mutex_lock(&queue->mutex);

  time_t now = time(NULL);

  for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
    if (queue->entries[i].id != -1 &&
        queue->entries[i].state == REQ_STATE_PENDING &&
        (now - queue->entries[i].timestamp) > REQUEST_TIMEOUT_SEC) {
      fprintf(stderr, "Request %d timed out\n", queue->entries[i].id);
      queue->entries[i].state = REQ_STATE_TIMEOUT;
      pthread_cond_signal(&queue->entries[i].cond);
    }
  }

  pthread_mutex_unlock(&queue->mutex);
}

const char *request_queue_get_request_data(request_queue_t *queue, int req_id) {
  if (!queue) {
    return NULL;
  }

  pthread_mutex_lock(&queue->mutex);

  const char *result = NULL;
  for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
    if (queue->entries[i].id == req_id) {
      result = queue->entries[i].request_data;
      break;
    }
  }

  pthread_mutex_unlock(&queue->mutex);
  return result;
}

int request_queue_get_next_to_send(request_queue_t *queue, char **request_data,
                                   int *req_id) {
  if (!queue || !request_data || !req_id) {
    return -1;
  }

  pthread_mutex_lock(&queue->mutex);

  /* Find the first queued request */
  for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
    if (queue->entries[i].id != -1 &&
        queue->entries[i].state == REQ_STATE_QUEUED) {
      *req_id = queue->entries[i].id;
      *request_data = queue->entries[i].request_data;
      pthread_mutex_unlock(&queue->mutex);
      return 0;
    }
  }

  pthread_mutex_unlock(&queue->mutex);
  return -1; /* No queued requests */
}

int request_queue_mark_sent(request_queue_t *queue, int req_id) {
  if (!queue) {
    return -1;
  }

  pthread_mutex_lock(&queue->mutex);

  /* Find the request and mark it as sent */
  for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
    if (queue->entries[i].id == req_id) {
      if (queue->entries[i].state == REQ_STATE_QUEUED) {
        queue->entries[i].state = REQ_STATE_PENDING;
        queue->entries[i].timestamp =
            time(NULL); /* Reset timestamp for timeout tracking */
        pthread_mutex_unlock(&queue->mutex);
        return 0;
      } else {
        pthread_mutex_unlock(&queue->mutex);
        return -1; /* Request not in QUEUED state */
      }
    }
  }

  pthread_mutex_unlock(&queue->mutex);
  return -1; /* Request not found */
}

int jsonrpc_parse_id(const char *json, size_t len) {
  if (!json || len == 0) {
    return -1;
  }

  double id_val = 0;
  if (mg_json_get_num(mg_str_n(json, len), "$.id", &id_val)) {
    return (int) id_val;
  }

  return -1;
}

int jsonrpc_is_response(const char *json, size_t len) {
  if (!json || len == 0) {
    printf("Not a response definitely\n");
    return 0;
  }

  struct mg_str json_str = mg_str_n(json, len);

  /* Check if $.result or $.error exists (returns non-negative if found) */
  int result_pos = mg_json_get(json_str, "$.result", NULL);
  int error_pos = mg_json_get(json_str, "$.error", NULL);

  /* Response if either result or error exists */
  int is_resp = (result_pos >= 0 || error_pos >= 0) ? 1 : 0;

  return is_resp;
}
