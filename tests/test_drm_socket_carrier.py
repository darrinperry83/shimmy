"""Darwin carrier semantics required by the virtual DRM fd handoff."""
import array
import os
import socket
import tempfile

with tempfile.TemporaryDirectory(prefix="rd-drm-fd-", dir="/tmp") as base:
    path = base + "/carrier"
    sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sender.bind(path)
    sender.connect(path)
    os.unlink(path)
    bridge_a, bridge_b = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    fds = array.array("i", [sender.fileno()])
    bridge_a.sendmsg([b"D"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, fds)])
    _, ancillary, _, _ = bridge_b.recvmsg(1, socket.CMSG_SPACE(fds.itemsize))
    assert len(ancillary) == 1 and ancillary[0][:2] == (socket.SOL_SOCKET, socket.SCM_RIGHTS)
    copy = array.array("i")
    copy.frombytes(ancillary[0][2][:fds.itemsize])
    received = socket.socket(fileno=copy[0])
    assert os.fstat(sender.fileno()).st_ino == os.fstat(received.fileno()).st_ino
    sender.close()
    received.send(b"flip-event")
    assert received.recv(32) == b"flip-event"
    received.close()
    bridge_a.close()
    bridge_b.close()
