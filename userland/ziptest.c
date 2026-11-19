/* The archive reader, against archives it did not make.
 *
 * They come from tools/genzip.py, which writes them with Python's zipfile
 * and zlib and then spoils some on purpose, a byte or a field each. A
 * reader checked against its own writer agrees with itself and proves
 * nothing; these are what other systems hand over.
 *
 * Headless and quick: whether Files shows an archive as a folder is
 * zipcheck.py's question, whether the bytes come out right is this one's.
 */
#include "zelr.h"
#include "alloc.h"
#include "zip.h"
#include "zipdata.h"

static int failed;

static void ok(const char *what, int cond) {
    puts(cond ? "  PASS  " : "  FAIL  ");
    puts(what);
    putc('\n');
    if (!cond) failed++;
}

static int same(const u8 *a, const u8 *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int named(const zip_entry *e, const char *name) {
    int n = (int)strlen(name);
    return e->name_len == n && same((const u8 *)e->name, (const u8 *)name, n);
}

/* The entry of a name, or 0. */
static int find(const zip_t *z, const char *name, zip_entry *out) {
    u32 at = 0;
    int r;
    while ((r = zip_next(z, &at, out)) == 1)
        if (named(out, name)) return 1;
    return 0;
}

static u8 room[8192];

/* What taking a name out of an archive answers: bytes, or a ZIP_ number. */
static int take(const u8 *data, u32 len, const char *name) {
    zip_t z;
    zip_entry e;
    if (zip_open(&z, data, len) != ZIP_OK) return -100;
    if (!find(&z, name, &e)) return -101;
    return zip_extract(&z, &e, room, sizeof(room));
}

int main(void) {
    puts("archives\n");

    /* --- what is in one, from its own directory --------------------------- */
    zip_t z;
    ok("an archive with a comment after its end is opened",
       zip_open(&z, ARC_GOOD, sizeof(ARC_GOOD)) == ZIP_OK);
    ok("and says it holds four things: three files and a folder", z.count == 4);
    {
        u32 at = 0;
        zip_entry e;
        int n = 0, dirs = 0, r;
        while ((r = zip_next(&z, &at, &e)) == 1) {
            n++;
            dirs += e.is_dir;
        }
        ok("its directory lists all four, the folder as a folder", r == 0 && n == 4 && dirs == 1);
    }
    zip_entry e;
    ok("readme.txt is in it, deflated", find(&z, "readme.txt", &e) && e.method == 8
       && e.usize == sizeof(FILE_README) && e.csize < e.usize);
    ok("notes/a.txt is in it, stored", find(&z, "notes/a.txt", &e) && e.method == 0
       && e.usize == sizeof(FILE_A));

    /* --- taken out, and checked ------------------------------------------- */
    int got = take(ARC_GOOD, sizeof(ARC_GOOD), "readme.txt");
    ok("a deflated file comes out as it went in",
       got == (int)sizeof(FILE_README) && same(room, FILE_README, got));
    got = take(ARC_GOOD, sizeof(ARC_GOOD), "notes/a.txt");
    ok("and a stored one", got == (int)sizeof(FILE_A) && same(room, FILE_A, got));
    got = take(ARC_GOOD, sizeof(ARC_GOOD), "notes/b.txt");
    ok("and one in a folder", got == (int)sizeof(FILE_B) && same(room, FILE_B, got));
    ok("the CRC-32 is the archive's own",
       find(&z, "notes/b.txt", &e) && zip_crc32(FILE_B, sizeof(FILE_B)) == e.crc);

    /* Written as a stream: no sizes in the local headers, only after the
       data and in the directory, which is what is read. */
    got = take(ARC_STREAM, sizeof(ARC_STREAM), "readme.txt");
    ok("an archive written as a stream, sizes only in its directory, is read",
       got == (int)sizeof(FILE_README) && same(room, FILE_README, got));
    got = take(ARC_STREAM, sizeof(ARC_STREAM), "notes/b.txt");
    ok("all of it", got == (int)sizeof(FILE_B) && same(room, FILE_B, got));

    /* --- refused, each for what it is ------------------------------------- */
    ok("a byte changed in a stored file is caught by its CRC-32",
       take(ARC_CRC, sizeof(ARC_CRC), "notes/a.txt") == ZIP_CRC);
    ok("and the rest of that archive still comes out",
       take(ARC_CRC, sizeof(ARC_CRC), "readme.txt") == (int)sizeof(FILE_README));
    ok("a file compressed in a way not read here is refused as such",
       take(ARC_METHOD, sizeof(ARC_METHOD), "readme.txt") == ZIP_METHOD);
    ok("a file locked with a password is refused as such",
       take(ARC_LOCKED, sizeof(ARC_LOCKED), "readme.txt") == ZIP_LOCKED);
    ok("an archive with its end cut off is not an archive",
       zip_open(&z, ARC_CUT, sizeof(ARC_CUT)) == ZIP_BAD);
    ok("nor is a picture", zip_open(&z, FILE_A, sizeof(FILE_A)) == ZIP_BAD);
    {
        zip_t g;
        zip_open(&g, ARC_GOOD, sizeof(ARC_GOOD));
        find(&g, "readme.txt", &e);
        ok("a file bigger than the room given says so", zip_extract(&g, &e, room, 100) == ZIP_ROOM);
    }

    puts(failed ? "ZIPTEST_FAIL\n" : "ZIPTEST_PASS\n");
    return failed;
}
