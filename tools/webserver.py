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


# --- a page whose appearance is entirely in its style sheets --------------
#
# The markup below says nothing about how it looks. Everything that decides
# that is in three places, on purpose, because they are three different
# paths through the cascade and a browser can get any one of them right and
# the others wrong: a sheet fetched over its own connection, a sheet written
# into the page, and an attribute on one element.
#
# BARE is the same markup with none of them. The check is that the two do not
# look alike, which is a question the browser cannot pass by accident: it
# fails both if the sheets are ignored and if they are applied to everything.

STYLED = b"""<!doctype html>
<html><head><title>styled</title>
<link rel="stylesheet" href="/style.css">
<style>
  .band { background: #1d4ed8; color: #ffffff; padding: 22px 26px; }
  .band h1 { font-size: 30px; margin: 0; color: #ffffff; }
  #note { border-left: 4px solid #f59e0b; padding-left: 14px; color: #92400e; }
</style>
</head>
<body>
<div class="band"><h1>A band across the top</h1></div>
<div class="wrap">
<p class="lead">This paragraph is set wider and larger than the rest by a
sheet that arrived over its own connection.</p>
<p id="note">And this one is called out by an id, which beats the class
rules above it however many of them there are.</p>
<p style="text-align:center;color:#15803d">This one says so itself, which
beats every sheet there is.</p>
<ul class="plain"><li>no bullet</li><li>on these</li></ul>
</div>
</body></html>
"""

SHEET = b"""
body { margin: 0; font-size: 16px; color: #333; }
.wrap { max-width: 520px; margin: 0 auto; padding: 18px 0; }
.wrap p { line-height: 1.6; margin: 14px 0; }
p.lead { font-size: 20px; color: #111; }
ul.plain { list-style-type: none; padding-left: 0; }
ul.plain li { background: #f1f5f9; padding: 6px 10px; margin: 4px 0;
              border-radius: 6px; }
"""

# A page that looks one way if its script ran and another if it did not.
#
# The band is the check. Nothing in the markup has the class that paints it:
# the script puts that class on, so the band exists only if getElementById
# found the element, the assignment reached the document, and the cascade
# then matched the class that was not there when the sheet was indexed.
# Counting one colour asks all of that at once.
#
# The sum is there so the answer cannot come from anywhere but running the
# language: 1 through 10 is 55, and it is written into the page as text.
SCRIPTED = b"""<!doctype html>
<html><head><title>before the script</title>
<style>
  body { font-family: sans-serif; padding: 24px }
  .lit { background: #1d4ed8; color: #ffffff; padding: 22px 26px }
</style></head>
<body>
<h1 id="head">before the script</h1>
<div id="box">a band, if the script switched it on</div>
<p id="sum">nothing yet</p>
<script>
  var total = 0;
  for (var i = 1; i <= 10; i++) total = total + i;

  document.getElementById("head").textContent = "the script ran";
  document.getElementById("sum").textContent = "sum " + total;
  document.getElementById("box").className = "lit";
  document.title = "after the script";
</script>
</body></html>
"""

# The same page with the script taken out, so the check can show that the
# band and the words come from running it rather than from the markup.
UNSCRIPTED = SCRIPTED[:SCRIPTED.index(b"<script>")] + b"</body></html>" + chr(10).encode()

# A picture, built here rather than kept as a file, so the test data is in
# the same place as everything else it is testing with. Solid rose, which is
# a colour nothing else on the screen is, so counting it answers "did the
# picture arrive, decode and get drawn" in one number.
def _logo(w, h, rgb):
    import struct
    import zlib

    def chunk(name, body):
        return (struct.pack(">I", len(body)) + name + body
                + struct.pack(">I", zlib.crc32(name + body) & 0xFFFFFFFF))

    raw = bytearray()
    for _ in range(h):
        raw.append(0)                       # no filter on this row
        raw += bytes(rgb) * w

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


LOGO = _logo(160, 90, (225, 29, 72))

# A drawing rather than a picture: the same colour, described as shapes. This
# is what a logo on a real page is now, and it is why the browser could show
# the word Google and no Google.
DRAWING = b"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 60">
  <rect x="0" y="0" width="100" height="60" fill="#e11d48"/>
</svg>
"""

DRAWN = b"""<!doctype html>
<html><head><title>a drawing</title>
<style>body { font-family: sans-serif; padding: 24px }</style></head>
<body>
<h1>a drawing</h1>
<p>below this line there should be one</p>
<img src="/logo.svg" width="200" height="120" alt="a drawing that did not draw">
</body></html>
"""

PICTURE = b"""<!doctype html>
<html><head><title>a picture</title>
<style>body { font-family: sans-serif; padding: 24px }</style></head>
<body>
<h1>a picture</h1>
<p>below this line there should be one</p>
<img src="/logo.png" alt="this is what it says when it cannot be shown">
</body></html>
"""

# The same page pointing at something that is not there, so the words it
# carries are what shows instead. That is what alt text is for, and a
# browser that drew nothing at all would look identical to one that drew
# the picture wrongly.
MISSING = PICTURE.replace(b"/logo.png", b"/no-such-picture.png")

BARE = STYLED.replace(b'<link rel="stylesheet" href="/style.css">', b"")
BARE = BARE[:BARE.index(b"<style>")] + BARE[BARE.index(b"</style>") + 8:]
BARE = BARE.replace(b' style="text-align:center;color:#15803d"', b"")


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



# A form, and a page that says back exactly what arrived. The value of this
# one is that it is the server deciding what was received rather than the
# browser reporting what it thinks it sent: a client that escapes a space
# wrongly, or sends a box that was never ticked, looks correct from inside
# itself and wrong here.
# Every control is given a colour of its own, because the check that drives
# this has to find them on the screen, and a field is otherwise the same
# white as the page behind it. The unnamed box is there to be left out: a
# control with no name sends nothing, and a client that sent it anyway would
# look perfectly correct from inside itself.
FORM = b"""<html><head><title>a form</title></head><body>
<style>
#q{background:#00A000}
#deep{background:#C000C0}
#go{background:#E08000}
#spare{background:#00C0C0}
#off{background:#808000}
</style>
<h1>Ask something</h1>
<form action="/said" method="get">
<input id="q" name="q" size="24" value="">
<input id="spare" size="6" value="ignored">
<input id="deep" type="checkbox" name="deep" value="yes">
<input id="off" type="checkbox" name="off" value="no">
<input type="hidden" name="from" value="zelr">
<input id="go" type="submit" value="Search">
</form>
</body></html>"""

POSTED = b"""<html><head><title>a form that posts</title></head><body>
<style>#q{background:#00A000}#go{background:#E08000}</style>
<h1>Say something</h1>
<form action="/said" method="post">
<input id="q" name="q" size="24" value="">
<input id="go" type="submit" value="Send">
</form>
</body></html>"""

# What the server was actually sent, which is the only account of a form that
# is not the client marking its own work.
RECEIVED = []

# How many connections were opened, and how many requests came down them.
# The second divided by the first is the whole question about keep alive, and
# it is a question only the server can answer: a client that opened one
# connection per request looks identical from inside itself.
COUNTS = {"connections": 0, "requests": 0}

# And what a Cookie header said, per request, so a session can be checked
# from the side that would actually act on it.
COOKIES = []

GZIPPED = b"""<html><head><title>compressed</title></head><body>
<h1>Sent compressed</h1>
<p>This page went over the wire deflated, with a gzip wrapper round it, and
had to be put back together at the other end before any of these words
existed.</p>
</body></html>"""


class Handler(http.server.BaseHTTPRequestHandler):
    def setup(self):
        COUNTS["connections"] += 1
        http.server.BaseHTTPRequestHandler.setup(self)

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

    def _said(self, how, text):
        RECEIVED.append((how, text))
        """What arrived, as a page, with the pairs one to a line."""
        out = [b"<html><head><title>said</title></head><body><h1>said</h1>"]
        out.append(b"<p id=how>" + how.encode() + b"</p><ul>")
        for pair in text.split("&"):
            if not pair:
                continue
            out.append(b"<li>" + pair.encode() + b"</li>")
        # A way on to the form that posts, so a check does not have to type
        # an address: typing one goes through the serial port a character at
        # a time and the guest drops what it cannot drain in time.
        out.append(b"</ul><p><a href=\"/posts\">post one</a></p>"
                   b"</body></html>")
        self._send(b"".join(out))

    def do_POST(self):
        path = self.path.split("?")[0]
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode("latin-1")
        if path == "/said":
            self._said("post", body)
        else:
            self._send(b"<html><body><h1>404</h1></body></html>", status=404)

    def _gzip(self, body):
        """Compressed, but only for a client that said it could cope.

        Sending it to one that did not ask is how a check passes against a
        browser that never learned to undo it."""
        import gzip as gz
        if "gzip" not in (self.headers.get("Accept-Encoding") or ""):
            self._send(body)
            return
        packed = gz.compress(body)
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(len(packed)))
        self.end_headers()
        self.wfile.write(packed)

    def do_GET(self):
        COUNTS["requests"] += 1
        COOKIES.append(self.headers.get("Cookie") or "")
        path = self.path.split("?")[0]
        query = self.path.split("?", 1)[1] if "?" in self.path else ""

        if path == "/" or path == "/index.html":
            self._send(PAGE)
        elif path == "/gz":
            self._gzip(GZIPPED)
        elif path == "/setcookie":
            self._send(b"<html><body><h1>set</h1>"
                       b"<p><a href=\"/whoami\">who am i</a></p></body></html>",
                       extra=[("Set-Cookie", "sid=abc123; Path=/"),
                              ("Set-Cookie", "pref=dark; Path=/")])
        elif path == "/whoami":
            got = self.headers.get("Cookie") or "nothing"
            self._send(b"<html><body><h1>you are</h1><p id=who>"
                       + got.encode() + b"</p></body></html>")
        elif path == "/bye":
            self._send(b"<html><body><h1>bye</h1>"
                       b"<p><a href=\"/whoami\">who am i</a></p></body></html>",
                       extra=[("Set-Cookie", "sid=; Path=/; Max-Age=0")])
        elif path == "/form":
            self._send(FORM)
        elif path == "/posts":
            self._send(POSTED)
        elif path == "/said":
            self._said("get", query)
        elif path == "/second":
            self._send(SECOND)
        elif path == "/styled":
            self._send(STYLED)
        elif path == "/bare":
            self._send(BARE)
        elif path == "/logo.svg":
            self._send(DRAWING, ctype="image/svg+xml")
        elif path == "/drawn":
            self._send(DRAWN)
        elif path == "/logo.png":
            self._send(LOGO, ctype="image/png")
        elif path == "/picture":
            self._send(PICTURE)
        elif path == "/missing-picture":
            self._send(MISSING)
        elif path == "/scripted":
            self._send(SCRIPTED)
        elif path == "/unscripted":
            self._send(UNSCRIPTED)
        elif path == "/style.css":
            self._send(SHEET, ctype="text/css")
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

    def counts(self):
        """Connections opened and requests served, since the last reset."""
        return dict(COUNTS)

    def cookies(self):
        """The Cookie header of every request, oldest first."""
        return list(COOKIES)

    def reset_counts(self):
        COUNTS["connections"] = 0
        COUNTS["requests"] = 0
        del COOKIES[:]

    def received(self):
        """Every form this server was sent, oldest first."""
        return list(RECEIVED)

    def forget(self):
        del RECEIVED[:]

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
