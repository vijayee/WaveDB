from wavedb._native import ffi, lib


def test_ffi_loaded():
    assert ffi is not None
    assert lib is not None


def test_database_t_declared():
    # Exercise the cdef->symbol->call path with a side-effect-free function.
    cfg = lib.database_config_default()
    try:
        assert cfg != ffi.NULL
    finally:
        lib.database_config_destroy(cfg)


def test_raw_op_t_layout():
    # raw_op_t: key, key_len, value, value_len, type
    assert ffi.sizeof("raw_op_t") >= 40


def test_raw_result_t_layout():
    # raw_result_t: key, key_len, value, value_len
    assert ffi.sizeof("raw_result_t") >= 32
