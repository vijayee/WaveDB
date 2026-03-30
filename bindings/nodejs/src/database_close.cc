Napi::Value WaveDB::Close(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  fprintf(stderr, "DEBUG: Close() called\n");
  fflush(stderr);
  usleep(100000);  // Give time for output to flush

  if (db_) {
    // DEBUG
    fprintf(stderr, "DEBUG: Close() - db_ is not null\n");
    fflush(stderr);
    usleep(100000);

    // Wait for all references to be released
    // This ensures all async operations complete before we destroy
    uint32_t count = refcounter_count((refcounter_t*)db_);
    fprintf(stderr, "DEBUG: Initial refcount = %u\n", count);
    fflush(stderr);
    usleep(100000);

    uint32_t max_wait_ms = 5000;  // Max 5 seconds
    uint32_t waited_ms = 0;
    while (count > 1 && waited_ms < max_wait_ms) {  // > 1 because we still hold a reference
      usleep(1000);  // Wait 1ms
      count = refcounter_count((refcounter_t*)db_);
      waited_ms += 1;
    }

    fprintf(stderr, "DEBUG: Waited %u ms, final refcount = %u\n", waited_ms, count);
    fflush(stderr);
    usleep(100000);

    // Note: Never call database_snapshot() from Node.js bindings.
    // Thread-local WAL architecture means async workers create WAL state in their threads,
    // which is not accessible from the main thread. Calling snapshot would crash.
    // Data persists via WAL recovery on next database open.
    // database_snapshot(db_);

    database_t* db = db_;
    db_ = nullptr;  // Clear pointer first to prevent double-destroy

    // DEBUG: Log before destroy
    fprintf(stderr, "DEBUG: About to call database_destroy\n");
    fflush(stderr);
    usleep(100000);

    database_destroy(db);  // This just dereferences, actual destruction happens when all refs are gone

    // DEBUG: Log after destroy
    fprintf(stderr, "DEBUG: database_destroy returned\n");
    fflush(stderr);
    usleep(100000);
  }
  fprintf(stderr, "DEBUG: Close() returning\n");
  fflush(stderr);
  usleep(100000);

  return env.Undefined();
}