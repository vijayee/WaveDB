#include "path_join.h"


//
// path-join.c
//
// Copyright (c) 2013 Stephen Mathieson
// MIT licensed
//

#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#define PATH_JOIN_SEPERATOR   "\\"
#else
#define PATH_JOIN_SEPERATOR   "/"
#endif

/*
 * Join `dir` with `file`
 */
int str_ends_with(const char *str, const char *end);
int str_starts_with(const char *str, const char *start);

char* path_join(const char *dir, const char *file) {
  int size = strlen(dir) + strlen(file) + 2;
  char *buf = malloc(size * sizeof(char));
  if (NULL == buf) return NULL;

  strcpy(buf, dir);

  // add the sep if necessary
  if (!str_ends_with(dir, PATH_JOIN_SEPERATOR)) {
    strcat(buf, PATH_JOIN_SEPERATOR);
  }

  // remove the sep if necessary
  if (str_starts_with(file, PATH_JOIN_SEPERATOR)) {
    char *filecopy = strdup(file);
    if (NULL == filecopy) {
      free(buf);
      return NULL;
    }
    strcat(buf, ++filecopy);
    free(--filecopy);
  } else {
    strcat(buf, file);
  }

  return buf;
}

int str_ends_with(const char *str, const char *end) {
  int end_len;
  int str_len;

  if (NULL == str || NULL == end) {
    return 0;
  }

  end_len = strlen(end);
  str_len = strlen(str);

  return str_len < end_len
         ? 0
         : !strcmp(str + str_len - end_len, end);
}

int str_starts_with(const char *str, const char *start) {
  for (; ; str++, start++)
    if (!*start) {
      return 1;
    } else if (*str != *start) {
      return 0;
    }
}