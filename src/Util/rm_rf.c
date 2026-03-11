//
// Created by victor on 7/30/25.
//
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "allocator.h"
#include "log.h"
#include "rm_rf.h"


int rm_rf(const char *path) {
  DIR *d = opendir(path);
  size_t path_len = strlen(path);
  int r = -1; // Return value, -1 for error, 0 for success

  if (!d) {
    log_error("failed to open directory");
    return -1;
  }

  struct dirent *p;
  r = 0; // Assume success initially

  while ((p = readdir(d)) != NULL) {
    // Skip "." and ".."
    if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, "..")) {
      continue;
    }

    char *buf = get_memory(path_len + strlen(p->d_name) + 2); // +2 for '/' and null terminator
    if (!buf) {
      log_error("failed to allocate buffer");
      r = -1;
      break;
    }
    sprintf(buf, "%s/%s", path, p->d_name);

    struct stat statbuf;
    if (stat(buf, &statbuf) == -1) {
      log_error("failed to stat");
      free(buf);
      r = -1;
      break;
    }

    if (S_ISDIR(statbuf.st_mode)) {
      // Recursively remove subdirectory
      if (rm_rf(buf) == -1) {
        r = -1;
        free(buf);
        break;
      }
    } else {
      // Remove file
      if (unlink(buf) == -1) {
        log_error("failed to unlink");
        r = -1;
        free(buf);
        break;
      }
    }
    free(buf);
  }

  if (closedir(d) == -1) {
    log_error("failed to close directory");
    r = -1;
  }

  // Remove the now-empty directory
  if (r == 0 && rmdir(path) == -1) {
    log_error("failed to remove directory");
    r = -1;
  }

  return r;
}
