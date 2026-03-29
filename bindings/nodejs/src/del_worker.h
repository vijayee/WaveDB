#ifndef WAVEDB_BINDINGS_DEL_WORKER_H
#define WAVEDB_BINDINGS_DEL_WORKER_H

#include <napi.h>
#include "../../../src/Database/database.h"
#include "async_worker.h"
#include "path.h"

class DelWorker : public WaveDBAsyncWorker {
public:
  DelWorker(Napi::Env env,
            Napi::Function callback,
            database_t* db,
            path_t* path);

  virtual ~DelWorker();

  void Execute() override;

protected:
  std::vector<napi_value> GetResult(Napi::Env env) override;

private:
  database_t* db_;
  path_t* path_;
};

#endif // WAVEDB_BINDINGS_DEL_WORKER_H