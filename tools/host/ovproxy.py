"""A stand-in for tlsproxy.py that answers some addresses from local files, for finding out why a
site's script stops: copy the script, put console.log where it goes wrong, and serve the copy in its
place (scriptdump prints the page's console).

  python tools/host/ovproxy.py PORT host/path=localfile ...
  HOST_PROXY=PORT build/host/scriptdump.exe URL 8

The shim's "host:port" line, then '1', then plain HTTP: each request is answered from its file when
host/path (without the query) is overridden, and otherwise fetched from the real server over verified
TLS and handed back whole, so the shim's kept connections keep working. A debugging tool, not a check.
A site that renames its scripts on every deploy needs the new name (GitHub's did within the hour).
"""
import http.client
import socket
import ssl
import sys
import threading

import kernelroots

OVERRIDES = {}
CTX = kernelroots.context()      # the kernel's roots, as tlsproxy.py


def read_request(f):
    line = f.readline()
    if not line:
        return None
    headers = []
    length = 0
    while True:
        h = f.readline()
        if not h or h in (b"\r\n", b"\n"):
            break
        name, _, value = h.decode("latin-1").partition(":")
        headers.append((name.strip(), value.strip()))
        if name.strip().lower() == "content-length":
            length = int(value.strip() or 0)
    body = f.read(length) if length else b""
    method, path, _ = line.decode("latin-1").split(" ", 2)
    return method, path, headers, body


def serve(c):
    try:
        c.settimeout(60)
        hostline = b""
        while not hostline.endswith(b"\n"):
            d = c.recv(1)
            if not d:
                return
            hostline += d
        host, port = hostline.decode().strip().rsplit(":", 1)
        c.sendall(b"1")
        f = c.makefile("rb")
        up = None
        while True:
            req = read_request(f)
            if not req:
                return
            method, path, headers, body = req
            key = host + path.split("?")[0]
            if key in OVERRIDES:
                data = open(OVERRIDES[key], "rb").read()
                ctype = "text/javascript" if key.endswith(".js") else "text/html"
                c.sendall(("HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                           "Access-Control-Allow-Origin: *\r\n\r\n" % (ctype, len(data))).encode() + data)
                print("overrode", key, flush=True)
                continue
            if up is None:
                up = http.client.HTTPSConnection(host, int(port), context=CTX, timeout=30)
            send = [(n, v) for n, v in headers if n.lower() not in ("connection", "accept-encoding")]
            up.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
            for n, v in send:
                up.putheader(n, v)
            up.endheaders(body or None)
            r = up.getresponse()
            data = r.read()
            out = ["HTTP/1.1 %d %s" % (r.status, r.reason)]
            for n, v in r.getheaders():
                if n.lower() in ("transfer-encoding", "content-length", "connection", "content-encoding"):
                    continue
                out.append("%s: %s" % (n, v))
            out.append("Content-Length: %d" % len(data))
            c.sendall(("\r\n".join(out) + "\r\n\r\n").encode("latin-1") + data)
    except Exception as e:
        print("proxy:", host if 'host' in dir() else "?", e, flush=True)
    finally:
        try:
            c.close()
        except Exception:
            pass


def main():
    port = int(sys.argv[1])
    for a in sys.argv[2:]:
        k, v = a.split("=", 1)
        OVERRIDES[k] = v
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(64)
    while True:
        c, _ = s.accept()
        threading.Thread(target=serve, args=(c,), daemon=True).start()


if __name__ == "__main__":
    main()
