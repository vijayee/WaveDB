#ifndef WAVEDB_BINDINGS_ITERATOR_H
#define WAVEDB_BINDINGS_ITERATOR_H

#include <napi.h>
#include <string>
#include "../../../src/Database/database.h"

class Iterator : public Napi::ObjectWrap<Iterator> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

  Iterator(const Napi::CallbackInfo& info);
  ~Iterator();

  static Napi::FunctionReference constructor_;

private:
  Napi::Value Read(const Napi::CallbackInfo& info);
  Napi::Value End(const Napi::CallbackInfo& info);

  Napi::ObjectReference databaseRef_;  // Keeps JS WaveDB object alive
  database_t* db_;
  std::string start_;
  std::string end_;
  bool reverse_;
  bool keys_;
  bool values_;
  bool keyAsArray_;
  char delimiter_;
  bool ended_;
  database_iterator_t* scan_handle_;  // Native scan iterator
};

#endif // WAVEDB_BINDINGS_ITERATOR_H