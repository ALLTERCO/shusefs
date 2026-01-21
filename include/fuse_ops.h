#ifndef FUSE_OPS_H
#define FUSE_OPS_H

#include <fuse.h>
#include "device_state.h"
#include "request_queue.h"

/* FUSE context data */
typedef struct {
  device_state_t *dev_state;
  request_queue_t *req_queue;
  struct mg_connection *conn;
  pthread_mutex_t *fuse_mutex;
} fuse_context_data_t;

/* Initialize FUSE operations with device state */
int fuse_ops_init(device_state_t *dev_state, request_queue_t *req_queue,
                  struct mg_connection *conn);

/* Update connection pointer in FUSE context */
void fuse_ops_update_conn(struct mg_connection *conn);

/* Get FUSE operations structure */
struct fuse_operations *fuse_ops_get(void);

/* Start FUSE in a separate thread */
int fuse_start(const char *mountpoint, device_state_t *dev_state,
               request_queue_t *req_queue, struct mg_connection **conn_ptr,
               pthread_t *fuse_thread);

/* Stop FUSE and unmount */
void fuse_stop(const char *mountpoint);

#endif /* FUSE_OPS_H */
