# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

import json
import os
import socket
import struct
import sys


path = sys.argv[1]
os.makedirs(os.path.dirname(path), exist_ok=True)
if os.path.exists(path):
    os.unlink(path)

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
os.chmod(path, 0o777)
server.listen(8)

while True:
    connection, _ = server.accept()
    with connection:
        pid, uid, gid = struct.unpack(
            "3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        )
        connection.sendall(
            json.dumps({"pid": pid, "uid": uid, "gid": gid}).encode("utf-8") + b"\n"
        )
