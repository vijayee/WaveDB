#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define S_IRWXU 0
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)

static char* dirname_win(char* path) {
  char* last_slash = strrchr(path, '/');
  char* last_backslash = strrchr(path, '\\');
  char* last_sep = last_slash > last_backslash ? last_slash : last_backslash;
  if (last_sep == NULL) return ".";
  if (last_sep == path) return "/";
  *last_sep = '\0';
  return path;
}
#define dirname(path) dirname_win(path)
#else
#include <libgen.h>
#endif

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