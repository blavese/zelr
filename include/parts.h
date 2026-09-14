#pragma once
#include "types.h"

/* What is on the disk, before any filesystem is.
 *
 * nyx used to read the volume straight off sector zero, which is true of a
 * disk image and of nothing else. A real disk is divided, and the division
 * is described one of two ways.
 *
 * MBR is the old one: four entries in the boot sector, 32-bit sector
 * numbers, and no way to say anything about a disk past two terabytes. GPT
 * replaced it, and everything that boots through UEFI uses it, because the
 * firmware needs an EFI System Partition and that is a GPT concept. So on
 * any laptop this is the only table there is, and a kernel that reads only
 * MBR sees a single entry of type 0xEE covering the whole disk, which is
 * exactly what GPT puts there to stop it doing any damage.
 *
 * Both are read here, GPT first, and neither is written. Reading is what is
 * needed to find a filesystem on a machine somebody else partitioned;
 * writing one is how you destroy their disk by getting it wrong. */

#define PARTS_MAX 16
#define PART_NAME_MAX 37          /* 36 characters from GPT, and a terminator */

typedef enum {
    PART_SCHEME_NONE,             /* nothing recognisable: treat as one volume */
    PART_SCHEME_MBR,
    PART_SCHEME_GPT,
} part_scheme_t;

typedef struct {
    u32  start;                   /* first sector */
    u32  sectors;
    u8   mbr_type;                /* 0 when the table was GPT */
    bool efi_system;              /* the FAT volume the firmware boots from */
    bool looks_like_fat;          /* its first sector says so */
    char name[PART_NAME_MAX];     /* from GPT, or a description of the type */
} part_t;

/* Reads whichever table is there. Safe to call with no disk. */
void parts_scan(void);

part_scheme_t parts_scheme(void);
const char   *parts_scheme_name(void);
u32           parts_count(void);
const part_t *parts_get(u32 index);

/* Why a GPT was rejected, for the boot log. Empty when it was fine or when
   there was not one to begin with. */
const char *parts_gpt_error(void);

/* The IEEE polynomial, which GPT requires to validate its own header. It is
   here rather than in a utility file because this is the only thing that
   needs it. */
u32 crc32(const void *data, u32 len);
