//
// Created by victor on 6/14/25.
//
#include "get_dir.h"
#include "allocator.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>


int sortstring( const void *str1, const void *str2 ){
  char *const *pp1 = str1;
  char *const *pp2 = str2;
  return strcmp(*pp1, *pp2);
}
vec_str_t* get_dir(const char* directory) {
  vec_str_t* files = get_clear_memory(sizeof(vec_char_t));
  vec_init(files);
  vec_reserve(files, 2);

  DIR *dir;
  struct dirent *ent;

  dir = opendir(directory);
  if (dir != NULL) {
    while ((ent = readdir(dir)) != NULL) {
      if (ent->d_type != DT_DIR) {
        char *str = strdup(ent->d_name);
        vec_push(files, str);
      }
    }
    closedir(dir);
    vec_sort(files, sortstring);
    return files;
  } else {
    vec_deinit(files);
    free(files);
    return NULL;
  }
}
void destroy_files(vec_str_t* files) {
  size_t i; char* file;
  vec_foreach(files, file, i) {
      free(file);
  }
  vec_deinit(files);
  free(files);
}