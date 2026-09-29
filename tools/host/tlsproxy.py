"""TLS for the host shim: a line "host:port\\n", then '1' and the bytes both
ways over a verified TLS connection, or '0'. One thread per connection,
moving bytes with select, so the TLS socket is never used from two threads."""
import select
import socket
import ssl
import sys
import threading

CTX = ssl.create_default_context()


def serve(c):
    t = None
    try:
        c.settimeout(20)
        line = b""
        while not line.endswith(b"\n") and len(line) < 300:
            d = c.recv(1)
            if not d:
                return
            line += d
        host, port = line.decode().strip().rsplit(":", 1)
        raw = socket.create_connection((host, int(port)), timeout=15)
        t = CTX.wrap_socket(raw, server_hostname=host)
        c.sendall(b"1")
    except Exception:
        try:
            c.sendall(b"0")
        except Exception:
            pass
        try:
            c.close()
        except Exception:
            pass
        return
    c.setblocking(False)
    t.setblocking(False)
    to_t = b""
    to_c = b""
    c_open = t_open = True
    try:
        while (c_open and t_open) or to_c or to_t:
            rl, wl = [], []
            if c_open and not to_t:
                rl.append(c)
            if t_open and not to_c:
                rl.append(t)
            if to_t:
                wl.append(t)
            if to_c:
                wl.append(c)
            if not rl and not wl:
                break
            if not (t_open and t.pending()):
                r, w, _ = select.select(rl, wl, [], 120)
                if not r and not w:
                    break
            else:
                r, w = [t], []
            if t in r or (t_open and t.pending()):
                try:
                    d = t.recv(65536)
                    if d:
                        to_c += d
                    else:
                        t_open = False
                except (ssl.SSLWantReadError, ssl.SSLWantWriteError, BlockingIOError):
                    pass
                except Exception:
                    t_open = False
            if c in r:
                try:
                    d = c.recv(65536)
                    if d:
                        to_t += d
                    else:
                        c_open = False
                except BlockingIOError:
                    pass
                except Exception:
                    c_open = False
            if to_t and t in w:
                try:
                    k = t.send(to_t)
                    to_t = to_t[k:]
                except (ssl.SSLWantReadError, ssl.SSLWantWriteError, BlockingIOError):
                    pass
                except Exception:
                    to_t = b""
                    t_open = False
            if to_c and c in w:
                try:
                    k = c.send(to_c)
                    to_c = to_c[k:]
                except BlockingIOError:
                    pass
                except Exception:
                    to_c = b""
                    c_open = False
            if not t_open and not to_c:
                break
            if not c_open and not to_t:
                break
    finally:
        for s in (c, t):
            try:
                s.close()
            except Exception:
                pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(128)
    while True:
        c, _ = s.accept()
        threading.Thread(target=serve, args=(c,), daemon=True).start()


if __name__ == "__main__":
    main()
