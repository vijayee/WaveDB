#include <errno.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int mkdir_p(char* path) {
  int len = strlen(path);
  if (len == 0) {
    return -1;
  } else if (len == 1 && (path[0] == '.' || path[0] == '/')) {
    return 0;
  }

  int ret = 0;
  for (int i = 0; i < 2; ++i) {
    ret = mkdir(path, S_IRWXU);
    if (ret == 0) {
      return 0;
    } else if (errno == EEXIST) {
      struct stat st;
      ret = stat(path, &st);
      if (ret != 0) {
        return ret;
      } else if (S_ISDIR(st.st_mode)) {
        return 0;
      } else {
        return -1;
      }
    } else if (errno != ENOENT) {
      return -1;
    }
    char* copy = strdup(path);
    ret = mkdir_p(dirname(copy));
    free(copy);
    if (ret != 0) {
      return ret;
    }
  }
  return ret;
}