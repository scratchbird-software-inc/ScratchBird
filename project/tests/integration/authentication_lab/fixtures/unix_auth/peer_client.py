# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

import json
import socket
import sys


client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(sys.argv[1])
response = json.loads(client.recv(4096))
if not all(key in response for key in ("pid", "uid", "gid")):
    raise SystemExit("peer credential response is incomplete")
print(json.dumps(response, sort_keys=True))
