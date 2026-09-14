/* The parts of the UEFI interface zelr needs, written from the specification.
 *
 * There is a well-known library for this (gnu-efi) and this is deliberately
 * not it. Everything below is a transcription of the structure layouts in the
 * UEFI specification, which is what the firmware actually expects in memory;
 * getting a field's offset wrong is the only way to be wrong here, so the
 * ones that are never called are still declared, as void pointers, to keep
 * the ones that are at the right offsets.
 *
 * Firmware calls use the Microsoft calling convention regardless of what the
 * rest of the machine uses, which is what EFIAPI is for.
 */
#pragma once

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

typedef u64  UINTN;
typedef u64  EFI_STATUS;
typedef void *EFI_HANDLE;
typedef u16  CHAR16;
typedef u64  EFI_PHYSICAL_ADDRESS;
typedef u64  EFI_VIRTUAL_ADDRESS;

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS               0
#define EFI_LOAD_ERROR            0x8000000000000001ull
#define EFI_INVALID_PARAMETER     0x8000000000000002ull
#define EFI_UNSUPPORTED           0x8000000000000003ull
#define EFI_BUFFER_TOO_SMALL      0x8000000000000005ull
#define EFI_NOT_FOUND             0x800000000000000Eull

#define EFI_ERROR(s) (((EFI_STATUS)(s)) >> 63)

typedef struct {
    u32 data1;
    u16 data2;
    u16 data3;
    u8  data4[8];
} EFI_GUID;

typedef struct {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
} EFI_TABLE_HEADER;

/* --- console ------------------------------------------------------------ */

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *reset;
    EFI_STATUS (EFIAPI *output_string)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *,
                                       CHAR16 *);
    void *test_string;
    void *query_mode;
    EFI_STATUS (EFIAPI *set_mode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
    EFI_STATUS (EFIAPI *set_attribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *,
                                       UINTN);
    EFI_STATUS (EFIAPI *clear_screen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
    void *set_cursor_position;
    void *enable_cursor;
    void *mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
    u16 scan_code;
    CHAR16 unicode_char;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, u8);
    EFI_STATUS (EFIAPI *read_key_stroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *,
                                         EFI_INPUT_KEY *);
    void *wait_for_key;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* --- memory ------------------------------------------------------------- */

typedef enum {
    AllocateAnyPages, AllocateMaxAddress, AllocateAddress
} EFI_ALLOCATE_TYPE;

/* The types the firmware uses to describe a region. Anything the loader may
   take over once boot services are gone is listed in usable_after_exit(). */
#define EfiReservedMemoryType       0
#define EfiLoaderCode               1
#define EfiLoaderData               2
#define EfiBootServicesCode         3
#define EfiBootServicesData         4
#define EfiRuntimeServicesCode      5
#define EfiRuntimeServicesData      6
#define EfiConventionalMemory       7
#define EfiUnusableMemory           8
#define EfiACPIReclaimMemory        9
#define EfiACPIMemoryNVS            10
#define EfiMemoryMappedIO           11
#define EfiMemoryMappedIOPortSpace  12
#define EfiPalCode                  13
#define EfiPersistentMemory         14

typedef struct {
    u32 type;
    u32 pad;
    EFI_PHYSICAL_ADDRESS physical_start;
    EFI_VIRTUAL_ADDRESS  virtual_start;
    u64 pages;
    u64 attribute;
} EFI_MEMORY_DESCRIPTOR;

/* --- graphics ----------------------------------------------------------- */

#define EFI_GOP_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    u32 version;
    u32 horizontal_resolution;
    u32 vertical_resolution;
    u32 pixel_format;
    u32 red_mask, green_mask, blue_mask, reserved_mask;
    u32 pixels_per_scan_line;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    u32 max_mode;
    u32 mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    UINTN size_of_info;
    EFI_PHYSICAL_ADDRESS frame_buffer_base;
    UINTN frame_buffer_size;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *query_mode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, u32,
                                    UINTN *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    EFI_STATUS (EFIAPI *set_mode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, u32);
    void *blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* --- the image we are, and the volume we came from ---------------------- */

#define EFI_LOADED_IMAGE_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    u32 revision;
    EFI_HANDLE parent_handle;
    void *system_table;
    EFI_HANDLE device_handle;
    void *file_path;
    void *reserved;
    u32 load_options_size;
    void *load_options;
    void *image_base;
    u64 image_size;
    u32 image_code_type;
    u32 image_data_type;
    void *unload;
} EFI_LOADED_IMAGE_PROTOCOL;

#define EFI_SIMPLE_FILE_SYSTEM_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_MODE_READ  0x0000000000000001ull

typedef struct EFI_FILE_PROTOCOL {
    u64 revision;
    EFI_STATUS (EFIAPI *open)(struct EFI_FILE_PROTOCOL *, struct EFI_FILE_PROTOCOL **,
                              CHAR16 *, u64, u64);
    EFI_STATUS (EFIAPI *close)(struct EFI_FILE_PROTOCOL *);
    void *delete;
    EFI_STATUS (EFIAPI *read)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
    void *write;
    EFI_STATUS (EFIAPI *get_position)(struct EFI_FILE_PROTOCOL *, u64 *);
    EFI_STATUS (EFIAPI *set_position)(struct EFI_FILE_PROTOCOL *, u64);
    void *get_info;
    void *set_info;
    void *flush;
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    u64 revision;
    EFI_STATUS (EFIAPI *open_volume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *,
                                     EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* --- boot services ------------------------------------------------------ */

/* Only the entries this loader calls are typed. The rest are void pointers,
   because what matters is that everything after them lands at the offset the
   firmware wrote it to. */
typedef struct {
    EFI_TABLE_HEADER hdr;

    void *raise_tpl;
    void *restore_tpl;

    EFI_STATUS (EFIAPI *allocate_pages)(EFI_ALLOCATE_TYPE, u32, UINTN,
                                        EFI_PHYSICAL_ADDRESS *);
    EFI_STATUS (EFIAPI *free_pages)(EFI_PHYSICAL_ADDRESS, UINTN);
    EFI_STATUS (EFIAPI *get_memory_map)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *,
                                        UINTN *, u32 *);
    EFI_STATUS (EFIAPI *allocate_pool)(u32, UINTN, void **);
    EFI_STATUS (EFIAPI *free_pool)(void *);

    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;

    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    EFI_STATUS (EFIAPI *handle_protocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;

    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    EFI_STATUS (EFIAPI *exit_boot_services)(EFI_HANDLE, UINTN);

    void *get_next_monotonic_count;
    EFI_STATUS (EFIAPI *stall)(UINTN);
    EFI_STATUS (EFIAPI *set_watchdog_timer)(UINTN, u64, UINTN, CHAR16 *);

    void *connect_controller;
    void *disconnect_controller;

    EFI_STATUS (EFIAPI *open_protocol)(EFI_HANDLE, EFI_GUID *, void **,
                                       EFI_HANDLE, EFI_HANDLE, u32);
    void *close_protocol;
    void *open_protocol_information;

    void *protocols_per_handle;
    EFI_STATUS (EFIAPI *locate_handle_buffer)(u32, EFI_GUID *, void *, UINTN *,
                                              EFI_HANDLE **);
    EFI_STATUS (EFIAPI *locate_protocol)(EFI_GUID *, void *, void **);
    void *install_multiple_protocol_interfaces;
    void *uninstall_multiple_protocol_interfaces;

    void *calculate_crc32;

    void *copy_mem;
    void *set_mem;
    void *create_event_ex;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_GUID vendor_guid;
    void *vendor_table;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER hdr;
    CHAR16 *firmware_vendor;
    u32 firmware_revision;
    EFI_HANDLE console_in_handle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *con_in;
    EFI_HANDLE console_out_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con_out;
    EFI_HANDLE standard_error_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *std_err;
    void *runtime_services;
    EFI_BOOT_SERVICES *boot_services;
    UINTN number_of_table_entries;
    EFI_CONFIGURATION_TABLE *configuration_table;
} EFI_SYSTEM_TABLE;

/* Where the firmware leaves the ACPI pointer. Without this the kernel would
   have to search low memory for it, which a UEFI machine need not put there. */
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

static inline int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    if (a->data1 != b->data1 || a->data2 != b->data2 || a->data3 != b->data3)
        return 0;
    for (int i = 0; i < 8; i++)
        if (a->data4[i] != b->data4[i]) return 0;
    return 1;
}
