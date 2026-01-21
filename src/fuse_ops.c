/*
 * FUSE_USE_VERSION is defined by the Makefile:
 * - Linux: FUSE_USE_VERSION=31 (libfuse3)
 * - macOS: FUSE_USE_VERSION=29 (macFUSE/FUSE-T compatibility)
 */
#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif

#include "../include/fuse_ops.h"
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../include/device_state.h"
#include "../include/mongoose.h"

/*
 * macOS/FUSE compatibility layer
 * FUSE3 (Linux) has additional parameters that older versions don't have
 */
#if FUSE_USE_VERSION < 30
/* For older FUSE versions (macOS with macFUSE/FUSE-T) */
#define SHUSE_READDIR_FLAGS_PARAM
#define SHUSE_READDIR_UNUSED_FLAGS
/* fuse_fill_dir_t has 4 args in FUSE2: buf, name, stbuf, off */
#define FUSE_FILL_DIR(filler, buf, name) filler(buf, name, NULL, 0)
#else
/* FUSE3 (Linux) has the flags parameter */
#define SHUSE_READDIR_FLAGS_PARAM , enum fuse_readdir_flags flags
#define SHUSE_READDIR_UNUSED_FLAGS (void) flags;
/* fuse_fill_dir_t has 5 args in FUSE3: buf, name, stbuf, off, flags */
#define FUSE_FILL_DIR(filler, buf, name) filler(buf, name, NULL, 0, 0)
#endif

/* Global FUSE context */
static fuse_context_data_t g_fuse_ctx = {0};

/* Global FUSE handle for fuse_exit() */
static struct fuse *g_fuse_handle = NULL;

/* Write buffer for file modifications */
typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} write_buffer_t;

/* File handle context for tracking writes */
typedef struct {
  write_buffer_t *buffer;
  int script_id; /* For script files, -1 otherwise */
  int switch_id; /* For switch config files, -1 otherwise */
  int input_id;  /* For input config files, -1 otherwise */
} file_handle_t;

/* Helper function to get FUSE context */
static fuse_context_data_t *get_fuse_ctx(void) {
  return &g_fuse_ctx;
}

/* Create write buffer */
static write_buffer_t *write_buffer_create(size_t initial_capacity) {
  write_buffer_t *buf = malloc(sizeof(write_buffer_t));
  if (!buf) {
    return NULL;
  }

  buf->data = malloc(initial_capacity);
  if (!buf->data) {
    free(buf);
    return NULL;
  }

  buf->size = 0;
  buf->capacity = initial_capacity;
  return buf;
}

/* Destroy write buffer */
static void write_buffer_destroy(write_buffer_t *buf) {
  if (!buf) {
    return;
  }

  if (buf->data) {
    free(buf->data);
  }
  free(buf);
}

/* Resize write buffer if needed */
static int write_buffer_ensure_capacity(write_buffer_t *buf, size_t needed) {
  if (buf->capacity >= needed) {
    return 0;
  }

  size_t new_capacity = buf->capacity * 2;
  while (new_capacity < needed) {
    new_capacity *= 2;
  }

  char *new_data = realloc(buf->data, new_capacity);
  if (!new_data) {
    return -1;
  }

  buf->data = new_data;
  buf->capacity = new_capacity;
  return 0;
}

/* Parse script ID from filename (e.g., "script_0.js" -> 0) */
static int parse_script_id(const char *name) {
  if (strncmp(name, "script_", 7) != 0) {
    return -1;
  }

  const char *num_str = name + 7;
  char *endptr;
  long id = strtol(num_str, &endptr, 10);

  if (endptr == num_str || (*endptr != '\0' && strcmp(endptr, ".js") != 0)) {
    return -1;
  }

  if (id < 0 || id >= MAX_SCRIPTS) {
    return -1;
  }

  return (int) id;
}

/* Parse switch ID from filename (e.g., "switch_0_config.json" -> 0) */
static int parse_switch_id(const char *name) {
  if (strncmp(name, "switch_", 7) != 0) {
    return -1;
  }

  const char *num_str = name + 7;
  char *endptr;
  long id = strtol(num_str, &endptr, 10);

  if (endptr == num_str || strcmp(endptr, "_config.json") != 0) {
    return -1;
  }

  if (id < 0 || id >= MAX_SWITCHES) {
    return -1;
  }

  return (int) id;
}

/* Parse input ID from filename (e.g., "input_0_config.json" -> 0) */
static int parse_input_id(const char *name) {
  if (strncmp(name, "input_", 6) != 0) {
    return -1;
  }

  const char *num_str = name + 6;
  char *endptr;
  long id = strtol(num_str, &endptr, 10);

  if (endptr == num_str || strcmp(endptr, "_config.json") != 0) {
    return -1;
  }

  if (id < 0 || id >= MAX_INPUTS) {
    return -1;
  }

  return (int) id;
}

/* Get file attributes */
static int shuse_getattr(const char *path, struct stat *stbuf,
                         struct fuse_file_info *fi) {
  (void) fi;

  fuse_context_data_t *ctx = get_fuse_ctx();
  memset(stbuf, 0, sizeof(struct stat));

  /* Get FUSE context for ownership */
  struct fuse_context *fuse_ctx = fuse_get_context();

  /* Root directory */
  if (strcmp(path, "/") == 0) {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
    stbuf->st_uid = fuse_ctx->uid;
    stbuf->st_gid = fuse_ctx->gid;
    return 0;
  }

  /* scripts directory */
  if (strcmp(path, "/scripts") == 0) {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
    stbuf->st_uid = fuse_ctx->uid;
    stbuf->st_gid = fuse_ctx->gid;
    return 0;
  }

  /* proc directory */
  if (strcmp(path, "/proc") == 0) {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
    stbuf->st_uid = fuse_ctx->uid;
    stbuf->st_gid = fuse_ctx->gid;
    return 0;
  }

  /* proc/switch directory */
  if (strcmp(path, "/proc/switch") == 0) {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
    stbuf->st_uid = fuse_ctx->uid;
    stbuf->st_gid = fuse_ctx->gid;
    return 0;
  }

  /* Check for proc/switch/N directories */
  if (strncmp(path, "/proc/switch/", 13) == 0) {
    const char *remainder = path + 13;
    char *endptr;
    long switch_id = strtol(remainder, &endptr, 10);

    if (endptr != remainder && switch_id >= 0 && switch_id < MAX_SWITCHES) {
      switch_config_t *sw =
          device_state_get_switch(ctx->dev_state, (int) switch_id);

      /* Check if it's the directory itself or a file within it */
      if (*endptr == '\0') {
        /* It's the directory /proc/switch/N */
        if (sw && sw->valid) {
          stbuf->st_mode = S_IFDIR | 0755;
          stbuf->st_nlink = 2;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          return 0;
        }
      } else if (strcmp(endptr, "/output") == 0) {
        /* output file - read/write */
        if (sw && sw->valid) {
          stbuf->st_mode = S_IFREG | 0664;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 6; /* "true\n" or "false\n" */
          stbuf->st_mtime = sw->status.mtime_output;
          return 0;
        }
      } else if (strcmp(endptr, "/id") == 0) {
        if (sw && sw->valid) {
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 32;
          stbuf->st_mtime = sw->status.mtime_id;
          return 0;
        }
      } else if (strcmp(endptr, "/source") == 0) {
        if (sw && sw->valid) {
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 32;
          stbuf->st_mtime = sw->status.mtime_source;
          return 0;
        }
      } else if (strcmp(endptr, "/apower") == 0) {
        if (sw && sw->valid) {
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 32;
          stbuf->st_mtime = sw->status.mtime_apower;
          return 0;
        }
      } else if (strcmp(endptr, "/voltage") == 0) {
        if (sw && sw->valid) {
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 32;
          stbuf->st_mtime = sw->status.mtime_voltage;
          return 0;
        }
      } else if (strcmp(endptr, "/current") == 0) {
        if (sw && sw->valid) {
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 32;
          stbuf->st_mtime = sw->status.mtime_current;
          return 0;
        }
      } else if (strcmp(endptr, "/freq") == 0) {
        if (sw && sw->valid) {
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 32;
          stbuf->st_mtime = sw->status.mtime_freq;
          return 0;
        }
      } else if (strcmp(endptr, "/energy") == 0) {
        if (sw && sw->valid) {
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 32;
          stbuf->st_mtime = sw->status.mtime_energy;
          return 0;
        }
      } else if (strcmp(endptr, "/ret_energy") == 0) {
        if (sw && sw->valid) {
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 32;
          stbuf->st_mtime = sw->status.mtime_ret_energy;
          return 0;
        }
      } else if (strcmp(endptr, "/temperature") == 0) {
        if (sw && sw->valid) {
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 32;
          stbuf->st_mtime = sw->status.mtime_temperature;
          return 0;
        }
      }
    }
  }

  /* proc/input directory */
  if (strcmp(path, "/proc/input") == 0) {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
    stbuf->st_uid = fuse_ctx->uid;
    stbuf->st_gid = fuse_ctx->gid;
    return 0;
  }

  /* Check for proc/input/N directories */
  if (strncmp(path, "/proc/input/", 12) == 0) {
    const char *remainder = path + 12;
    char *endptr;
    long input_id = strtol(remainder, &endptr, 10);

    if (endptr != remainder && input_id >= 0 && input_id < MAX_INPUTS) {
      input_config_t *inp =
          device_state_get_input(ctx->dev_state, (int) input_id);

      /* Check if it's the directory itself or a file within it */
      if (*endptr == '\0') {
        /* It's the directory /proc/input/N */
        if (inp && inp->valid) {
          stbuf->st_mode = S_IFDIR | 0755;
          stbuf->st_nlink = 2;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          return 0;
        }
      } else if (strcmp(endptr, "/id") == 0) {
        if (inp && inp->valid) {
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 32;
          stbuf->st_mtime = inp->status.mtime_id;
          return 0;
        }
      } else if (strcmp(endptr, "/state") == 0) {
        if (inp && inp->valid) {
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_uid = fuse_ctx->uid;
          stbuf->st_gid = fuse_ctx->gid;
          stbuf->st_size = 6; /* "true\n" or "false\n" */
          stbuf->st_mtime = inp->status.mtime_state;
          return 0;
        }
      }
    }
  }

  /* sys_config.json */
  if (strcmp(path, "/sys_config.json") == 0) {
    stbuf->st_mode = S_IFREG | 0644;
    stbuf->st_nlink = 1;
    stbuf->st_uid = fuse_ctx->uid;
    stbuf->st_gid = fuse_ctx->gid;
    if (ctx->dev_state->sys_config.valid &&
        ctx->dev_state->sys_config.raw_json) {
      stbuf->st_size = ctx->dev_state->sys_config.json_len;
      stbuf->st_mtime = ctx->dev_state->sys_config.last_update;
    } else {
      stbuf->st_size = 0;
    }
    return 0;
  }

  /* mqtt_config.json */
  if (strcmp(path, "/mqtt_config.json") == 0) {
    stbuf->st_mode = S_IFREG | 0664; /* Read/write for owner and group */
    stbuf->st_nlink = 1;
    stbuf->st_uid = fuse_ctx->uid;
    stbuf->st_gid = fuse_ctx->gid;
    if (ctx->dev_state->mqtt_config.valid &&
        ctx->dev_state->mqtt_config.raw_json) {
      stbuf->st_size = ctx->dev_state->mqtt_config.json_len;
      stbuf->st_mtime = ctx->dev_state->mqtt_config.last_update;
    } else {
      stbuf->st_size = 0;
    }
    return 0;
  }

  /* crontab file */
  if (strcmp(path, "/crontab") == 0) {
    stbuf->st_mode = S_IFREG | 0644;
    stbuf->st_nlink = 1;
    stbuf->st_uid = fuse_ctx->uid;
    stbuf->st_gid = fuse_ctx->gid;

    /* Calculate size by generating content */
    char *content = NULL;
    if (device_state_get_crontab_str(ctx->dev_state, &content) == 0 &&
        content) {
      stbuf->st_size = strlen(content);
      stbuf->st_mtime = ctx->dev_state->schedules.last_update;
      free(content);
    } else {
      stbuf->st_size = 0;
    }
    return 0;
  }

  /* Check for switch config files (switch_N_config.json) */
  if (strncmp(path, "/switch_", 8) == 0) {
    const char *filename = path + 1;
    int switch_id = parse_switch_id(filename);

    if (switch_id >= 0) {
      switch_config_t *sw = device_state_get_switch(ctx->dev_state, switch_id);
      if (sw && sw->valid) {
        stbuf->st_mode = S_IFREG | 0664; /* Read/write for owner and group */
        stbuf->st_nlink = 1;
        stbuf->st_uid = fuse_ctx->uid;
        stbuf->st_gid = fuse_ctx->gid;
        if (sw->raw_json) {
          stbuf->st_size = sw->json_len;
        } else {
          stbuf->st_size = 0;
        }
        stbuf->st_mtime = sw->last_update;
        return 0;
      }
    }
  }

  /* Check for input config files (input_N_config.json) */
  if (strncmp(path, "/input_", 7) == 0) {
    const char *filename = path + 1;
    int input_id = parse_input_id(filename);

    if (input_id >= 0) {
      input_config_t *inp = device_state_get_input(ctx->dev_state, input_id);
      if (inp && inp->valid) {
        stbuf->st_mode = S_IFREG | 0664; /* Read/write for owner and group */
        stbuf->st_nlink = 1;
        stbuf->st_uid = fuse_ctx->uid;
        stbuf->st_gid = fuse_ctx->gid;
        if (inp->raw_json) {
          stbuf->st_size = inp->json_len;
        } else {
          stbuf->st_size = 0;
        }
        stbuf->st_mtime = inp->last_update;
        return 0;
      }
    }
  }

  /* Check for script files in /scripts/ directory */
  if (strncmp(path, "/scripts/", 9) == 0) {
    const char *filename = path + 9;
    int script_id = parse_script_id(filename);

    if (script_id >= 0) {
      script_entry_t *script =
          device_state_get_script(ctx->dev_state, script_id);
      if (script && script->valid) {
        stbuf->st_mode = S_IFREG | 0664; /* Read/write for owner and group */
        stbuf->st_nlink = 1;
        stbuf->st_uid = fuse_ctx->uid;
        stbuf->st_gid = fuse_ctx->gid;
        if (script->code) {
          stbuf->st_size = strlen(script->code);
        } else {
          stbuf->st_size = 0;
        }
        stbuf->st_mtime = script->modify_time;
        return 0;
      }
    }
  }

  return -ENOENT;
}

/* Read directory contents */
static int shuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset,
                         struct fuse_file_info *fi SHUSE_READDIR_FLAGS_PARAM) {
  (void) offset;
  (void) fi;
  SHUSE_READDIR_UNUSED_FLAGS

  fuse_context_data_t *ctx = get_fuse_ctx();

  /* Root directory */
  if (strcmp(path, "/") == 0) {
    FUSE_FILL_DIR(filler, buf, ".");
    FUSE_FILL_DIR(filler, buf, "..");
    FUSE_FILL_DIR(filler, buf, "scripts");
    FUSE_FILL_DIR(filler, buf, "proc");
    FUSE_FILL_DIR(filler, buf, "sys_config.json");
    FUSE_FILL_DIR(filler, buf, "mqtt_config.json");
    FUSE_FILL_DIR(filler, buf, "crontab");

    /* List all valid switch config files */
    for (int i = 0; i < MAX_SWITCHES; i++) {
      switch_config_t *sw = device_state_get_switch(ctx->dev_state, i);
      if (sw && sw->valid) {
        char filename[64];
        snprintf(filename, sizeof(filename), "switch_%d_config.json", i);
        FUSE_FILL_DIR(filler, buf, filename);
      }
    }

    /* List all valid input config files */
    for (int i = 0; i < MAX_INPUTS; i++) {
      input_config_t *inp = device_state_get_input(ctx->dev_state, i);
      if (inp && inp->valid) {
        char filename[64];
        snprintf(filename, sizeof(filename), "input_%d_config.json", i);
        FUSE_FILL_DIR(filler, buf, filename);
      }
    }
    return 0;
  }

  /* proc directory */
  if (strcmp(path, "/proc") == 0) {
    FUSE_FILL_DIR(filler, buf, ".");
    FUSE_FILL_DIR(filler, buf, "..");
    FUSE_FILL_DIR(filler, buf, "switch");
    FUSE_FILL_DIR(filler, buf, "input");
    return 0;
  }

  /* proc/switch directory */
  if (strcmp(path, "/proc/switch") == 0) {
    FUSE_FILL_DIR(filler, buf, ".");
    FUSE_FILL_DIR(filler, buf, "..");

    /* List all valid switch directories */
    for (int i = 0; i < MAX_SWITCHES; i++) {
      switch_config_t *sw = device_state_get_switch(ctx->dev_state, i);
      if (sw && sw->valid) {
        char dirname[8];
        snprintf(dirname, sizeof(dirname), "%d", i);
        FUSE_FILL_DIR(filler, buf, dirname);
      }
    }
    return 0;
  }

  /* proc/switch/N directories */
  if (strncmp(path, "/proc/switch/", 13) == 0) {
    const char *remainder = path + 13;
    char *endptr;
    long switch_id = strtol(remainder, &endptr, 10);

    if (endptr != remainder && *endptr == '\0' && switch_id >= 0 &&
        switch_id < MAX_SWITCHES) {
      switch_config_t *sw =
          device_state_get_switch(ctx->dev_state, (int) switch_id);
      if (sw && sw->valid) {
        FUSE_FILL_DIR(filler, buf, ".");
        FUSE_FILL_DIR(filler, buf, "..");
        FUSE_FILL_DIR(filler, buf, "output");
        FUSE_FILL_DIR(filler, buf, "id");
        FUSE_FILL_DIR(filler, buf, "source");
        FUSE_FILL_DIR(filler, buf, "apower");
        FUSE_FILL_DIR(filler, buf, "voltage");
        FUSE_FILL_DIR(filler, buf, "current");
        FUSE_FILL_DIR(filler, buf, "freq");
        FUSE_FILL_DIR(filler, buf, "energy");
        FUSE_FILL_DIR(filler, buf, "ret_energy");
        FUSE_FILL_DIR(filler, buf, "temperature");
        return 0;
      }
    }
  }

  /* proc/input directory */
  if (strcmp(path, "/proc/input") == 0) {
    FUSE_FILL_DIR(filler, buf, ".");
    FUSE_FILL_DIR(filler, buf, "..");

    /* List all valid input directories */
    for (int i = 0; i < MAX_INPUTS; i++) {
      input_config_t *inp = device_state_get_input(ctx->dev_state, i);
      if (inp && inp->valid) {
        char dirname[8];
        snprintf(dirname, sizeof(dirname), "%d", i);
        FUSE_FILL_DIR(filler, buf, dirname);
      }
    }
    return 0;
  }

  /* proc/input/N directories */
  if (strncmp(path, "/proc/input/", 12) == 0) {
    const char *remainder = path + 12;
    char *endptr;
    long input_id = strtol(remainder, &endptr, 10);

    if (endptr != remainder && *endptr == '\0' && input_id >= 0 &&
        input_id < MAX_INPUTS) {
      input_config_t *inp =
          device_state_get_input(ctx->dev_state, (int) input_id);
      if (inp && inp->valid) {
        FUSE_FILL_DIR(filler, buf, ".");
        FUSE_FILL_DIR(filler, buf, "..");
        FUSE_FILL_DIR(filler, buf, "id");
        FUSE_FILL_DIR(filler, buf, "state");
        return 0;
      }
    }
  }

  /* scripts directory */
  if (strcmp(path, "/scripts") == 0) {
    FUSE_FILL_DIR(filler, buf, ".");
    FUSE_FILL_DIR(filler, buf, "..");

    /* List all valid scripts */
    for (int i = 0; i < MAX_SCRIPTS; i++) {
      script_entry_t *script = device_state_get_script(ctx->dev_state, i);
      if (script && script->valid) {
        char filename[64];
        snprintf(filename, sizeof(filename), "script_%d.js", i);
        FUSE_FILL_DIR(filler, buf, filename);
      }
    }
    return 0;
  }

  return -ENOENT;
}

/* Open file */
static int shuse_open(const char *path, struct fuse_file_info *fi) {
  fuse_context_data_t *ctx = get_fuse_ctx();

  /* Check if it's a config file */
  if (strcmp(path, "/sys_config.json") == 0) {
    if (!ctx->dev_state->sys_config.valid) {
      return -ENOENT;
    }

    /* If opening for writing, allocate a write buffer */
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
      write_buffer_t *buf = write_buffer_create(MAX_CONFIG_SIZE);
      if (!buf) {
        return -ENOMEM;
      }

      /* Copy current config to buffer for modification */
      if (ctx->dev_state->sys_config.raw_json) {
        size_t len = ctx->dev_state->sys_config.json_len;
        if (write_buffer_ensure_capacity(buf, len + 1) == 0) {
          memcpy(buf->data, ctx->dev_state->sys_config.raw_json, len);
          buf->data[len] = '\0';
          buf->size = len;
        }
      }

      file_handle_t *fh = malloc(sizeof(file_handle_t));
      if (!fh) {
        write_buffer_destroy(buf);
        return -ENOMEM;
      }
      fh->buffer = buf;
      fh->script_id = -1;
      fh->switch_id = -1;
      fh->input_id = -1;

      fi->fh = (uint64_t) (uintptr_t) fh;
    }
    return 0;
  }

  if (strcmp(path, "/mqtt_config.json") == 0) {
    if (!ctx->dev_state->mqtt_config.valid) {
      return -ENOENT;
    }

    /* If opening for writing, allocate a write buffer */
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
      write_buffer_t *buf = write_buffer_create(MAX_CONFIG_SIZE);
      if (!buf) {
        return -ENOMEM;
      }

      /* Copy current config to buffer for modification */
      if (ctx->dev_state->mqtt_config.raw_json) {
        size_t len = ctx->dev_state->mqtt_config.json_len;
        if (write_buffer_ensure_capacity(buf, len + 1) == 0) {
          memcpy(buf->data, ctx->dev_state->mqtt_config.raw_json, len);
          buf->data[len] = '\0';
          buf->size = len;
        }
      }

      file_handle_t *fh = malloc(sizeof(file_handle_t));
      if (!fh) {
        write_buffer_destroy(buf);
        return -ENOMEM;
      }
      fh->buffer = buf;
      fh->script_id = -1;
      fh->switch_id = -1;
      fh->input_id = -1;

      fi->fh = (uint64_t) (uintptr_t) fh;
    }
    return 0;
  }

  /* crontab file */
  if (strcmp(path, "/crontab") == 0) {
    /* If opening for writing, allocate a write buffer */
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
      write_buffer_t *buf = write_buffer_create(MAX_CONFIG_SIZE);
      if (!buf) {
        return -ENOMEM;
      }

      /* Copy current crontab content to buffer for modification,
       * but NOT if O_TRUNC is set (user wants to replace entire file) */
      if (!(fi->flags & O_TRUNC)) {
        char *content = NULL;
        if (device_state_get_crontab_str(ctx->dev_state, &content) == 0 &&
            content) {
          size_t len = strlen(content);
          if (write_buffer_ensure_capacity(buf, len + 1) == 0) {
            memcpy(buf->data, content, len);
            buf->data[len] = '\0';
            buf->size = len;
          }
          free(content);
        }
      }

      file_handle_t *fh = malloc(sizeof(file_handle_t));
      if (!fh) {
        write_buffer_destroy(buf);
        return -ENOMEM;
      }
      fh->buffer = buf;
      fh->script_id = -1;
      fh->switch_id = -1;
      fh->input_id = -1;

      fi->fh = (uint64_t) (uintptr_t) fh;
    }
    return 0;
  }

  /* Check if it's a switch config file */
  if (strncmp(path, "/switch_", 8) == 0) {
    const char *filename = path + 1;
    int switch_id = parse_switch_id(filename);

    if (switch_id >= 0) {
      switch_config_t *sw = device_state_get_switch(ctx->dev_state, switch_id);
      if (sw && sw->valid) {
        /* If opening for writing, allocate a write buffer */
        if ((fi->flags & O_ACCMODE) != O_RDONLY) {
          write_buffer_t *buf = write_buffer_create(MAX_CONFIG_SIZE);
          if (!buf) {
            return -ENOMEM;
          }

          /* Copy current config to buffer for modification */
          if (sw->raw_json) {
            size_t len = sw->json_len;
            if (write_buffer_ensure_capacity(buf, len + 1) == 0) {
              memcpy(buf->data, sw->raw_json, len);
              buf->data[len] = '\0';
              buf->size = len;
            }
          }

          file_handle_t *fh = malloc(sizeof(file_handle_t));
          if (!fh) {
            write_buffer_destroy(buf);
            return -ENOMEM;
          }
          fh->buffer = buf;
          fh->script_id = -1;
          fh->switch_id = switch_id;
          fh->input_id = -1;

          fi->fh = (uint64_t) (uintptr_t) fh;
        }
        return 0;
      }
    }
  }

  /* Check if it's an input config file */
  if (strncmp(path, "/input_", 7) == 0) {
    const char *filename = path + 1;
    int input_id = parse_input_id(filename);

    if (input_id >= 0) {
      input_config_t *inp = device_state_get_input(ctx->dev_state, input_id);
      if (inp && inp->valid) {
        /* If opening for writing, allocate a write buffer */
        if ((fi->flags & O_ACCMODE) != O_RDONLY) {
          write_buffer_t *buf = write_buffer_create(MAX_CONFIG_SIZE);
          if (!buf) {
            return -ENOMEM;
          }

          /* Copy current config to buffer for modification */
          if (inp->raw_json) {
            size_t len = inp->json_len;
            if (write_buffer_ensure_capacity(buf, len + 1) == 0) {
              memcpy(buf->data, inp->raw_json, len);
              buf->data[len] = '\0';
              buf->size = len;
            }
          }

          file_handle_t *fh = malloc(sizeof(file_handle_t));
          if (!fh) {
            write_buffer_destroy(buf);
            return -ENOMEM;
          }
          fh->buffer = buf;
          fh->script_id = -1;
          fh->switch_id = -1;
          fh->input_id = input_id;

          fi->fh = (uint64_t) (uintptr_t) fh;
        }
        return 0;
      }
    }
  }

  /* Check if it's a script file */
  if (strncmp(path, "/scripts/", 9) == 0) {
    const char *filename = path + 9;
    int script_id = parse_script_id(filename);

    if (script_id >= 0) {
      script_entry_t *script =
          device_state_get_script(ctx->dev_state, script_id);
      if (script && script->valid) {
        /* If opening for writing, allocate a write buffer */
        if ((fi->flags & O_ACCMODE) != O_RDONLY) {
          write_buffer_t *buf = write_buffer_create(MAX_SCRIPT_CODE);
          if (!buf) {
            return -ENOMEM;
          }

          /* Copy current script code to buffer for modification */
          if (script->code) {
            size_t len = strlen(script->code);
            if (write_buffer_ensure_capacity(buf, len + 1) == 0) {
              memcpy(buf->data, script->code, len);
              buf->data[len] = '\0';
              buf->size = len;
            }
          }

          file_handle_t *fh = malloc(sizeof(file_handle_t));
          if (!fh) {
            write_buffer_destroy(buf);
            return -ENOMEM;
          }
          fh->buffer = buf;
          fh->script_id = script_id;
          fh->switch_id = -1;
          fh->input_id = -1;

          fi->fh = (uint64_t) (uintptr_t) fh;
        }
        return 0;
      }
    }
  }

  /* Check if it's a proc switch file */
  if (strncmp(path, "/proc/switch/", 13) == 0) {
    const char *remainder = path + 13;
    char *endptr;
    long switch_id = strtol(remainder, &endptr, 10);

    if (endptr != remainder && switch_id >= 0 && switch_id < MAX_SWITCHES) {
      /* Check for valid proc file names */
      if (strcmp(endptr, "/output") == 0 || strcmp(endptr, "/id") == 0 ||
          strcmp(endptr, "/source") == 0 || strcmp(endptr, "/apower") == 0 ||
          strcmp(endptr, "/voltage") == 0 || strcmp(endptr, "/current") == 0 ||
          strcmp(endptr, "/freq") == 0 || strcmp(endptr, "/energy") == 0 ||
          strcmp(endptr, "/ret_energy") == 0 ||
          strcmp(endptr, "/temperature") == 0) {
        switch_config_t *sw =
            device_state_get_switch(ctx->dev_state, (int) switch_id);
        if (sw && sw->valid) {
          /* Proc files don't need write buffers - writes are handled
           * immediately */
          return 0;
        }
      }
    }
  }

  /* Check if it's a proc input file */
  if (strncmp(path, "/proc/input/", 12) == 0) {
    const char *remainder = path + 12;
    char *endptr;
    long input_id = strtol(remainder, &endptr, 10);

    if (endptr != remainder && input_id >= 0 && input_id < MAX_INPUTS) {
      /* Check for valid proc file names */
      if (strcmp(endptr, "/id") == 0 || strcmp(endptr, "/state") == 0) {
        input_config_t *inp =
            device_state_get_input(ctx->dev_state, (int) input_id);
        if (inp && inp->valid) {
          /* Proc files don't need write buffers - reads only */
          return 0;
        }
      }
    }
  }

  return -ENOENT;
}

/* Read file contents */
static int shuse_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
  (void) fi;

  fuse_context_data_t *ctx = get_fuse_ctx();

  /* Read sys_config.json */
  if (strcmp(path, "/sys_config.json") == 0) {
    if (!ctx->dev_state->sys_config.valid ||
        !ctx->dev_state->sys_config.raw_json) {
      return -ENOENT;
    }

    size_t len = ctx->dev_state->sys_config.json_len;
    if (offset >= (off_t) len) {
      return 0;
    }

    if (offset + size > len) {
      size = len - offset;
    }

    memcpy(buf, ctx->dev_state->sys_config.raw_json + offset, size);
    return size;
  }

  /* Read mqtt_config.json */
  if (strcmp(path, "/mqtt_config.json") == 0) {
    if (!ctx->dev_state->mqtt_config.valid ||
        !ctx->dev_state->mqtt_config.raw_json) {
      return -ENOENT;
    }

    size_t len = ctx->dev_state->mqtt_config.json_len;
    if (offset >= (off_t) len) {
      return 0;
    }

    if (offset + size > len) {
      size = len - offset;
    }

    memcpy(buf, ctx->dev_state->mqtt_config.raw_json + offset, size);
    return size;
  }

  /* Read crontab */
  if (strcmp(path, "/crontab") == 0) {
    char *content = NULL;
    if (device_state_get_crontab_str(ctx->dev_state, &content) != 0 ||
        !content) {
      return -EIO;
    }

    size_t len = strlen(content);
    if (offset >= (off_t) len) {
      free(content);
      return 0;
    }

    if (offset + size > len) {
      size = len - offset;
    }

    memcpy(buf, content + offset, size);
    free(content);
    return size;
  }

  /* Read switch config file */
  if (strncmp(path, "/switch_", 8) == 0) {
    const char *filename = path + 1;
    int switch_id = parse_switch_id(filename);

    if (switch_id >= 0) {
      switch_config_t *sw = device_state_get_switch(ctx->dev_state, switch_id);
      if (sw && sw->valid && sw->raw_json) {
        size_t len = sw->json_len;
        if (offset >= (off_t) len) {
          return 0;
        }

        if (offset + size > len) {
          size = len - offset;
        }

        memcpy(buf, sw->raw_json + offset, size);
        return size;
      }
    }
  }

  /* Read script file */
  if (strncmp(path, "/scripts/", 9) == 0) {
    const char *filename = path + 9;
    int script_id = parse_script_id(filename);

    if (script_id >= 0) {
      script_entry_t *script =
          device_state_get_script(ctx->dev_state, script_id);
      if (script && script->valid && script->code) {
        size_t len = strlen(script->code);
        if (offset >= (off_t) len) {
          return 0;
        }

        if (offset + size > len) {
          size = len - offset;
        }

        memcpy(buf, script->code + offset, size);
        return size;
      }
    }
  }

  /* Read proc/switch/N/ files */
  if (strncmp(path, "/proc/switch/", 13) == 0) {
    const char *remainder = path + 13;
    char *endptr;
    long switch_id = strtol(remainder, &endptr, 10);

    if (endptr != remainder && switch_id >= 0 && switch_id < MAX_SWITCHES) {
      switch_config_t *sw =
          device_state_get_switch(ctx->dev_state, (int) switch_id);
      if (sw && sw->valid) {
        char value_str[64];
        value_str[0] = '\0';

        /* Determine which file is being read */
        if (strcmp(endptr, "/output") == 0) {
          snprintf(value_str, sizeof(value_str), "%s\n",
                   sw->status.output ? "true" : "false");
        } else if (strcmp(endptr, "/id") == 0) {
          snprintf(value_str, sizeof(value_str), "%d\n", sw->status.id);
        } else if (strcmp(endptr, "/source") == 0) {
          snprintf(value_str, sizeof(value_str), "%s\n", sw->status.source);
        } else if (strcmp(endptr, "/apower") == 0) {
          snprintf(value_str, sizeof(value_str), "%.1f\n", sw->status.apower);
        } else if (strcmp(endptr, "/voltage") == 0) {
          snprintf(value_str, sizeof(value_str), "%.1f\n", sw->status.voltage);
        } else if (strcmp(endptr, "/current") == 0) {
          snprintf(value_str, sizeof(value_str), "%.3f\n", sw->status.current);
        } else if (strcmp(endptr, "/freq") == 0) {
          snprintf(value_str, sizeof(value_str), "%.1f\n", sw->status.freq);
        } else if (strcmp(endptr, "/energy") == 0) {
          snprintf(value_str, sizeof(value_str), "%.3f\n",
                   sw->status.energy_total);
        } else if (strcmp(endptr, "/ret_energy") == 0) {
          snprintf(value_str, sizeof(value_str), "%.3f\n",
                   sw->status.ret_energy_total);
        } else if (strcmp(endptr, "/temperature") == 0) {
          snprintf(value_str, sizeof(value_str), "%.1f\n",
                   sw->status.temperature_c);
        } else {
          return -ENOENT;
        }

        size_t len = strlen(value_str);
        if (offset >= (off_t) len) {
          return 0;
        }

        if (offset + size > len) {
          size = len - offset;
        }

        memcpy(buf, value_str + offset, size);
        return size;
      }
    }
  }

  /* Read input config file */
  if (strncmp(path, "/input_", 7) == 0) {
    const char *filename = path + 1;
    int input_id = parse_input_id(filename);

    if (input_id >= 0) {
      input_config_t *inp = device_state_get_input(ctx->dev_state, input_id);
      if (inp && inp->valid && inp->raw_json) {
        size_t len = inp->json_len;
        if (offset >= (off_t) len) {
          return 0;
        }

        if (offset + size > len) {
          size = len - offset;
        }

        memcpy(buf, inp->raw_json + offset, size);
        return size;
      }
    }
  }

  /* Read proc/input/N/ files */
  if (strncmp(path, "/proc/input/", 12) == 0) {
    const char *remainder = path + 12;
    char *endptr;
    long input_id = strtol(remainder, &endptr, 10);

    if (endptr != remainder && input_id >= 0 && input_id < MAX_INPUTS) {
      input_config_t *inp =
          device_state_get_input(ctx->dev_state, (int) input_id);
      if (inp && inp->valid) {
        char value_str[64];
        value_str[0] = '\0';

        /* Determine which file is being read */
        if (strcmp(endptr, "/id") == 0) {
          snprintf(value_str, sizeof(value_str), "%d\n", inp->status.id);
        } else if (strcmp(endptr, "/state") == 0) {
          snprintf(value_str, sizeof(value_str), "%s\n",
                   inp->status.state ? "true" : "false");
        } else {
          return -ENOENT;
        }

        size_t len = strlen(value_str);
        if (offset >= (off_t) len) {
          return 0;
        }

        if (offset + size > len) {
          size = len - offset;
        }

        memcpy(buf, value_str + offset, size);
        return size;
      }
    }
  }

  return -ENOENT;
}

/* Write file contents */
static int shuse_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi) {
  fuse_context_data_t *ctx = get_fuse_ctx();

  /* Handle proc/switch/N/output file writes - immediate action, no buffering */
  if (strncmp(path, "/proc/switch/", 13) == 0) {
    const char *remainder = path + 13;
    char *endptr;
    long switch_id = strtol(remainder, &endptr, 10);

    if (endptr != remainder && strcmp(endptr, "/output") == 0 &&
        switch_id >= 0 && switch_id < MAX_SWITCHES) {
      switch_config_t *sw =
          device_state_get_switch(ctx->dev_state, (int) switch_id);
      if (sw && sw->valid) {
        /* Parse the input - accept "true"/"false" or "1"/"0" */
        if (size > 0) {
          bool turn_on = false;
          if (size >= 4 && strncmp(buf, "true", 4) == 0) {
            turn_on = true;
          } else if (size >= 1 && buf[0] == '1') {
            turn_on = true;
          }

          /* Send Switch.Set command to device */
          int req_id =
              device_state_set_switch(ctx->dev_state, ctx->req_queue, ctx->conn,
                                      (int) switch_id, turn_on);
          if (req_id < 0) {
            fprintf(stderr, "Failed to set switch %d state\n", (int) switch_id);
            return -EIO;
          }

          /* Request status update immediately after */
          device_state_request_switch_status(ctx->dev_state, ctx->req_queue,
                                             ctx->conn, (int) switch_id);

          return size;
        }
        return -EINVAL;
      }
      return -ENOENT;
    }
  }

  file_handle_t *fh = (file_handle_t *) (uintptr_t) fi->fh;
  if (!fh || !fh->buffer) {
    return -EBADF;
  }

  write_buffer_t *wbuf = fh->buffer;

  /* Handle O_APPEND - write at end regardless of offset */
  if (fi->flags & O_APPEND) {
    offset = wbuf->size;
  }

  /* Ensure buffer has enough capacity */
  if (write_buffer_ensure_capacity(wbuf, offset + size + 1) != 0) {
    return -ENOMEM;
  }

  /* Write data to buffer */
  memcpy(wbuf->data + offset, buf, size);

  /* Update size if we wrote beyond current end */
  if ((size_t) (offset + size) > wbuf->size) {
    wbuf->size = offset + size;
    wbuf->data[wbuf->size] = '\0';
  }

  return size;
}

/* Truncate file */
static int shuse_truncate(const char *path, off_t size,
                          struct fuse_file_info *fi) {
  (void) path;

  if (fi && fi->fh) {
    file_handle_t *fh = (file_handle_t *) (uintptr_t) fi->fh;
    if (fh && fh->buffer) {
      write_buffer_t *wbuf = fh->buffer;

      if (size == 0) {
        /* Clear buffer */
        wbuf->size = 0;
        if (wbuf->data) {
          wbuf->data[0] = '\0';
        }
      } else if ((size_t) size < wbuf->size) {
        /* Truncate to smaller size */
        wbuf->size = size;
        wbuf->data[size] = '\0';
      }
      /* If size > current size, do nothing (file will be extended on write) */
    }
    return 0;
  }

  /* For files opened read-only */
  if (strcmp(path, "/sys_config.json") == 0) {
    return 0;
  }

  if (strcmp(path, "/mqtt_config.json") == 0) {
    return 0;
  }

  if (strcmp(path, "/crontab") == 0) {
    return 0;
  }

  if (strncmp(path, "/scripts/", 9) == 0) {
    return 0;
  }

  return -ENOENT;
}

/* Flush file - sync data to device */
static int shuse_flush(const char *path, struct fuse_file_info *fi) {
  fuse_context_data_t *ctx = get_fuse_ctx();

  if (!fi || !fi->fh) {
    return 0;
  }

  file_handle_t *fh = (file_handle_t *) (uintptr_t) fi->fh;
  if (!fh || !fh->buffer) {
    return 0;
  }

  write_buffer_t *wbuf = fh->buffer;

  /* Flush sys_config.json */
  if (strcmp(path, "/sys_config.json") == 0) {
    if (wbuf->size > 0) {
      printf("Flushing sys_config.json to device (%zu bytes)\n", wbuf->size);

      /* Validate JSON before sending */
      struct mg_str json_str = mg_str_n(wbuf->data, wbuf->size);
      int dummy_len = 0;
      if (mg_json_get(json_str, "$", &dummy_len) < 0) {
        fprintf(stderr, "Error: Invalid JSON in sys_config.json\n");
        return -EINVAL;
      }

      /* Send to device without updating local state yet.
       * On success response, auto-refresh will update state with canonical
       * device data. On error response, local state remains unchanged
       * (preserves original content). */
      if (ctx->conn) {
        int ret = device_state_set_sys_config_from_json(
            wbuf->data, ctx->req_queue, ctx->conn);
        if (ret < 0) {
          fprintf(stderr, "Error: Failed to send sys_config to device\n");
          return -EIO;
        }
        printf("sys_config.json write queued (request ID: %d)\n", ret);
        printf("Waiting for device response...\n");
      } else {
        fprintf(stderr, "Error: Not connected to device\n");
        return -EIO;
      }
    }
    return 0;
  }

  /* Flush mqtt_config.json */
  if (strcmp(path, "/mqtt_config.json") == 0) {
    if (wbuf->size > 0) {
      printf("Flushing mqtt_config.json to device (%zu bytes)\n", wbuf->size);

      /* Validate JSON before sending */
      struct mg_str json_str = mg_str_n(wbuf->data, wbuf->size);
      int dummy_len = 0;
      if (mg_json_get(json_str, "$", &dummy_len) < 0) {
        fprintf(stderr, "Error: Invalid JSON in mqtt_config.json\n");
        return -EINVAL;
      }

      /* Send to device without updating local state yet.
       * On success response, auto-refresh will update state with canonical
       * device data. On error response, local state remains unchanged
       * (preserves original content). */
      if (ctx->conn) {
        int ret = device_state_set_mqtt_config_from_json(
            wbuf->data, ctx->req_queue, ctx->conn);
        if (ret < 0) {
          fprintf(stderr, "Error: Failed to send mqtt_config to device\n");
          return -EIO;
        }
        printf("mqtt_config.json write queued (request ID: %d)\n", ret);
        printf("Waiting for device response...\n");
      } else {
        fprintf(stderr, "Error: Not connected to device\n");
        return -EIO;
      }
    }
    return 0;
  }

  /* Flush switch config files */
  if (strncmp(path, "/switch_", 8) == 0) {
    if (fh->switch_id >= 0 && wbuf->size > 0) {
      printf("Flushing switch_%d_config.json to device (%zu bytes)\n",
             fh->switch_id, wbuf->size);

      /* Validate JSON before sending */
      struct mg_str json_str = mg_str_n(wbuf->data, wbuf->size);
      int dummy_len = 0;
      if (mg_json_get(json_str, "$", &dummy_len) < 0) {
        fprintf(stderr, "Error: Invalid JSON in switch_%d_config.json\n",
                fh->switch_id);
        return -EINVAL;
      }

      /* Send to device without updating local state yet.
       * On success response, auto-refresh will update state with canonical
       * device data. On error response, local state remains unchanged
       * (preserves original content). */
      if (ctx->conn) {
        int ret = device_state_set_switch_config_from_json(
            wbuf->data, ctx->req_queue, ctx->conn, fh->switch_id);
        if (ret < 0) {
          fprintf(stderr, "Error: Failed to send switch_%d_config to device\n",
                  fh->switch_id);
          return -EIO;
        }
        printf("switch_%d_config.json write queued (request ID: %d)\n",
               fh->switch_id, ret);
        printf("Waiting for device response...\n");
      } else {
        fprintf(stderr, "Error: Not connected to device\n");
        return -EIO;
      }
    }
    return 0;
  }

  /* Flush input config files */
  if (strncmp(path, "/input_", 7) == 0) {
    if (fh->input_id >= 0 && wbuf->size > 0) {
      printf("Flushing input_%d_config.json to device (%zu bytes)\n",
             fh->input_id, wbuf->size);

      /* Validate JSON before sending */
      struct mg_str json_str = mg_str_n(wbuf->data, wbuf->size);
      int dummy_len = 0;
      if (mg_json_get(json_str, "$", &dummy_len) < 0) {
        fprintf(stderr, "Error: Invalid JSON in input_%d_config.json\n",
                fh->input_id);
        return -EINVAL;
      }

      /* Send to device without updating local state yet.
       * On success response, auto-refresh will update state with canonical
       * device data. On error response, local state remains unchanged
       * (preserves original content). */
      if (ctx->conn) {
        int ret = device_state_set_input_config_from_json(
            wbuf->data, ctx->req_queue, ctx->conn, fh->input_id);
        if (ret < 0) {
          fprintf(stderr, "Error: Failed to send input_%d_config to device\n",
                  fh->input_id);
          return -EIO;
        }
        printf("input_%d_config.json write queued (request ID: %d)\n",
               fh->input_id, ret);
        printf("Waiting for device response...\n");
      } else {
        fprintf(stderr, "Error: Not connected to device\n");
        return -EIO;
      }
    }
    return 0;
  }

  /* Flush script files */
  if (strncmp(path, "/scripts/", 9) == 0) {
    if (fh->script_id >= 0 && wbuf->size > 0) {
      printf("Flushing script %d to device (%zu bytes)\n", fh->script_id,
             wbuf->size);

      /* Send to device in chunks */
      if (ctx->conn) {
        int ret =
            device_state_put_script_code(ctx->dev_state, ctx->req_queue,
                                         ctx->conn, fh->script_id, wbuf->data);
        if (ret < 0) {
          fprintf(stderr, "Error: Failed to send script %d to device\n",
                  fh->script_id);
          return -EIO;
        }
        printf("Script %d synced to device (last request ID: %d)\n",
               fh->script_id, ret);
      }
    }
    return 0;
  }

  /* Flush crontab */
  if (strcmp(path, "/crontab") == 0) {
    if (wbuf->size > 0) {
      printf("Flushing crontab to device (%zu bytes)\n", wbuf->size);

      if (ctx->conn) {
        int ret = device_state_sync_crontab(ctx->dev_state, ctx->req_queue,
                                            ctx->conn, wbuf->data, wbuf->size);
        if (ret < 0) {
          fprintf(stderr, "Error: Failed to sync crontab to device\n");
          return -EIO;
        }
        if (ret > 0) {
          printf("crontab write queued (%d operations)\n", ret);
          printf("Waiting for device response...\n");
        } else {
          printf("crontab unchanged, no operations needed\n");
        }
      } else {
        fprintf(stderr, "Error: Not connected to device\n");
        return -EIO;
      }
    }
    return 0;
  }

  return 0;
}

/* Release file - cleanup */
static int shuse_release(const char *path, struct fuse_file_info *fi) {
  /* Release sys_config.json */
  if (strcmp(path, "/sys_config.json") == 0) {
    if (fi && fi->fh) {
      file_handle_t *fh = (file_handle_t *) (uintptr_t) fi->fh;
      if (fh) {
        if (fh->buffer) {
          write_buffer_destroy(fh->buffer);
        }
        free(fh);
      }
      fi->fh = 0;
    }
    return 0;
  }

  /* Release mqtt_config.json */
  if (strcmp(path, "/mqtt_config.json") == 0) {
    if (fi && fi->fh) {
      file_handle_t *fh = (file_handle_t *) (uintptr_t) fi->fh;
      if (fh) {
        if (fh->buffer) {
          write_buffer_destroy(fh->buffer);
        }
        free(fh);
      }
      fi->fh = 0;
    }
    return 0;
  }

  /* Release crontab */
  if (strcmp(path, "/crontab") == 0) {
    if (fi && fi->fh) {
      file_handle_t *fh = (file_handle_t *) (uintptr_t) fi->fh;
      if (fh) {
        if (fh->buffer) {
          write_buffer_destroy(fh->buffer);
        }
        free(fh);
      }
      fi->fh = 0;
    }
    return 0;
  }

  /* Release switch config files */
  if (strncmp(path, "/switch_", 8) == 0) {
    if (fi && fi->fh) {
      file_handle_t *fh = (file_handle_t *) (uintptr_t) fi->fh;
      if (fh) {
        if (fh->buffer) {
          write_buffer_destroy(fh->buffer);
        }
        free(fh);
      }
      fi->fh = 0;
    }
    return 0;
  }

  /* Release script files */
  if (strncmp(path, "/scripts/", 9) == 0) {
    if (fi && fi->fh) {
      file_handle_t *fh = (file_handle_t *) (uintptr_t) fi->fh;
      if (fh) {
        if (fh->buffer) {
          write_buffer_destroy(fh->buffer);
        }
        free(fh);
      }
      fi->fh = 0;
    }
    return 0;
  }

  return 0;
}

/* FUSE operations structure */
static struct fuse_operations shuse_oper = {
    .getattr = shuse_getattr,
    .readdir = shuse_readdir,
    .open = shuse_open,
    .read = shuse_read,
    .write = shuse_write,
    .truncate = shuse_truncate,
    .flush = shuse_flush,
    .release = shuse_release,
};

/* Initialize FUSE operations with device state */
int fuse_ops_init(device_state_t *dev_state, request_queue_t *req_queue,
                  struct mg_connection *conn) {
  if (!dev_state || !req_queue) {
    return -1;
  }

  g_fuse_ctx.dev_state = dev_state;
  g_fuse_ctx.req_queue = req_queue;
  g_fuse_ctx.conn = conn;

  return 0;
}

/* Update connection pointer in FUSE context */
void fuse_ops_update_conn(struct mg_connection *conn) {
  g_fuse_ctx.conn = conn;
}

/* Get FUSE operations structure */
struct fuse_operations *fuse_ops_get(void) {
  return &shuse_oper;
}

/* Thread function for FUSE */
static void *fuse_thread_func(void *arg) {
  char **fuse_argv = (char **) arg;
  int fuse_argc = 0;

  /* Count arguments */
  while (fuse_argv[fuse_argc] != NULL) {
    fuse_argc++;
  }

  /* Extract mountpoint (last non-option argument) */
  const char *mountpoint = fuse_argv[fuse_argc - 1];

  /* Create FUSE arguments - only pass program name and options (not mountpoint)
   */
  struct fuse_args args = FUSE_ARGS_INIT(fuse_argc - 1, fuse_argv);

  g_fuse_handle = fuse_new(&args, &shuse_oper, sizeof(shuse_oper), NULL);
  if (!g_fuse_handle) {
    fprintf(stderr, "Error: Failed to create FUSE handle\n");
    for (int i = 0; fuse_argv[i] != NULL; i++) {
      free(fuse_argv[i]);
    }
    free(fuse_argv);
    return (void *) (long) -1;
  }

  /* Mount the filesystem */
  if (fuse_mount(g_fuse_handle, mountpoint) != 0) {
    fprintf(stderr, "Error: Failed to mount FUSE filesystem at %s\n",
            mountpoint);
    fuse_destroy(g_fuse_handle);
    g_fuse_handle = NULL;
    for (int i = 0; fuse_argv[i] != NULL; i++) {
      free(fuse_argv[i]);
    }
    free(fuse_argv);
    return (void *) (long) -1;
  }

  printf("FUSE filesystem mounted at %s\n", mountpoint);

  /* Run FUSE event loop (single-threaded) */
  int ret = fuse_loop(g_fuse_handle);

  printf("FUSE loop exited with code %d\n", ret);

  /* Cleanup */
  fuse_unmount(g_fuse_handle);
  fuse_destroy(g_fuse_handle);
  g_fuse_handle = NULL;

  /* Free argv */
  for (int i = 0; fuse_argv[i] != NULL; i++) {
    free(fuse_argv[i]);
  }
  free(fuse_argv);

  return (void *) (long) ret;
}

/* Start FUSE in a separate thread */
int fuse_start(const char *mountpoint, device_state_t *dev_state,
               request_queue_t *req_queue, struct mg_connection **conn_ptr,
               pthread_t *fuse_thread) {
  if (!mountpoint || !dev_state || !req_queue || !fuse_thread) {
    return -1;
  }

  /* Initialize FUSE context */
  if (fuse_ops_init(dev_state, req_queue, *conn_ptr) != 0) {
    return -1;
  }

  /* Receive pre-built FUSE arguments from caller */
  char **fuse_argv = (char **) mountpoint;

  /* Create FUSE thread */
  if (pthread_create(fuse_thread, NULL, fuse_thread_func, fuse_argv) != 0) {
    return -1;
  }

  /* Give FUSE time to initialize */
  sleep(1);

  return 0;
}

/* Stop FUSE and unmount */
void fuse_stop(const char *mountpoint) {
  (void) mountpoint; /* Not needed when using fuse_exit() */

  /* Signal FUSE loop to exit */
  if (g_fuse_handle) {
    printf("Signaling FUSE to exit...\n");
    fuse_exit(g_fuse_handle);
  }
}
