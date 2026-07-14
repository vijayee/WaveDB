/* log_bridge.c -- expose WaveDB's rxi log (src/Util/log.c) to FFI bindings
 * (Python/Dart/Node) without forcing the binding to touch rxi's log_Event /
 * va_list (cffi ABI mode has no C compiler, and va_list is not safely
 * accessible from cffi anyway).
 *
 * wavedb_log_add_callback registers a Python-friendly callback
 * (level:int, message:const char*, udata:void*) that fires synchronously on
 * the calling thread for every log event at or above `level`. The message is
 * formatted HERE in C with vsnprintf (mirroring rxi's stdout_callback) so the
 * binding never sees a va_list.
 *
 * rxi's log_add_callback appends into a fixed MAX_CALLBACKS=32 array and has
 * NO remove -- so the wavedb_log_reg_t we allocate is intentionally leaked for
 * process lifetime. The binding must keep its own cffi callback alive for the
 * process lifetime too (rxi will call a freed function otherwise). The Python
 * log.py module keeps every cffi callback in an immortal list for exactly this
 * reason. */
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

typedef void (*wavedb_log_cb)(int level, const char *message, void *udata);
typedef struct { wavedb_log_cb fn; void *user_udata; } wavedb_log_reg_t;

static void wavedb_log_bridge_cb(log_Event *ev) {
    wavedb_log_reg_t *r = (wavedb_log_reg_t *)ev->udata;
    if (r == NULL || r->fn == NULL) return;
    char buf[1024];
    vsnprintf(buf, sizeof buf, ev->fmt, ev->ap);  /* mirror stdout_callback */
    r->fn(ev->level, buf, r->user_udata);
}

int wavedb_log_add_callback(wavedb_log_cb fn, void *user_udata, int level) {
    if (fn == NULL) return -1;
    wavedb_log_reg_t *r = (wavedb_log_reg_t *)malloc(sizeof *r);
    if (r == NULL) return -1;
    r->fn = fn;
    r->user_udata = user_udata;
    int rc = log_add_callback(wavedb_log_bridge_cb, r, level);
    if (rc != 0) {
        free(r);  /* rxi callback array full (MAX_CALLBACKS 32) */
        return rc;
    }
    /* r is intentionally leaked: rxi has no log_remove_callback. The binding
     * must keep its cffi cb alive for the process lifetime regardless. */
    return 0;
}