/* Windows' own H.264 encoder and decoder, for making test streams and the
 * answers to them. A host tool: it runs on the machine that builds zelr,
 * never on zelr, and nothing it makes is code.
 *
 *   mfh264 encode IN.yuv W H FPS PROFILE BFRAMES GOP QP OUT.mp4
 *       IN is raw I420 frames; PROFILE is base, main or high. The encoder
 *       is asked for constant QP, so a test has detail at every rate.
 *   mfh264 decode IN OUT.yuv
 *       IN is anything Windows can play (.mp4, .ts); OUT is raw I420
 *       frames in display order, at the size the stream says it shows.
 *       Prints "frames N size W H".
 *
 * Built by tools/genh264.py with zig cc; Media Foundation through its C
 * interfaces.
 */
#define COBJMACROS
#define CINTERFACE
#define INITGUID
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the import library for GUIDs would hold; spelled out here. */
DEFINE_GUID(MY_MFMediaType_Video, 0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(MY_MFVideoFormat_H264, 0x34363248, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(MY_MFVideoFormat_I420, 0x30323449, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(MY_MFVideoFormat_IYUV, 0x56555949, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(MY_MF_MT_MAJOR_TYPE, 0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f);
DEFINE_GUID(MY_MF_MT_SUBTYPE, 0xf7e34c9a, 0x42e8, 0x4714, 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5);
DEFINE_GUID(MY_MF_MT_AVG_BITRATE, 0x20332624, 0xfb0d, 0x4d9e, 0xbd, 0x0d, 0xcb, 0xf6, 0x78, 0x6c, 0x10, 0x2e);
DEFINE_GUID(MY_MF_MT_INTERLACE_MODE, 0xe2724bb8, 0xe676, 0x4806, 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd);
DEFINE_GUID(MY_MF_MT_FRAME_SIZE, 0x1652c33d, 0xd6b2, 0x4012, 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d);
DEFINE_GUID(MY_MF_MT_FRAME_RATE, 0xc459a2e8, 0x3d2c, 0x4e44, 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0);
DEFINE_GUID(MY_MF_MT_PIXEL_ASPECT_RATIO, 0xc6376a1e, 0x8d0a, 0x4027, 0xbe, 0x45, 0x6d, 0x9a, 0x0a, 0xd3, 0x9b, 0xb6);
DEFINE_GUID(MY_MF_MT_MPEG2_PROFILE, 0xad76a80b, 0x2d5c, 0x4e0b, 0xb3, 0x75, 0x64, 0xe5, 0x20, 0x13, 0x70, 0x36);
DEFINE_GUID(MY_MF_MT_DEFAULT_STRIDE, 0x644b4e48, 0x1e02, 0x4516, 0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6);
DEFINE_GUID(MY_MF_MT_MINIMUM_DISPLAY_APERTURE, 0xd7388766, 0x18fe, 0x48c6, 0xa1, 0x77, 0xee, 0x89, 0x48, 0x67, 0xc8, 0xc4);
DEFINE_GUID(MY_MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 0xa634a91c, 0x822b, 0x41b9, 0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0);
DEFINE_GUID(MY_MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 0xfb394f3d, 0xccf1, 0x42ee, 0xbb, 0xb3, 0xf9, 0xb8, 0x45, 0xd5, 0x68, 0x1d);
DEFINE_GUID(MY_IID_ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda);
DEFINE_GUID(MY_GUID_NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
DEFINE_GUID(MY_CODECAPI_AVEncMPVDefaultBPictureCount, 0x8d390aac, 0xdc5c, 0x4200, 0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2);
DEFINE_GUID(MY_CODECAPI_AVEncMPVGOPSize, 0x95f31b26, 0x95a4, 0x41aa, 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1);
DEFINE_GUID(MY_CODECAPI_AVEncCommonRateControlMode, 0x1c0608e9, 0x370c, 0x4710, 0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23);
DEFINE_GUID(MY_CODECAPI_AVEncVideoEncodeQP, 0x2cb5696b, 0x23fb, 0x4ce1, 0xa0, 0xf9, 0xef, 0x5b, 0x90, 0xfd, 0x55, 0xca);
DEFINE_GUID(MY_CODECAPI_AVEncH264CABACEnable, 0xee6cad62, 0xd305, 0x4248, 0xa5, 0x0e, 0xe1, 0xb2, 0x55, 0xf7, 0xca, 0xf8);

#define OK(x, what) do { HRESULT hr_ = (x); if (FAILED(hr_)) { fprintf(stderr, "%s failed: 0x%08lx\n", what, (unsigned long)hr_); exit(1); } } while (0)

static void set_size(IMFMediaType *t, GUID key, UINT32 hi, UINT32 lo) {
    OK(IMFMediaType_SetUINT64(t, &key, ((UINT64)hi << 32) | lo), "set a size");
}

static void set_codec_u32(ICodecAPI *api, const GUID *key, UINT32 v, const char *what) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    HRESULT hr = ICodecAPI_SetValue(api, key, &var);
    if (FAILED(hr)) fprintf(stderr, "the encoder would not take %s = %u (0x%08lx)\n", what, v, (unsigned long)hr);
}

static void set_codec_u64(ICodecAPI *api, const GUID *key, ULONGLONG v, const char *what) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI8;
    var.ullVal = v;
    HRESULT hr = ICodecAPI_SetValue(api, key, &var);
    if (FAILED(hr)) fprintf(stderr, "the encoder would not take %s (0x%08lx)\n", what, (unsigned long)hr);
}

static void set_codec_bool(ICodecAPI *api, const GUID *key, int v, const char *what) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    HRESULT hr = ICodecAPI_SetValue(api, key, &var);
    if (FAILED(hr)) fprintf(stderr, "the encoder would not take %s (0x%08lx)\n", what, (unsigned long)hr);
}

static int encode(int argc, char **argv) {
    if (argc < 11) { fprintf(stderr, "encode IN W H FPS PROFILE BFRAMES GOP QP OUT\n"); return 2; }
    const char *in = argv[2];
    UINT32 w = (UINT32)atoi(argv[3]), h = (UINT32)atoi(argv[4]), fps = (UINT32)atoi(argv[5]);
    const char *prof = argv[6];
    UINT32 bframes = (UINT32)atoi(argv[7]), gop = (UINT32)atoi(argv[8]), qp = (UINT32)atoi(argv[9]);
    UINT32 profile = !strcmp(prof, "base") ? eAVEncH264VProfile_Base
                   : !strcmp(prof, "high") ? eAVEncH264VProfile_High : eAVEncH264VProfile_Main;
    wchar_t out[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[10], -1, out, MAX_PATH);

    FILE *f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", in); return 1; }

    IMFAttributes *attrs = NULL;
    OK(MFCreateAttributes(&attrs, 1), "MFCreateAttributes");
    OK(IMFAttributes_SetUINT32(attrs, &MY_MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE), "no hardware");
    IMFSinkWriter *wr = NULL;
    OK(MFCreateSinkWriterFromURL(out, NULL, attrs, &wr), "MFCreateSinkWriterFromURL");

    IMFMediaType *ot = NULL, *it = NULL;
    OK(MFCreateMediaType(&ot), "MFCreateMediaType");
    IMFMediaType_SetGUID(ot, &MY_MF_MT_MAJOR_TYPE, &MY_MFMediaType_Video);
    IMFMediaType_SetGUID(ot, &MY_MF_MT_SUBTYPE, &MY_MFVideoFormat_H264);
    IMFMediaType_SetUINT32(ot, &MY_MF_MT_AVG_BITRATE, 2000000);
    IMFMediaType_SetUINT32(ot, &MY_MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT32(ot, &MY_MF_MT_MPEG2_PROFILE, profile);
    set_size(ot, MY_MF_MT_FRAME_SIZE, w, h);
    set_size(ot, MY_MF_MT_FRAME_RATE, fps, 1);
    set_size(ot, MY_MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    OK(IMFSinkWriter_AddStream(wr, ot, &stream), "AddStream");

    OK(MFCreateMediaType(&it), "MFCreateMediaType");
    IMFMediaType_SetGUID(it, &MY_MF_MT_MAJOR_TYPE, &MY_MFMediaType_Video);
    IMFMediaType_SetGUID(it, &MY_MF_MT_SUBTYPE, &MY_MFVideoFormat_IYUV);
    IMFMediaType_SetUINT32(it, &MY_MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    set_size(it, MY_MF_MT_FRAME_SIZE, w, h);
    set_size(it, MY_MF_MT_FRAME_RATE, fps, 1);
    set_size(it, MY_MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    /* The encoder takes its settings when its types are set, as attributes
       keyed by the codec API's names; set later they are ignored. */
    IMFAttributes *enc = NULL;
    OK(MFCreateAttributes(&enc, 6), "MFCreateAttributes");
    IMFAttributes_SetUINT32(enc, &MY_CODECAPI_AVEncMPVDefaultBPictureCount, bframes);
    IMFAttributes_SetUINT32(enc, &MY_CODECAPI_AVEncMPVGOPSize, gop);
    IMFAttributes_SetUINT32(enc, &MY_CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
    IMFAttributes_SetUINT64(enc, &MY_CODECAPI_AVEncVideoEncodeQP,
                            (UINT64)qp | (UINT64)qp << 16 | (UINT64)qp << 32 | (UINT64)qp << 48);
    IMFAttributes_SetUINT32(enc, &MY_CODECAPI_AVEncH264CABACEnable, profile != eAVEncH264VProfile_Base);
    OK(IMFSinkWriter_SetInputMediaType(wr, stream, it, enc), "SetInputMediaType");

    ICodecAPI *api = NULL;
    if (SUCCEEDED(IMFSinkWriter_GetServiceForStream(wr, stream, &MY_GUID_NULL, &MY_IID_ICodecAPI, (void **)&api)) && api) {
        set_codec_u32(api, &MY_CODECAPI_AVEncMPVDefaultBPictureCount, bframes, "B pictures");
        set_codec_u32(api, &MY_CODECAPI_AVEncMPVGOPSize, gop, "the GOP size");
        set_codec_u32(api, &MY_CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality, "constant quality");
        /* The same QP for every kind of frame, in each 16-bit field. */
        set_codec_u64(api, &MY_CODECAPI_AVEncVideoEncodeQP,
                      (ULONGLONG)qp | (ULONGLONG)qp << 16 | (ULONGLONG)qp << 32 | (ULONGLONG)qp << 48, "the QP");
        if (profile != eAVEncH264VProfile_Base) set_codec_bool(api, &MY_CODECAPI_AVEncH264CABACEnable, 1, "CABAC");
    } else {
        fprintf(stderr, "no ICodecAPI on the encoder; it keeps its own settings\n");
    }

    OK(IMFSinkWriter_BeginWriting(wr), "BeginWriting");
    DWORD fbytes = w * h * 3 / 2;
    LONGLONG dur = 10000000LL / fps, t = 0;
    int n = 0;
    for (;;) {
        IMFMediaBuffer *buf = NULL;
        OK(MFCreateMemoryBuffer(fbytes, &buf), "MFCreateMemoryBuffer");
        BYTE *p = NULL;
        IMFMediaBuffer_Lock(buf, &p, NULL, NULL);
        size_t got = fread(p, 1, fbytes, f);
        IMFMediaBuffer_Unlock(buf);
        if (got != fbytes) { IMFMediaBuffer_Release(buf); break; }
        IMFMediaBuffer_SetCurrentLength(buf, fbytes);
        IMFSample *s = NULL;
        OK(MFCreateSample(&s), "MFCreateSample");
        IMFSample_AddBuffer(s, buf);
        IMFSample_SetSampleTime(s, t);
        IMFSample_SetSampleDuration(s, dur);
        OK(IMFSinkWriter_WriteSample(wr, stream, s), "WriteSample");
        IMFSample_Release(s);
        IMFMediaBuffer_Release(buf);
        t += dur;
        n++;
    }
    OK(IMFSinkWriter_Finalize(wr), "Finalize");
    fclose(f);
    printf("frames %d\n", n);
    return 0;
}

static int decode(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "decode IN OUT\n"); return 2; }
    wchar_t in[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, in, MAX_PATH);
    FILE *f = fopen(argv[3], "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }

    IMFAttributes *attrs = NULL;
    OK(MFCreateAttributes(&attrs, 2), "MFCreateAttributes");
    IMFAttributes_SetUINT32(attrs, &MY_MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    IMFAttributes_SetUINT32(attrs, &MY_MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
    IMFSourceReader *rd = NULL;
    OK(MFCreateSourceReaderFromURL(in, attrs, &rd), "MFCreateSourceReaderFromURL");
    DWORD vs = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(rd, vs, TRUE);

    IMFMediaType *t = NULL;
    OK(MFCreateMediaType(&t), "MFCreateMediaType");
    IMFMediaType_SetGUID(t, &MY_MF_MT_MAJOR_TYPE, &MY_MFMediaType_Video);
    IMFMediaType_SetGUID(t, &MY_MF_MT_SUBTYPE, &MY_MFVideoFormat_I420);
    OK(IMFSourceReader_SetCurrentMediaType(rd, vs, NULL, t), "SetCurrentMediaType I420");

    UINT32 w = 0, h = 0, sw = 0, sh = 0, ox = 0, oy = 0;
    int frames = 0;
    for (;;) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample *s = NULL;
        OK(IMFSourceReader_ReadSample(rd, vs, 0, &idx, &flags, &ts, &s), "ReadSample");
        if (flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) || !w) {
            IMFMediaType *cur = NULL;
            if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(rd, vs, &cur))) {
                UINT64 sz = 0;
                IMFMediaType_GetUINT64(cur, &MY_MF_MT_FRAME_SIZE, &sz);
                w = (UINT32)(sz >> 32); h = (UINT32)sz;
                MFVideoArea area;
                UINT32 got = 0;
                sw = w; sh = h; ox = oy = 0;
                if (SUCCEEDED(IMFMediaType_GetBlob(cur, &MY_MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), &got))) {
                    ox = (UINT32)area.OffsetX.value; oy = (UINT32)area.OffsetY.value;
                    sw = (UINT32)area.Area.cx; sh = (UINT32)area.Area.cy;
                }
                IMFMediaType_Release(cur);
            }
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        IMFMediaBuffer *buf = NULL;
        OK(IMFSample_ConvertToContiguousBuffer(s, &buf), "ConvertToContiguousBuffer");
        BYTE *p = NULL;
        DWORD len = 0;
        IMFMediaBuffer_Lock(buf, &p, NULL, &len);
        /* I420 at the decoder's (coded, padded) size: write the part that is
           shown, plane by plane. */
        if (len >= w * h * 3 / 2) {
            for (UINT32 y = 0; y < sh; y++) fwrite(p + (size_t)(oy + y) * w + ox, 1, sw, f);
            BYTE *u = p + (size_t)w * h, *v = u + (size_t)(w / 2) * (h / 2);
            for (UINT32 y = 0; y < sh / 2; y++) fwrite(u + (size_t)(oy / 2 + y) * (w / 2) + ox / 2, 1, sw / 2, f);
            for (UINT32 y = 0; y < sh / 2; y++) fwrite(v + (size_t)(oy / 2 + y) * (w / 2) + ox / 2, 1, sw / 2, f);
            frames++;
        }
        IMFMediaBuffer_Unlock(buf);
        IMFMediaBuffer_Release(buf);
        IMFSample_Release(s);
    }
    fclose(f);
    printf("frames %d size %u %u\n", frames, sw, sh);
    return 0;
}

int main(int argc, char **argv) {
    OK(CoInitializeEx(NULL, COINIT_MULTITHREADED), "CoInitializeEx");
    OK(MFStartup(MF_VERSION, MFSTARTUP_FULL), "MFStartup");
    int rc = 2;
    if (argc > 1 && !strcmp(argv[1], "encode")) rc = encode(argc, argv);
    else if (argc > 1 && !strcmp(argv[1], "decode")) rc = decode(argc, argv);
    else fprintf(stderr, "mfh264 encode ... | decode ...\n");
    MFShutdown();
    return rc;
}
