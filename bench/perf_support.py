import asyncio
import time
from pathlib import Path


def read_credentials(path, count):
    values = [line.strip() for line in Path(path).read_text().splitlines() if line.strip()]
    if len(values) != count or len(set(values)) != count:
        raise ValueError("credentials must be distinct and match the requested count")
    return values


async def wait_start(path, warmup, timeout):
    start = time.monotonic() + warmup
    deadline = start + timeout
    if path:
        while not Path(path).exists():
            if time.monotonic() >= deadline:
                raise TimeoutError("start-file timed out")
            await asyncio.sleep(0.02)
        start = int(Path(path).read_text()) / 1e9
        if not time.monotonic() < start <= deadline:
            raise ValueError("start-file time is outside readiness deadline")
    await asyncio.sleep(max(0, start - time.monotonic()))
    return start
