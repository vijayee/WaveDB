#include <napi.h>
#include <string>

class WaveDBAsyncWorker : public Napi::AsyncWorker {
public:
  WaveDBAsyncWorker(Napi::Env env, Napi::Function callback)
    : Napi::AsyncWorker(callback),
      deferred_(Napi::Promise::Deferred::New(env)),
      env_(env) {}

  virtual ~WaveDBAsyncWorker() {}

  void OnOK() override {
    Napi::HandleScope scope(Env());

    try {
      // Get result vector and use first element if available
      std::vector<napi_value> results = GetResult(Env());
      Napi::Value result = results.empty() ? Env().Undefined() : Napi::Value(Env(), results[0]);

      // Call callback if provided
      if (!Callback().IsEmpty()) {
        Callback().Call({ Env().Null(), result });
      }

      // Resolve promise
      deferred_.Resolve(result);
    } catch (const std::exception& e) {
      // Handle any exceptions during result retrieval
      Napi::Error error = Napi::Error::New(Env(), e.what());
      OnError(error);
      return;
    }
  }

  void OnError(const Napi::Error& e) override {
    Napi::HandleScope scope(Env());

    // Create error object with code
    std::string code = "UNKNOWN";
    std::string message = e.Message();
    std::string fullError = e.what();

    // Find first colon+space to avoid splitting on colons in messages
    size_t colonPos = fullError.find(": ");
    if (colonPos != std::string::npos && colonPos > 0) {
      code = fullError.substr(0, colonPos);
      message = fullError.substr(colonPos + 2);  // Skip ": "
    }

    Napi::Object errorObj = Napi::Error::New(Env(), message).Value();
    errorObj.Set("code", Napi::String::New(Env(), code));

    // Call callback if provided
    if (!Callback().IsEmpty()) {
      Callback().Call({ errorObj, Env().Undefined() });
    }

    // Reject promise
    deferred_.Reject(errorObj);
  }

protected:
  virtual std::vector<napi_value> GetResult(Napi::Env env) override {
    return { env.Null() };
  }

  Napi::Promise::Deferred deferred_;
  Napi::Env env_;
};