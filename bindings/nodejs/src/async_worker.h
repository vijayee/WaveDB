#ifndef WAVEDB_BINDINGS_ASYNC_WORKER_H
#define WAVEDB_BINDINGS_ASYNC_WORKER_H

#include <napi.h>
#include <string>

class WaveDBAsyncWorker : public Napi::AsyncWorker {
public:
  WaveDBAsyncWorker(Napi::Env env, Napi::Function callback)
    : Napi::AsyncWorker(callback),
      deferred_(Napi::Promise::Deferred::New(env)),
      env_(env) {}

  virtual ~WaveDBAsyncWorker() {}

  void OnOK() override;
  void OnError(const Napi::Error& e) override;

  Napi::Promise Promise() const { return deferred_.Promise(); }

protected:
  virtual std::vector<napi_value> GetResult(Napi::Env env) override;

  Napi::Promise::Deferred deferred_;
  Napi::Env env_;
};

#endif // WAVEDB_BINDINGS_ASYNC_WORKER_H