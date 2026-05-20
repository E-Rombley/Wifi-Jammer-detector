python3 -c "
import asyncio
import websockets

async def test():
    async with websockets.connect('ws://192.168.1.140:8073/ws/') as ws:
        for i in range(5):
            msg = await ws.recv()
            if isinstance(msg, bytes):
                print(msg[:20].hex())
            else:
                print(repr(msg[:100]))

asyncio.run(test())
"