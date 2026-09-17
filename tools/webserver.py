"""A web server on the host, for the guest to fetch pages from.

Written here rather than pointed at a site on the internet, because a check
that depends on somebody else's server tells you about their uptime as well
as your own code, and it cannot serve the awkward cases on purpose: a body
with no length on it, a chunked one, a redirect that loops.

It is a server and not a recording. The bytes on the wire are produced by
Python's own HTTP implementation, so a request this project got wrong is
answered the way a real server would answer it rather than the way this
project expected.

QEMU's user networking carries the guest to it, so nothing in the guest
knows it is talking to the machine it is running on.
"""
import http.server
import socket
import threading

# Where the guest reaches the host. QEMU's user networking puts the machine
# it is running on at this address, and a connection to any port on it is
# carried to the same port on the host's loopback.
#
# Not guestfwd, which looks like the tidier way to do this and carries one
# connection and then stops answering. The first fetch worked and every one
# after it timed out, which reads exactly like a bug in the guest.
HOST_IP = "10.0.2.2"

# And where the same server sits when it is reached the other way, through a
# forward inside qemu rather than out through its NAT. See instant_args.
INSTANT_IP = "10.0.2.100"


# A page with a heading, paragraphs, a list and links, which is the shape of
# thing the browser has to get right.
PAGE = b"""<!doctype html>
<html><head><title>zelr test page</title></head>
<body>
<h1>Hello from the host</h1>
<p>This page came off a real web server over a real TCP connection.
It is here so the browser can be checked against something that is not
part of this project.</p>
<h2>A list</h2>
<ul>
<li>first item</li>
<li>second item</li>
<li>third item</li>
</ul>
<p>And a <a href="/second">link to another page</a>.</p>
</body></html>
"""

# A page with some of everything in it and no link, so that what is on the
# screen is decided entirely by the markup and the layout.
SAMPLE = b"""<!doctype html>
<html><head><title>every shape</title></head><body>
<h1>A heading</h1>
<p>A paragraph with <b>bold</b> in it and a word in <code>code</code>.
It is long enough to need wrapping at any sensible window width, which is
the point of it.</p>
<h2>A smaller heading</h2>
<ul><li>one</li><li>two</li><li>three</li></ul>
<ol><li>first</li><li>second</li></ol>
<hr>
<blockquote>Something set in from the margin.</blockquote>
<pre>  preformatted
  two   spaces   kept</pre>
<p>Entities: &amp; &lt; &gt; &quot; &mdash; &hellip; &copy;</p>
</body></html>
"""

SECOND = b"""<!doctype html>
<html><head><title>the second page</title></head>
<body><h1>Second</h1><p>You followed a link to get here.</p>
<p><a href="/">back to the first</a></p></body></html>
"""


def filler(n):
    """A body of a known length whose every byte can be checked.

    Sixteen bytes to a line, numbered, so a body that came back doubled or
    short says where it went wrong rather than only that it did."""
    out = bytearray()
    i = 0
    while len(out) < n:
        out += b"%06d abcdefgh\n" % i
        i += 1
    return bytes(out[:n])


BIG = filler(200000)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, body, ctype="text/html", status=200, extra=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or []):
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]

        if path == "/" or path == "/index.html":
            self._send(PAGE)
        elif path == "/second":
            self._send(SECOND)
        elif path == "/big":
            self._send(BIG, ctype="text/plain")
        elif path.startswith("/size/"):
            self._send(filler(int(path[6:])), ctype="text/plain")
        elif path == "/framed":
            # The same bytes as /measured, sent in pieces with a size on each
            # rather than with a length on the whole. A browser that gets the
            # decoding wrong renders this differently from the other one, and
            # comparing the two pictures is the check.
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            body = SAMPLE
            step = 97                    # an awkward size on purpose
            for i in range(0, len(body), step):
                piece = body[i:i + step]
                self.wfile.write(b"%X\r\n" % len(piece) + piece + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif path == "/measured":
            self._send(SAMPLE)
        elif path == "/chunked":
            # No length: the body arrives in pieces and ends with a zero.
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            body = filler(50000)
            step = 4000
            for i in range(0, len(body), step):
                piece = body[i:i + step]
                self.wfile.write(b"%X\r\n" % len(piece) + piece + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif path == "/close":
            # Neither a length nor chunks: the end of the body is the end of
            # the connection, which is what HTTP/1.0 servers still do.
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(filler(30000))
            self.close_connection = True
        elif path == "/redirect":
            self._send(b"moved", status=302, extra=[("Location", "/second")])
        elif path == "/redirect-relative":
            self._send(b"moved", status=301, extra=[("Location", "second")])
        elif path == "/loop":
            self._send(b"round", status=302, extra=[("Location", "/loop")])
        elif path == "/slow":
            # A body that arrives in dribs, to catch anything that treats a
            # quiet moment as the end of the answer.
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", "2000")
            self.end_headers()
            import time
            for _ in range(4):
                self.wfile.write(b"x" * 500)
                self.wfile.flush()
                time.sleep(0.4)
        else:
            self._send(b"<html><body><h1>404</h1></body></html>", status=404)


class Server:
    """Started with `with`, so a check that fails still puts the port back."""

    def __init__(self):
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever,
                                       daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *a):
        self.httpd.shutdown()
        self.httpd.server_close()

    @property
    def host(self):
        """What the guest puts in a URL to reach this."""
        return "%s:%d" % (HOST_IP, self.port)

    def qemu_args(self, model="e1000"):
        """What to pass a guest so it can reach this. Ordinary user mode
        networking: the forwarding is slirp's own, and it is the same path
        that reaches the rest of the internet."""
        return ["-nic", "user,model=%s" % model]

    def instant_args(self, model="e1000"):
        """The same server, reached by a path that answers instantly.

        guestfwd carries exactly one connection and then stops answering,
        which is useless for a sequence of fetches and perfect for this: it
        replies from inside qemu, in the same virtual instant the guest sends,
        so the card's interrupt lands between the instruction that sends a
        segment and the one after it. That is the only arrangement here in
        which a stack that settles a sequence number after sending is
        reliably wrong, so it is the only one that can check it was not.

        The address is on this machine's own subnet, so nothing leaves it."""
        return ["-nic", "user,model=%s,guestfwd=tcp:%s:80-tcp:127.0.0.1:%d"
                % (model, INSTANT_IP, self.port)]


def reachable(port):
    """That the server is up on the host, before blaming the guest."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2):
            return True
    except OSError:
        return False
