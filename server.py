import asyncio
import websockets
import threading
import time
import logging
from collections import defaultdict
from websockets.legacy.server import WebSocketServerProtocol as ws_type
import signal
import os

logging.basicConfig(level=logging.INFO)
host_websocket = {}
uuid_f = lambda: "123"
do_it = True


async def read(ws: ws_type):
    return await ws.recv()


async def send(ws: ws_type, msg):
    return await ws.send(msg)


async def handler(websocket: ws_type, path):
    logging.info("Connection established")
    message = await read(websocket)
    if message.startswith("client"):
        host_id = str(message[6:])
        if host_id in host_websocket:
            await send(websocket, "ok")
            await proxy(
                host_websocket[host_id], websocket, host_id
            ), asyncio.get_event_loop()
        else:
            await send(websocket, "bad")
    if message == "host":
        host_id = uuid_f()
        host_websocket[host_id] = websocket
        await websocket.send(host_id)
        await asyncio.Event().wait()  # https://stackoverflow.com/a/67322757


async def proxy(host: ws_type, client: ws_type, host_id):
    try:
        while True:
            if not client.open:
                break
            command = await read(client)
            if command.startswith("read"):
                await send(host, command)  # read
                await send(host, await read(client))  # off
                await send(host, await read(client))  # size
                ans = await read(host)
                if ans[0] == ord("0"):
                    await send(client, ans)
                else:
                    #  1fhash|total_chunks|chunk_buf а потом всегда 2fhash|cur_chunk|chunk_buf
                    #  1   10     5         etc
                    await send(client, ans)  # 1fhash
                    # fhash = conv(ans[1:11])
                    total_chunks = int(ans[11:16])
                    used = [False for i in range(total_chunks)]
                    for i in range(1, total_chunks):
                        data = await read(host)
                        chunk = int(data[11:16])
                        assert chunk < total_chunks
                        assert not used[chunk]
                        used[chunk] = True
                        await send(client, data)

            elif command.startswith("lookup") or command == "ls":
                await send(host, command)
                ans = await read(host)
                await send(client, ans)
    except websockets.exceptions.ConnectionClosedError as e:
        pass


def thost():
    while do_it:
        d = []
        for host_id in host_websocket:
            host = host_websocket[host_id]
            if not host.open:
                d.append(host_id)

        for host_id in d:
            del host_websocket[host_id]
        time.sleep(1)


async def main(host, port):
    K = 30
    server = await websockets.serve(
        handler,
        host,
        port,
        max_size=2**K,
        read_limit=2**K,
        write_limit=2**K,
        ping_interval=None,
    )
    await server.wait_closed()


def signal_handler(signal, frame):
    global do_it
    do_it = False
    loop = asyncio.get_event_loop()
    loop.call_soon_threadsafe(loop.stop)


def signal_kill():
    os.kill(os.getpid(), signal.SIGINT)


if __name__ == "__main__":
    signal.signal(signal.SIGINT, signal_handler)

    from sys import argv

    if len(argv) == 2 and argv[1] == "--pytest":
        host, port = "localhost", 8765
    else:
        import uuid

        uuid_f = lambda: str(uuid.uuid4().hex)
        host, port = "0.0.0.0", 8000  # TODO read cfg
    th = threading.Thread(target=thost)
    th.start()
    loop = asyncio.get_event_loop()
    try:
        loop.run_until_complete(main(host, port))
    except RuntimeError as e:
        if str(e) != "Event loop stopped before Future completed.":
            signal_kill()
    except IOError as e:
        if e.errno == 98:
            print("Порт уже используется другим процессом")
        else:
            print(f"Неизвестная ошибка: {e}")
        signal_kill()
    except Exception as e:
        print(f"Неизвестная ошибка: {e}")
        signal_kill()
    th.join()
