#ifndef WAVEDB_BINDINGS_GET_WORKER_H
#define WAVEDB_BINDINGS_GET_WORKER_H

#include <napi.h>
#include "../../../src/Database/database.h"
#include "async_worker.h"
#include "path.h"
#include "identifier.h"

class GetWorker : public WaveDBAsyncWorker {
public:
  GetWorker(Napi::Env env,
            Napi::Function callback,
            database_t* db,
            path_t* path);

  virtual ~GetWorker();

  void Execute() override;

protected:
  std::vector<napi_value> GetResult(Napi::Env env) override;

private:
  database_t* db_;
  path_t* path_;
  identifier_t* result_;
};

#endif // WAVEDB_BINDINGS_GET_WORKER_H