import pytest
from wavedb.exceptions import WaveDBError, NotFoundError, InvalidPathError, IOError_, EncryptionError
from wavedb._errors import map_error, raise_on_error


def test_not_found_from_string():
    err = map_error(0, "NOT_FOUND: users/alice")
    assert isinstance(err, NotFoundError)


def test_invalid_path_from_string():
    err = map_error(0, "INVALID_PATH: empty")
    assert isinstance(err, InvalidPathError)


def test_io_error_from_string():
    err = map_error(0, "IO_ERROR: disk full")
    assert isinstance(err, IOError_)


def test_database_closed_treated_as_io():
    err = map_error(0, "DATABASE_CLOSED")
    assert isinstance(err, IOError_)


def test_encryption_required_from_code():
    err = map_error(-100, "")
    assert isinstance(err, EncryptionError)


def test_encryption_key_invalid_from_code():
    err = map_error(-101, "")
    assert isinstance(err, EncryptionError)


def test_encryption_unsupported_from_code():
    err = map_error(-102, "")
    assert isinstance(err, EncryptionError)


def test_generic_falls_back_to_wavedb_error():
    err = map_error(1, "something else")
    assert isinstance(err, WaveDBError)
    assert not isinstance(err, (NotFoundError, InvalidPathError, IOError_, EncryptionError))


def test_message_preserved():
    err = map_error(0, "NOT_FOUND: users/alice")
    assert "users/alice" in str(err)


def test_raise_on_error_success_does_not_raise():
    # code == 0 -> no exception, regardless of message argument
    raise_on_error(0, "anything")


def test_raise_on_error_callable_path():
    with pytest.raises(NotFoundError):
        raise_on_error(-1, lambda: "NOT_FOUND: x")


def test_raise_on_error_string_path():
    with pytest.raises(WaveDBError):
        raise_on_error(1, "boom")


def test_raise_on_error_does_not_call_factory_on_success():
    called = []

    def factory():
        called.append(True)
        return "should not be used"

    raise_on_error(0, factory)
    assert called == []
