# lsof -i :8765 | awk 'NR>1 {print $2;}' | xargs -t kill -9

import pytest
import os
from pathlib import Path
import subprocess
import logging
import signal

FUSE_DIR = "mountpoint"

logging.basicConfig(level=logging.INFO)


class FileSize:
    def __init__(self, sz, prefix="mb"):
        self.sz = sz
        self.prefix = prefix

    def __str__(self):
        return f"{self.sz} {self.prefix}"

    def __int__(self):
        # sz -> b
        if self.prefix == "mb":
            return self.sz * 1024**2
        assert self.prefix == "b"
        return self.sz


@pytest.fixture(scope="session", autouse=True)
def set_test_directory():
    os.chdir(Path(os.path.abspath(__file__)).parent.parent)


@pytest.fixture(scope="session", autouse=True)
def compile_client(set_test_directory):
    os.system("cmake .")
    os.system("cmake --build .")


@pytest.fixture(scope="module")
def unmount_fuse():
    os.system(f"fusermount -u {FUSE_DIR}")


@pytest.fixture(scope="module")
def run_server():
    process = subprocess.Popen(
        "python3 server.py --pytest",
        shell=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process_alive(process)
    yield process
    process.terminate()


def process_alive(process: subprocess.Popen):
    return process.poll() is None


def stdout_iter(popen):
    for stdout_line in iter(popen.stdout.readline, ""):
        yield stdout_line
    popen.stdout.close()


@pytest.fixture(scope="module")
def init_host_client(compile_client, unmount_fuse):  # run_server
    host = subprocess.Popen(
        "./client",
        shell=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    assert process_alive(host)
    assert next(stdout_iter(host)).startswith("uuid : 123")

    client = subprocess.Popen(
        "./client 123",
        shell=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        preexec_fn=os.setsid,
    )

    assert process_alive(host)
    assert process_alive(client)

    yield host, client

    # assert process_alive(host)
    # assert process_alive(client)

    os.killpg(os.getpgid(client.pid), signal.SIGINT)

    host.terminate()
    # client.terminate()


import uuid


@pytest.fixture(scope="function")
def bigfile(init_host_client):
    fname = str(uuid.uuid1())
    file = open(fname, "wb")
    yield fname, file
    os.remove(fname)


import os.path


def wait_for_file(fname, file_sz_bytes):
    while not os.path.isfile(fname):
        pass

        # yes_no = subprocess.Popen(f'(ls -n {fname}  >> /dev/null 2>&1 && echo yes) || echo no', shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.read()
        # if yes_no == b"yes\n":
        #    break
    # TODO я не знаю почему, но если посмотреть на его размер то будет 0, хотя я всё равно делаю lookup по нему -- надо подумаь над этим
    while True:
        size = subprocess.Popen(
            f"ls -n {fname}" + " | awk '{print $5}'",
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ).stdout.read()
        if int(size) == file_sz_bytes:
            break


import random, string


@pytest.mark.parametrize(
    "file_sz_bytes",
    [
        FileSize(3, "b"),
        FileSize(3, "b"),
        FileSize(3),
        FileSize(2),
        FileSize(1),
    ],
)
def test_file(bigfile, file_sz_bytes):
    fname, file = bigfile

    file.write(
        "".join(
            random.choice(string.ascii_uppercase + string.digits)
            for _ in range(int(file_sz_bytes))
        ).encode()
    )
    # file.write(os.urandom(int(file_sz_bytes)))
    file.close()

    logging.info(fname)
    wait_for_file(f"{FUSE_DIR}/{fname}", int(file_sz_bytes))
    logging.info(
        "".join(
            list(
                stdout_iter(
                    subprocess.Popen(
                        f"ls -l {FUSE_DIR}",
                        shell=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        universal_newlines=True,
                    )
                )
            )
        )
    )
    assert (
        subprocess.Popen(
            f"diff {fname} {FUSE_DIR}/{fname}",
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ).stdout.read()
        == b""
    )


if __name__ == "__main__":
    pytest.main(["-s", __file__])
