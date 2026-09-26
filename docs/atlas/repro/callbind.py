"""Function.prototype.call, apply and bind do not work in the JS engine.

Found by reading (docs/atlas/12-js-engine.md section 10) and reproduced here
on 2026-09-26 against commit 6048716. Not a gate step: it is the evidence and
the starting point for a fix, and it is expected to report BROKEN until then.

Why it happens. Reading `f.call` returns a native wrapper with the target
stored on it as `__fn__` (userland/jsrun.h:450-460). When the wrapper is then
called as `f.call(obj, ...)`, the `this` it receives is `f`, the receiver of
the member expression, and `fn_held` looks for `__fn__` on that instead of on
the wrapper (jsrun.h:573-622). It finds nothing and calls undefined, which
throws "this is not a function". A bound function has the same problem one
call later.

How this shows it. A page served from the host carries four scripts: a
control that must run, and one each for call, apply and bind. /bin/jsprobe
runs every script on its own and says what stopped it.

    python docs/atlas/repro/callbind.py       (build first: bash build.sh)
"""
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
os.environ["ZELR_PREBUILT"] = "1"
from harness import Guest, free_port  # noqa: E402

PAGE = """<!doctype html>
<html><head><title>call apply bind</title></head><body><p>probe</p>
<script>var ctl = 1 + 1; if (ctl !== 2) throw new Error("control failed");</script>
<script>function f1(a, b) { return this.x + a + b; }
var r1 = f1.call({x: 1}, 2, 3); if (r1 !== 6) throw new Error("call gave " + r1);</script>
<script>function f2(a, b) { return this.x + a + b; }
var r2 = f2.apply({x: 1}, [2, 3]); if (r2 !== 6) throw new Error("apply gave " + r2);</script>
<script>function f3(a) { return this.x + a; }
var g3 = f3.bind({x: 10}); var r3 = g3(5); if (r3 !== 15) throw new Error("bind gave " + r3);</script>
</body></html>
"""


def main():
    site = tempfile.mkdtemp(prefix="zelr-callbind-")
    with open(os.path.join(site, "index.html"), "w", newline="\n") as f:
        f.write(PAGE)
    port = free_port()
    server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1",
         "--directory", site], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1)
        g = Guest(os.path.join(ROOT, "build", "repro-callbind.img"), memory=256,
                  extra=["-netdev", "user,id=n0", "-device", "e1000,netdev=n0"])
        try:
            g.wait_boot(120)
            time.sleep(6)                               # the boot-time dhcp task
            mark = len(g.serial())
            g.type("exec /bin/jsprobe http://10.0.2.2:%d/\n" % port)
            g.wait_serial("JSPROBE_DONE", timeout=120)
            out = g.serial()[mark:]
        finally:
            g.stop()
    finally:
        server.terminate()

    print(out)
    stopped = out.count("stopped at")
    print("BROKEN" if stopped else "WORKS", "(%d of the three method scripts stopped)" % stopped)
    return 1 if stopped else 0


if __name__ == "__main__":
    sys.exit(main())
