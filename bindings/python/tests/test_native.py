from wavedb._native import ffi, lib


def test_ffi_loaded():
    assert ffi is not None
    assert lib is not None


def test_database_t_declared():
    # database_t is declared opaque (we only hold pointers from C). cffi ABI
    # mode cannot compute sizeof for opaque structs, so verify the type is
    # registered by checking that a function taking database_t* is bound.
    assert lib.database_destroy is not None
    assert lib.database_create_with_config is not None
    # sanity: the destroy function is a real cffi function-pointer object
    assert hasattr(lib.database_destroy, "__call__")


def test_raw_op_t_layout():
    # raw_op_t: key, key_len, value, value_len, type
    assert ffi.sizeof("raw_op_t") >= 40


def test_raw_result_t_layout():
    # raw_result_t: key, key_len, value, value_len
    assert ffi.sizeof("raw_result_t") >= 32