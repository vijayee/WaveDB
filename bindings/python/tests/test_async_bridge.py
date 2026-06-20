import asyncio
import pytest
from wavedb._async import AsyncBridge


@pytest.mark.asyncio
async def test_bridge_resolves(db_path):
    bridge = AsyncBridge()
    fut, op_id, op_id_ptr = bridge.new_future("test")
    bridge.register_pending(fut, op_id, op_id_ptr, "test")
    bridge.schedule_resolve(fut, b"hello")
    result = await fut
    assert result == b"hello"


@pytest.mark.asyncio
async def test_bridge_rejects(db_path):
    bridge = AsyncBridge()
    fut, op_id, op_id_ptr = bridge.new_future("test")
    bridge.register_pending(fut, op_id, op_id_ptr, "test")
    bridge.schedule_reject(fut, RuntimeError("boom"))
    with pytest.raises(RuntimeError, match="boom"):
        await fut


@pytest.mark.asyncio
async def test_bridge_cancel_pending_on_close(db_path):
    bridge = AsyncBridge()
    fut, op_id, op_id_ptr = bridge.new_future("test")
    bridge.register_pending(fut, op_id, op_id_ptr, "test")
    bridge.cancel_all_pending()
    assert fut.done()


@pytest.mark.asyncio
async def test_callback_after_loop_close_is_dropped(db_path):
    bridge = AsyncBridge()
    fut, op_id, op_id_ptr = bridge.new_future("test")
    bridge.register_pending(fut, op_id, op_id_ptr, "test")
    # Simulate callback arriving after loop closure — _deliver must check and drop
    bridge._deliver(fut, b"x", loop_closed=True)
    assert not fut.done()  # silently dropped