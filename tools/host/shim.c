/* zelr's system calls on the Windows host, so a ring 3 program runs as an
 * ordinary Windows process. A host tool, never part of the machine.
 *
 * Why: the browser's layout, style and script code is most of what changes
 * when a page looks wrong, and trying a change in QEMU is a build, a boot and
 * a page load -- a minute or two each. Built against this instead, the same
 * code renders a page in a second or two, twenty at a time, which is what
 * makes "every site" something that can be looked at rather than sampled.
 *
 * How: tools/host/build.sh compiles the program with a copy of sdk/zelr.h
 * whose one system call door calls host_syscall below. Files are not
 * reached (a program sees none); the network is real, with TLS done by
 * tlsproxy.py on 127.0.0.1 (HOST_PROXY, default 8765) so that the program's
 * own HTTP, chunking and gzip still run. A window is a buffer; the program's
 * idle waits drive a small script (HOST_SCRIPT: shot, pgdn*N, wheel*N,
 * go=ADDRESS, click=X:Y, wait) and `shot` writes the window as a PPM
 * (HOST_SHOT is the file prefix). HOST_SAMPLE=N prints the program's stack
 * five times after N seconds, named from the .pdb, for finding a hang;
 * HOST_NETLOG shows each connection and each wait that ran out.
 *
 * What it is not: a check. Nothing in the gate uses it, and it proves
 * nothing about the kernel, the TLS or the drawing on the machine; those are
 * what the QEMU harnesses are for. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dbghelp.h>

typedef long long zw;

typedef struct { unsigned type; int x, y; unsigned buttons; unsigned key; } win_event;

static unsigned *surf;
static int sw = 860, sh = 620, maxw = 2048, maxh = 2048;
static char *heap_base, *heap_brk, *heap_end;
static SOCKET socks[64];
static int sock_used[64];
static char outbuf[1 << 16];
static int outn;
static int lines_seen;           /* "browser: " lines */
static int shots_done;
static const char *shot_prefix;
static const char *script;       /* e.g. "shot,pgdn*2,shot" */
static int script_pos;
static win_event queue[256];
static int qhead, qtail;
static int waiting_for_line = 1;
static int wanted_line = 1;
static DWORD start_ms;
static int max_seconds = 120;

static void dump(const char *tag) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s.ppm", shot_prefix ? shot_prefix : "shot", tag);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", sw, sh);
    for (int i = 0; i < sw * sh; i++) {
        unsigned p = surf[i];
        unsigned char c[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

static void push(unsigned type, int x, int y, unsigned buttons, unsigned key) {
    win_event e = { type, x, y, buttons, key };
    queue[qtail++ & 255] = e;
}

static void out_char(char c) {
    fputc(c, stdout);
    if (c == '\n') {
        outbuf[outn] = 0;
        if (!strncmp(outbuf, "browser: ", 9)) lines_seen++;
        outn = 0;
        fflush(stdout);
    } else if (outn < (int)sizeof(outbuf) - 1) {
        outbuf[outn++] = c;
    }
}

/* The next step of the script, run when the program is idle. */
static void step(void) {
    if (!script || !script[script_pos]) {
        exit(0);
    }
    const char *s = script + script_pos;
    int len = 0;
    while (s[len] && s[len] != ',') len++;
    char cmd[256];
    memcpy(cmd, s, len < 255 ? len : 255);
    cmd[len < 255 ? len : 255] = 0;
    script_pos += len + (s[len] == ',');
    int times = 1;
    char *star = strchr(cmd, '*');
    if (star) { *star = 0; times = atoi(star + 1); }
    if (!strcmp(cmd, "shot")) {
        char tag[16];
        snprintf(tag, sizeof(tag), "%d", shots_done++);
        dump(tag);
    } else if (!strcmp(cmd, "pgdn")) {
        for (int i = 0; i < times; i++) { push(2, 0, 0, 0, 0x107); }
    } else if (!strcmp(cmd, "wheel")) {
        for (int i = 0; i < times; i++) push(5, sw / 2, sh / 2, 0, 0);
        queue[(qtail - 1) & 255].y = 3;
    } else if (!strncmp(cmd, "go=", 3)) {
        /* click the bar, type, return */
        push(1, 400, 22, 0x81, 0);
        push(1, 400, 22, 0x00, 0);
        push(2, 0, 0, 0, 1 | 0x20000);    /* ctrl+a */
        for (const char *t = cmd + 3; *t; t++) push(2, 0, 0, 0, (unsigned char)*t);
        push(2, 0, 0, 0, '\n');
        waiting_for_line = 1;
        wanted_line = lines_seen + 1;
    } else if (!strncmp(cmd, "click=", 6)) {
        int x = 0, y = 0;
        sscanf(cmd + 6, "%d:%d", &x, &y);
        push(1, x, y, 0x81, 0);
        push(1, x, y, 0x00, 0);
        waiting_for_line = 1;
        wanted_line = lines_seen + 1;
    } else if (!strcmp(cmd, "wait")) {
        waiting_for_line = 1;
        wanted_line = lines_seen + 1;
    }
}

static void idle(void) {
    if ((GetTickCount() - start_ms) / 1000 > (DWORD)max_seconds) {
        fprintf(stdout, "\nHOST: timed out\n");
        dump("T");
        exit(3);
    }
    if (qhead != qtail) return;
    if (waiting_for_line) {
        if (lines_seen < wanted_line) return;
        waiting_for_line = 0;
    }
    step();
}

static int sock_new(SOCKET s) {
    for (int i = 1; i < 64; i++)
        if (!sock_used[i]) { sock_used[i] = 1; socks[i] = s; return i; }
    closesocket(s);
    return -6;
}

static SOCKET tcp_to(const char *host, int port) {
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res) return INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, res->ai_addr, (int)res->ai_addrlen)) { closesocket(s); freeaddrinfo(res); return INVALID_SOCKET; }
    freeaddrinfo(res);
    DWORD tmo = 4000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    return s;
}

zw host_syscall(zw n, zw a, zw b, zw c) {
    switch (n) {
    case 0: fflush(stdout); exit((int)a);
    case 1: out_char((char)a); return 0;
    case 2: { const char *s = (const char *)b; for (int i = 0; i < c; i++) out_char(s[i]); return c; }
    case 3: return 7;
    case 4: return (zw)((GetTickCount() - start_ms) / 10);
    case 5: Sleep((DWORD)a); return 0;
    case 7: return 1;
    case 8: return (zw)surf;
    case 9: return ((zw)sw << 16) | sh;
    case 10: {
        idle();
        if (qhead == qtail) return 0;
        *(win_event *)b = queue[qhead++ & 255];
        return 1;
    }
    case 11: case 66: return 0;
    case 12: return 0;
    case 65: {       /* win_wait */
        idle();
        if (qhead != qtail) return 1;
        return 0;
    }
    case 36: case 37: case 56: case 64: return 0;
    case 57: if (b > 0) ((char *)a)[0] = 0; return 0;
    case 25: {
        SOCKET s = tcp_to((const char *)a, (int)b);
        if (s == INVALID_SOCKET) return -4;
        return sock_new(s);
    }
    case 45: {
        const char *proxy = getenv("HOST_PROXY");
        SOCKET s = tcp_to("127.0.0.1", proxy ? atoi(proxy) : 8765);
        if (s == INVALID_SOCKET) return -4;
        char line[256];
        int ll = snprintf(line, sizeof(line), "%s:%d\n", (const char *)a, b ? (int)b : 443);
        if (getenv("HOST_NETLOG")) fprintf(stderr, "HOST_TLS %s (%lu ms)\n", (const char *)a, GetTickCount() - start_ms);
        send(s, line, ll, 0);
        DWORD tmo = 20000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
        char ok = 0;
        if (recv(s, &ok, 1, 0) != 1 || ok != '1') { closesocket(s); return -5; }
        tmo = 4000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
        return sock_new(s);
    }
    case 46: { const char *m = "TLS 1.3 by the host"; int l = (int)strlen(m); if (l >= b) l = (int)b - 1; memcpy((char *)a, m, l); ((char *)a)[l] = 0; return l; }
    case 26: {
        if (a <= 0 || a >= 64 || !sock_used[a]) return -1;
        if (getenv("HOST_NETLOG")) { const char *q = (const char *)b; int k = 0; while (k < c && k < 200 && q[k] != 13) k++; fprintf(stderr, "HOST_SEND %d %.*s\n", (int)a, k, q); }
        int sent = 0;
        while (sent < c) { int k = send(socks[a], (const char *)b + sent, (int)c - sent, 0); if (k <= 0) return -1; sent += k; }
        return c;
    }
    case 27: {
        if (a <= 0 || a >= 64 || !sock_used[a]) return -1;
        int k = recv(socks[a], (char *)b, (int)c, 0);
        if (getenv("HOST_NETLOG") && k <= 0) fprintf(stderr, "HOST_RECV %d -> %d (%lu ms)\n", (int)a, k, GetTickCount() - start_ms);
        if (k > 0) return k;
        if (k == 0) return -2;
        if (WSAGetLastError() == WSAETIMEDOUT) return 0;
        return -2;
    }
    case 28: if (a > 0 && a < 64 && sock_used[a]) { closesocket(socks[a]); sock_used[a] = 0; } return 0;
    case 29: return -1;
    case 30: { memset((void *)a, 0, 24); *(unsigned *)a = 1; return 0; }
    case 31: { memset((void *)a, 0, 64); ((unsigned *)a)[8] = 1024; ((unsigned *)a)[9] = 768; return 0; }
    case 47: {
        char *was = heap_brk;
        if (a == 0) return (zw)was;
        if (a < 0) { heap_brk += a; if (heap_brk < heap_base) heap_brk = heap_base; return (zw)was; }
        if (heap_brk + a > heap_end) return 0;
        heap_brk += a;
        return (zw)was;
    }
    case 59: {
        void *p = VirtualAlloc(0, (SIZE_T)a, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        return (zw)p;
    }
    case 60: VirtualFree((void *)a, 0, MEM_RELEASE); return 0;
    case 38: case 39: return 0;
    default:
        return -1;
    }
}


static HANDLE main_thread;

/* After HOST_SAMPLE seconds, where the program is: the main thread's stack,
   five times a second apart, named through dbghelp from the .pdb. */
static DWORD WINAPI sampler(LPVOID arg) {
    int secs = (int)(long long)arg;
    Sleep(secs * 1000);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, 0, TRUE);
    for (int round = 0; round < 5; round++) {
        SuspendThread(main_thread);
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        GetThreadContext(main_thread, &ctx);
        STACKFRAME64 f;
        memset(&f, 0, sizeof(f));
        f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
        fprintf(stderr, "HOST_SAMPLE %d:\n", round);
        for (int depth = 0; depth < 40; depth++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, main_thread, &f, &ctx, 0,
                             SymFunctionTableAccess64, SymGetModuleBase64, 0)) break;
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 disp = 0;
            IMAGEHLP_LINE64 line;
            DWORD ld = 0;
            line.SizeOfStruct = sizeof(line);
            const char *name = "?";
            if (SymFromAddr(proc, f.AddrPC.Offset, &disp, si)) name = si->Name;
            if (SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ld, &line))
                fprintf(stderr, "   %s  %s:%lu\n", name, line.FileName, line.LineNumber);
            else
                fprintf(stderr, "   %s  +%llx\n", name, (unsigned long long)disp);
            if (!f.AddrReturn.Offset) break;
        }
        fflush(stderr);
        ResumeThread(main_thread);
        Sleep(1000);
    }
    return 0;
}

int zelr_main(int argc, char **argv);

int main(int argc, char **argv) {
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    start_ms = GetTickCount();
    shot_prefix = getenv("HOST_SHOT");
    script = getenv("HOST_SCRIPT");
    if (!script) script = "shot";
    if (getenv("HOST_W")) sw = atoi(getenv("HOST_W"));
    if (getenv("HOST_H")) sh = atoi(getenv("HOST_H"));
    if (getenv("HOST_SECS")) max_seconds = atoi(getenv("HOST_SECS"));
    surf = (unsigned *)VirtualAlloc(0, (SIZE_T)maxw * maxh * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    SIZE_T hs = (SIZE_T)1 << 30;
    heap_base = heap_brk = (char *)VirtualAlloc(0, hs, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    heap_end = heap_base + hs;
    setvbuf(stdout, 0, _IOFBF, 1 << 16);
    if (getenv("HOST_SAMPLE")) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread,
                        0, FALSE, DUPLICATE_SAME_ACCESS);
        CreateThread(0, 0, sampler, (LPVOID)(long long)atoi(getenv("HOST_SAMPLE")), 0, 0);
    }
    return zelr_main(argc, argv);
}
