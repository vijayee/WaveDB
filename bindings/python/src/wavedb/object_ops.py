"""Flatten nested dicts into batch put ops, and reconstruct them from a scan."""
from __future__ import annotations

from typing import Any


def flatten_object(key: "str | list[str] | None", obj: Any, delimiter: str) -> list[dict]:
    """Returns a list of {'type': 'put', 'key': list[str], 'value': bytes} ops."""
    base: list[str] = []
    if key is not None:
        if isinstance(key, str):
            base = [p for p in key.split(delimiter) if p]
        elif isinstance(key, list):
            base = [str(p) for p in key]

    ops: list[dict] = []
    stack: list[tuple[Any, list[str]]] = [(obj, list(base))]

    while stack:
        value, path = stack.pop()
        if isinstance(value, dict):
            for k in reversed(list(value.keys())):
                stack.append((value[k], path + [str(k)]))
        elif isinstance(value, list):
            for i in range(len(value) - 1, -1, -1):
                stack.append((value[i], path + [str(i)]))
        elif isinstance(value, (bytes, bytearray)):
            ops.append({"type": "put", "key": path, "value": bytes(value)})
        elif isinstance(value, str):
            ops.append({"type": "put", "key": path, "value": value.encode("utf-8")})
        elif value is None or isinstance(value, (int, float, bool)):
            ops.append({"type": "put", "key": path, "value": str(value).encode("utf-8")})
        else:
            raise TypeError(f"unsupported leaf type: {type(value).__name__}")

    return ops


def reconstruct_object(prefix: "str | list[str]", kvs: list[tuple[str, bytes]], delimiter: str) -> dict:
    """Given a scan of (key_str, value_bytes) tuples, rebuild the nested dict."""
    base: list[str] = []
    if isinstance(prefix, str):
        base = [p for p in prefix.split(delimiter) if p]
    elif isinstance(prefix, list):
        base = [str(p) for p in prefix]

    result: dict = {}
    for key_str, value in kvs:
        parts = [p for p in key_str.split(delimiter) if p]
        # Strip the base prefix
        if base:
            if parts[: len(base)] != base:
                continue
            parts = parts[len(base):]
        if not parts:
            continue
        cursor = result
        for idx, p in enumerate(parts[:-1]):
            next_part = parts[idx + 1]
            cursor = cursor.setdefault(p, {} if not _is_int(next_part) else [])
        last = parts[-1]
        if _is_int(last):
            idx = int(last)
            while len(cursor) <= idx:
                cursor.append(None)
            cursor[idx] = value
        else:
            cursor[last] = value
    return result


def _is_int(s: str) -> bool:
    try:
        int(s)
        return True
    except ValueError:
        return False