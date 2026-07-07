#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>

/* Default log file name. It is created under %TEMP% (see get_log_path) unless the
 * USBPC_STUB_LOG environment variable overrides the full path. Logging is quiet by
 * default; set USBPC_VERBOSE=1 for full per-call/per-transfer tracing. */
#define USBPC_STUB_LOG_NAME "usbpc_stub_log.txt"

#define EP_OUT 0x03
#define EP_IN  0x83
#define AB_READ_UNIT 0x4000u
#define MAX_VFILES 128
#define USB_TIMEOUT_MS 5000u

typedef int (__cdecl *USBPC_STUB_FN)(DWORD, DWORD, DWORD, DWORD);
typedef int (__cdecl *USBPC_HELPER_FN)(DWORD, DWORD, DWORD, DWORD);

static const GUID PASORAMA_WINUSB_GUID =
    { 0x6ec98d31, 0x3f8b, 0x4c1f, { 0x9e, 0x6c, 0x3f, 0x6f, 0x5b, 0x0a, 0x1a, 0x11 } };

typedef struct USB_LINK {
    HANDLE dev;
    WINUSB_INTERFACE_HANDLE winusb;
    unsigned char seq;
    int opened;
    int handshook;
} USB_LINK;

typedef struct VFILE {
    int used;
    int handle;
    DWORD remote_handle;
    WCHAR path[512];
    DWORD desired_pos;
    DWORD stream_pos;
    int eof;
} VFILE;

static USB_LINK g_link = { INVALID_HANDLE_VALUE, NULL, 0, 0, 0 };
static VFILE g_files[MAX_VFILES];
static int g_next_handle = 1;
static CRITICAL_SECTION g_lock;
static int g_lock_ready = 0;
/* Verbose per-transfer / per-call logging. OFF by default: it opened+closed the
 * log file on every USB transfer (~10 file ops per index9, ~4.8MB per search) -
 * pure overhead once the protocol is working. Errors are still always logged.
 * Set env USBPC_VERBOSE=1 to re-enable full tracing for debugging. */
static int g_verbose = 0;
/* Block cache ON by default. It is now keyed by file path (immutable identity of
 * a read-only index file), which is correct by construction - see the cache block
 * below. The earlier remote_handle-keyed cache could serve another file's block
 * and broke multi-dictionary search / figures; that class of bug is gone. Escape
 * hatch: set USBPC_NOCACHE=1 to disable it without a rebuild. */
static int g_cache_enabled = 1;

static HANDLE g_keepalive_thread = NULL;
static HANDLE g_keepalive_event = NULL;
static volatile LONG g_keepalive_stop = 0;
static volatile LONG g_link_broken = 0;

/* --- index9 block cache -------------------------------------------------
 * Search re-reads the same (file, block) ~50% of the time, so caching the
 * fixed-size blocks avoids a lot of USB traffic.
 *
 * Correctness is by construction: the key is (path-hash, offset), NOT
 * (remote_handle, offset). The index9 files are read-only dictionary indices, so
 * a given (file path, byte offset) always holds the same bytes for the whole
 * session - the content is IMMUTABLE. Keying on the file path therefore can never
 * serve one file's block for another, and needs NO invalidation logic.
 *
 * (The earlier version keyed on remote_handle - the number the device returns
 * from open - and relied on invalidating it on close/reopen. That number is
 * reused across different files, and a gap in the invalidation let a stale block
 * be served for a different file: the corrupted index lookup made the app compute
 * a bogus block address, the read came back empty, and multi-dictionary search
 * and the first figure in an entry silently failed. Path-keying removes that
 * whole failure class.)
 *
 * Only the read-only index9 path uses this cache; writable user files go through
 * index4 and are never cached. All access is under g_lock. */
#define BLOCK_CACHE_ENTRIES 2048
#define BLOCK_CACHE_BYTES 4096u
typedef struct BLOCK_CACHE_ENTRY {
    int valid;
    unsigned __int64 key;   /* FNV-1a hash of the file path = immutable identity */
    DWORD offset;
    DWORD len;
    DWORD lru;
    unsigned char data[BLOCK_CACHE_BYTES];
} BLOCK_CACHE_ENTRY;
static BLOCK_CACHE_ENTRY g_block_cache[BLOCK_CACHE_ENTRIES];
static DWORD g_cache_tick = 0;

/* 64-bit FNV-1a over the UTF-16 path. Collisions across a session's handful of
 * dictionary files are astronomically unlikely. */
static unsigned __int64 path_key(const WCHAR *p)
{
    unsigned __int64 h = 1469598103934665603ULL;
    int i;
    for (i = 0; p[i] != 0 && i < 512; i++) {
        h ^= (unsigned __int64)(unsigned short)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static BLOCK_CACHE_ENTRY *cache_lookup(unsigned __int64 key, DWORD offset)
{
    int i;
    if (!g_cache_enabled) {
        return NULL;
    }
    for (i = 0; i < BLOCK_CACHE_ENTRIES; i++) {
        if (g_block_cache[i].valid &&
            g_block_cache[i].key == key &&
            g_block_cache[i].offset == offset) {
            return &g_block_cache[i];
        }
    }
    return NULL;
}

static void cache_store(unsigned __int64 key, DWORD offset,
                        const unsigned char *data, DWORD len)
{
    int i;
    int victim = 0;
    DWORD best = 0xffffffffu;
    if (!g_cache_enabled) {
        return;
    }
    if (len == 0u || len > BLOCK_CACHE_BYTES) {
        return;
    }
    for (i = 0; i < BLOCK_CACHE_ENTRIES; i++) {
        if (!g_block_cache[i].valid) {
            victim = i;
            break;
        }
        if (g_block_cache[i].lru < best) {
            best = g_block_cache[i].lru;
            victim = i;
        }
    }
    g_block_cache[victim].valid = 1;
    g_block_cache[victim].key = key;
    g_block_cache[victim].offset = offset;
    g_block_cache[victim].len = len;
    g_block_cache[victim].lru = ++g_cache_tick;
    CopyMemory(g_block_cache[victim].data, data, len);
}

static void cache_clear_all(void)
{
    int i;
    for (i = 0; i < BLOCK_CACHE_ENTRIES; i++) {
        g_block_cache[i].valid = 0;
    }
}

static void append_text_log(const char *text);

static void bytes_to_hex(const unsigned char *data, DWORD len, char *out, DWORD outsz)
{
    static const char hex[] = "0123456789abcdef";
    DWORD i;
    DWORD n = 0;
    if (outsz == 0) {
        return;
    }
    for (i = 0; i < len && n + 2 < outsz; i++) {
        out[n++] = hex[(data[i] >> 4) & 0x0f];
        out[n++] = hex[data[i] & 0x0f];
    }
    out[n] = '\0';
}

static const char *return_kind(int index)
{
    switch (index) {
    case 0: return "init";
    case 2:
    case 3: return "open";
    case 4: return "read";
    case 5: return "write";
    case 6: return "seek";
    case 8: return "close";
    case 20: return "get_unit_version";
    case 21: return "connect_unit";
    case 22: return "disconnect_unit";
    case 23: return "session_init";
    default: return "fail";
    }
}

static void get_log_path(char *path, DWORD pathsz)
{
    path[0] = '\0';
    GetEnvironmentVariableA("USBPC_STUB_LOG", path, pathsz);
    if (path[0] == '\0') {
        /* Default: %TEMP%\usbpc_stub_log.txt (no hard-coded user path). */
        char tmp[MAX_PATH];
        DWORD n = GetTempPathA((DWORD)sizeof(tmp), tmp);
        if (n == 0u || n >= sizeof(tmp)) {
            lstrcpynA(path, USBPC_STUB_LOG_NAME, (int)pathsz);
            return;
        }
        _snprintf_s(path, pathsz, _TRUNCATE, "%s%s", tmp, USBPC_STUB_LOG_NAME);
    }
}

static void append_text_log(const char *text)
{
    char path[MAX_PATH];
    HANDLE file;
    DWORD written;
    DWORD len;

    get_log_path(path, (DWORD)sizeof(path));
    file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    len = (DWORD)lstrlenA(text);
    (void)WriteFile(file, text, len, &written, NULL);
    CloseHandle(file);
}

static void append_protocol_log(const char *tag, const unsigned char *data, DWORD len)
{
    SYSTEMTIME st;
    char hex[256];
    if (!g_verbose) {
        return;
    }
    char line[384];
    DWORD dump_len = len;
    if (dump_len > 96u) {
        dump_len = 96u;
    }
    bytes_to_hex(data, dump_len, hex, (DWORD)sizeof(hex));
    GetLocalTime(&st);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "%04u-%02u-%02u %02u:%02u:%02u.%03u %s len=%lu hex=%s%s\r\n",
                (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                (unsigned)st.wMilliseconds, tag, (unsigned long)len, hex,
                (dump_len < len) ? "..." : "");
    append_text_log(line);
}

static void append_error_log(const char *where, DWORD err)
{
    char line[256];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "ERROR %s gle=%lu\r\n", where, (unsigned long)err);
    append_text_log(line);
}

static void safe_dump_ptr(DWORD addr, char *out, size_t outsz)
{
    out[0] = '\0';
    __try {
        const unsigned char *p = (const unsigned char *)addr;
        char ascii[40];
        char wide[40];
        int i;
        if (addr == 0) {
            lstrcpynA(out, "(null)", (int)outsz);
            return;
        }
        for (i = 0; i < 32; i++) {
            unsigned char c = p[i];
            ascii[i] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        ascii[32] = '\0';
        for (i = 0; i < 16; i++) {
            unsigned short wc = ((const unsigned short *)p)[i];
            unsigned char lo = (unsigned char)(wc & 0xffu);
            wide[i] = (lo >= 32 && lo < 127) ? (char)lo : '.';
        }
        wide[16] = '\0';
        _snprintf_s(out, outsz, _TRUNCATE, "A[%s] W[%s]", ascii, wide);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(out, "(unreadable)", (int)outsz);
    }
}

static void append_call_log(int index, DWORD a0, DWORD a1, DWORD a2, DWORD a3, int ret)
{
    SYSTEMTIME st;
    char line[512];
    char dump0[64];
    char dump1[64];

    if (!g_verbose) {
        return;
    }
    dump0[0] = '\0';
    dump1[0] = '\0';
    if (index == 2 || index == 3 || index == 11) {
        safe_dump_ptr(a0, dump0, sizeof(dump0));
    }

    GetLocalTime(&st);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "%04u-%02u-%02u %02u:%02u:%02u.%03u index=%02d ret=%d kind=%s args=[%08lX %08lX %08lX %08lX] p0=%s p1=%s\r\n",
                (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                (unsigned)st.wMilliseconds, index, ret, return_kind(index),
                (unsigned long)a0, (unsigned long)a1, (unsigned long)a2, (unsigned long)a3,
                dump0, dump1);
    append_text_log(line);
}

static int usb_write_all(const unsigned char *data, DWORD len)
{
    ULONG written = 0;
    if (!g_link.winusb) {
        return -1;
    }
    append_protocol_log("USB OUT", data, len);
    if (!WinUsb_WritePipe(g_link.winusb, EP_OUT, (PUCHAR)data, (ULONG)len, &written, NULL)) {
        append_error_log("WinUsb_WritePipe", GetLastError());
        InterlockedExchange(&g_link_broken, 1);
        return -1;
    }
    if (written != len) {
        append_error_log("WinUsb_WritePipe short", written);
        InterlockedExchange(&g_link_broken, 1);
        return -1;
    }
    /* No post-write Sleep: the device paces itself via the ACK/read that follows
     * every command (WinUsb_ReadPipe blocks until data arrives), so an
     * unconditional delay here only slowed the search path - each index9 issued
     * two writes (seek + read) x 10ms = ~20ms of pure sleep per read. A small
     * guard is kept only at the command->payload transition (see ab_cmd /
     * replay_handshake) where the device must parse the header before the body. */
    return 0;
}

static int usb_read_packet(unsigned char *buf, DWORD cap, DWORD *got)
{
    ULONG read = 0;
    if (!WinUsb_ReadPipe(g_link.winusb, EP_IN, buf, (ULONG)cap, &read, NULL)) {
        append_error_log("WinUsb_ReadPipe", GetLastError());
        InterlockedExchange(&g_link_broken, 1);
        return -1;
    }
    *got = (DWORD)read;
    append_protocol_log("USB IN", buf, *got);
    return 0;
}

static DWORD read_le32(const unsigned char *p)
{
    return ((DWORD)p[0]) | (((DWORD)p[1]) << 8) | (((DWORD)p[2]) << 16) | (((DWORD)p[3]) << 24);
}

static int read_until_ack(unsigned char expect_seq, unsigned char *data, DWORD cap,
                          DWORD *data_len, DWORD *ack_len, int collect_data)
{
    unsigned char pkt[4096];
    DWORD got;
    DWORD total = 0;
    DWORD i;

    for (i = 0; i < 100000u; i++) {
        if (usb_read_packet(pkt, sizeof(pkt), &got) != 0) {
            return -1;
        }
        if (got == 12u && pkt[0] == 0xAC) {
            if (pkt[1] != expect_seq) {
                append_error_log("ACK seq mismatch", pkt[1]);
            }
            *ack_len = read_le32(pkt + 4);
            *data_len = total;
            return 0;
        }
        if (collect_data && got > 0u) {
            DWORD take = got;
            if (take > cap - total) {
                take = cap - total;
            }
            if (take > 0u) {
                CopyMemory(data + total, pkt, take);
                total += take;
            }
        }
    }
    append_error_log("read_until_ack timeout", expect_seq);
    return -1;
}

static void put_le32(unsigned char *p, DWORD v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static int ab_cmd(DWORD length, DWORD opcode, const unsigned char tail[8],
                  const unsigned char *payload, DWORD payload_len,
                  unsigned char *data, DWORD data_cap, DWORD *data_len,
                  DWORD *ack_len, int collect_data)
{
    unsigned char pkt[24];
    unsigned char seq;
    ZeroMemory(pkt, sizeof(pkt));
    g_link.seq = (unsigned char)((g_link.seq + 1u) & 0xffu);
    seq = g_link.seq;
    pkt[0] = 0xAB;
    pkt[1] = seq;
    pkt[2] = 0x80;
    pkt[3] = 0x10;
    put_le32(pkt + 4, length);
    put_le32(pkt + 8, opcode);
    put_le32(pkt + 12, length);
    CopyMemory(pkt + 16, tail, 8);
    if (usb_write_all(pkt, sizeof(pkt)) != 0) {
        return -1;
    }
    if (payload && payload_len > 0u) {
        Sleep(1); /* let the device parse the 24B header before the payload body */
        if (usb_write_all(payload, payload_len) != 0) {
            return -1;
        }
    }
    return read_until_ack(seq, data, data_cap, data_len, ack_len, collect_data);
}

static DWORD WINAPI keepalive_thread_proc(LPVOID param)
{
    (void)param;
    append_text_log("KEEPALIVE thread started\r\n");
    while (InterlockedOr(&g_keepalive_stop, 0) == 0) {
        if (WaitForSingleObject(g_keepalive_event, 1000) == WAIT_OBJECT_0) {
            break;
        }
        if (InterlockedOr(&g_keepalive_stop, 0) != 0) {
            break;
        }

        if (g_lock_ready) {
            if (TryEnterCriticalSection(&g_lock)) {
                __try {
                    if (g_link.winusb && g_link.handshook) {
                        static const unsigned char keepalive_tail[8] = { 0xfe, 0xff, 0xff, 0xff, 0x00, 0x00, 0xa0, 0x0f };
                        unsigned char dummy[256];
                        DWORD data_len = 0;
                        DWORD ack_len = 0;
                        char line[256];

                        _snprintf_s(line, sizeof(line), _TRUNCATE, "KEEPALIVE sending opcode=0x75FC7543 length=15699\r\n");
                        append_text_log(line);

                        if (ab_cmd(15699, 0x75FC7543u, keepalive_tail, NULL, 0,
                                   dummy, sizeof(dummy), &data_len, &ack_len, 0) == 0) {
                            _snprintf_s(line, sizeof(line), _TRUNCATE, "KEEPALIVE ack ok: data_len=%lu ack_len=%lu\r\n", data_len, ack_len);
                            append_text_log(line);
                        } else {
                            append_text_log("KEEPALIVE sending failed\r\n");
                        }
                    }
                } __finally {
                    LeaveCriticalSection(&g_lock);
                }
            } else {
                append_text_log("KEEPALIVE skipped (lock busy)\r\n");
            }
        }
    }
    append_text_log("KEEPALIVE thread exiting\r\n");
    return 0;
}

static void start_keepalive_thread(void)
{
    if (g_keepalive_thread != NULL) {
        return;
    }
    g_keepalive_stop = 0;
    g_keepalive_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (g_keepalive_event == NULL) {
        append_error_log("CreateEvent keepalive", GetLastError());
        return;
    }
    g_keepalive_thread = CreateThread(NULL, 0, keepalive_thread_proc, NULL, 0, NULL);
    if (g_keepalive_thread == NULL) {
        append_error_log("CreateThread keepalive", GetLastError());
        CloseHandle(g_keepalive_event);
        g_keepalive_event = NULL;
    }
}

static void stop_keepalive_thread(void)
{
    if (g_keepalive_thread == NULL) {
        return;
    }
    InterlockedExchange(&g_keepalive_stop, 1);
    if (g_keepalive_event != NULL) {
        SetEvent(g_keepalive_event);
    }
    if (WaitForSingleObject(g_keepalive_thread, 2000) == WAIT_TIMEOUT) {
        append_text_log("WARNING: KEEPALIVE thread stop timed out\r\n");
    }
    CloseHandle(g_keepalive_thread);
    g_keepalive_thread = NULL;
    if (g_keepalive_event != NULL) {
        CloseHandle(g_keepalive_event);
        g_keepalive_event = NULL;
    }
}

static int replay_handshake(void)
{
    static const unsigned char seq1[] = { 0xab,0x01,0x80,0x10,0x53,0x3d,0x00,0x00,0x43,0x00,0x00,0x00,0x53,0x3d,0x00,0x00,0xf0,0xd0,0x5c,0x76,0xb5,0x53,0xaa,0x02 };
    static const unsigned char seq2[] = { 0xab,0x02,0x80,0x10,0x64,0x00,0x00,0x00,0x40,0x00,0x0f,0x00,0x64,0x00,0x00,0x00,0xd8,0xb1,0xaa,0x02,0xa4,0xf9,0x14,0x00 };
    static const unsigned char seq3[] = { 0xab,0x03,0x80,0x10,0xd9,0xfd,0xba,0x00,0x42,0x00,0x00,0x00,0xd9,0xfd,0xba,0x00,0xd0,0xf9,0x14,0x00,0x6e,0x2b,0xc6,0x00 };
    static const unsigned char seq4[] = { 0xab,0x04,0x80,0x10,0x1e,0x00,0x00,0x00,0x25,0x80,0x00,0x00,0x1e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
    static const unsigned char seq4_payload[] = "/mnt/sdcard/Download/DeviceID";
    static const unsigned char seq5[] = { 0xab,0x05,0x80,0x10,0x7e,0x00,0x00,0x00,0x26,0x00,0x07,0x00,0x7e,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x04,0x00,0x00,0x00 };
    static const unsigned char seq6[] = { 0xab,0x06,0x80,0x10,0x00,0x00,0x00,0x00,0x29,0x80,0x07,0x00,0x00,0x00,0x00,0x00,0x14,0x2e,0xc6,0x00,0x14,0x83,0xc0,0x00 };
    static const unsigned char seq7[] = { 0xab,0x07,0x80,0x10,0x60,0x9f,0x09,0x02,0x3a,0x80,0xc3,0x00,0x60,0x9f,0x09,0x02,0x4b,0x00,0x4a,0x00,0x4b,0x00,0x49,0x00 };
    const unsigned char *frames[7] = { seq1, seq2, seq3, seq4, seq5, seq6, seq7 };
    const DWORD sizes[7] = { sizeof(seq1), sizeof(seq2), sizeof(seq3), sizeof(seq4), sizeof(seq5), sizeof(seq6), sizeof(seq7) };
    DWORD i;
    unsigned char dummy[8192];
    DWORD data_len;
    DWORD ack_len;

    for (i = 0; i < 7u; i++) {
        unsigned char seq = frames[i][1];
        if (usb_write_all(frames[i], sizes[i]) != 0) {
            return -1;
        }
        if (i == 3u) {
            unsigned char payload[30];
            CopyMemory(payload, seq4_payload, 29);
            payload[29] = 0;
            Sleep(1); /* header->payload guard (see ab_cmd) */
            if (usb_write_all(payload, sizeof(payload)) != 0) {
                return -1;
            }
        }
        if (read_until_ack(seq, dummy, sizeof(dummy), &data_len, &ack_len, 1) != 0) {
            return -1;
        }
    }
    g_link.seq = 7;
    g_link.handshook = 1;
    start_keepalive_thread();
    return 0;
}

static int open_winusb_device(void)
{
    HDEVINFO info;
    SP_DEVICE_INTERFACE_DATA ifdata;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail = NULL;
    DWORD needed = 0;
    DWORD index;
    int rc = -1;

    if (g_link.opened) {
        return 0;
    }

    info = SetupDiGetClassDevsA(&PASORAMA_WINUSB_GUID, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        append_error_log("SetupDiGetClassDevs", GetLastError());
        return -1;
    }

    for (index = 0; ; index++) {
        ZeroMemory(&ifdata, sizeof(ifdata));
        ifdata.cbSize = sizeof(ifdata);
        if (!SetupDiEnumDeviceInterfaces(info, NULL, &PASORAMA_WINUSB_GUID, index, &ifdata)) {
            append_error_log("SetupDiEnumDeviceInterfaces", GetLastError());
            break;
        }
        (void)SetupDiGetDeviceInterfaceDetailA(info, &ifdata, NULL, 0, &needed, NULL);
        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
        if (!detail) {
            append_error_log("HeapAlloc detail", needed);
            break;
        }
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailA(info, &ifdata, detail, needed, NULL, NULL)) {
            append_error_log("SetupDiGetDeviceInterfaceDetail", GetLastError());
            HeapFree(GetProcessHeap(), 0, detail);
            detail = NULL;
            continue;
        }
        g_link.dev = CreateFileA(detail->DevicePath, GENERIC_WRITE | GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
        HeapFree(GetProcessHeap(), 0, detail);
        detail = NULL;
        if (g_link.dev == INVALID_HANDLE_VALUE) {
            append_error_log("CreateFile WinUSB path", GetLastError());
            continue;
        }
        if (!WinUsb_Initialize(g_link.dev, &g_link.winusb)) {
            append_error_log("WinUsb_Initialize", GetLastError());
            CloseHandle(g_link.dev);
            g_link.dev = INVALID_HANDLE_VALUE;
            continue;
        }
        {
            ULONG timeout = USB_TIMEOUT_MS;
            (void)WinUsb_SetPipePolicy(g_link.winusb, EP_IN, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
            (void)WinUsb_SetPipePolicy(g_link.winusb, EP_OUT, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
        }
        g_link.opened = 1;
        rc = 0;
        break;
    }

    SetupDiDestroyDeviceInfoList(info);
    return rc;
}

static void close_winusb_device(void)
{
    int i;
    stop_keepalive_thread();
    /* remote_handle numbering restarts after a reconnect, so any cached block
     * could collide with a different file next session - drop the whole cache. */
    cache_clear_all();
    for (i = 0; i < MAX_VFILES; i++) {
        ZeroMemory(&g_files[i], sizeof(g_files[i]));
    }
    if (g_link.winusb) {
        WinUsb_Free(g_link.winusb);
        g_link.winusb = NULL;
    }
    if (g_link.dev != INVALID_HANDLE_VALUE) {
        CloseHandle(g_link.dev);
        g_link.dev = INVALID_HANDLE_VALUE;
    }
    g_link.seq = 0;
    g_link.opened = 0;
    g_link.handshook = 0;
}

static int ensure_ready(void)
{
    /* Cold-start robustness: opening the WinUSB device path right after
     * enumeration (or just after a previous handle was released) can transiently
     * fail with ERROR_ACCESS_DENIED (gle=5), which made the very first connect -
     * and therefore the first search after launch - fail. Retry the open a few
     * times with a short backoff before giving up. */
    if (!g_link.opened) {
        int attempt;
        for (attempt = 0; attempt < 10; attempt++) {
            if (open_winusb_device() == 0) {
                break;
            }
            Sleep(200);
        }
    }
    if (open_winusb_device() != 0) {
        return -1;
    }
    if (!g_link.handshook) {
        if (replay_handshake() != 0) {
            close_winusb_device();
            return -1;
        }
    }
    return 0;
}

/* Checks the actual USB pipe health (case 23 is used both at connect time,
 * where 0 must mean success, and as CPLUsbFileApp's periodic "is alive" poll
 * via PLCommon.dll vtable+0x80 @ 0x1006fe60, which requires exactly 1 for
 * success). usb_write_all/usb_read_packet set g_link_broken on any transport
 * failure (observed in the field as WinUsb_WritePipe gle=22 after the device
 * silently drops the pipe); previously ensure_ready() alone couldn't detect
 * this because it only checks whether the WinUSB handle is still open, not
 * whether it can still transfer data, so PASORAMA kept sending requests into
 * a dead pipe until it crashed. This forces a full reconnect (device
 * close+reopen+handshake replay) when a break was observed. */
static int check_link_alive(void)
{
    if (InterlockedOr(&g_link_broken, 0) != 0) {
        append_text_log("LINK check: broken flag set, forcing reconnect\r\n");
        close_winusb_device();
        InterlockedExchange(&g_link_broken, 0);
        if (ensure_ready() != 0) {
            append_text_log("LINK check: reconnect failed\r\n");
            return -1;
        }
        append_text_log("LINK check: reconnect succeeded\r\n");
        return 0;
    }
    return ensure_ready();
}

static DWORD utf16_payload_from_wide(const WCHAR *path, unsigned char *out, DWORD outsz)
{
    DWORD chars = 0;
    __try {
        while (path[chars] != 0 && chars < 510u) {
            chars++;
        }
        if ((chars + 1u) * 2u > outsz) {
            return 0;
        }
        CopyMemory(out, path, (chars + 1u) * 2u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return (chars + 1u) * 2u;
}

static DWORD utf16_payload_from_ascii(const char *path, unsigned char *out, DWORD outsz, WCHAR *wide_out, DWORD wide_cap)
{
    DWORD chars = 0;
    __try {
        while (path[chars] != 0 && chars < 510u) {
            chars++;
        }
        if ((chars + 1u) * 2u > outsz || chars + 1u > wide_cap) {
            return 0;
        }
        {
            DWORD i;
            for (i = 0; i < chars; i++) {
                out[i * 2u] = (unsigned char)path[i];
                out[i * 2u + 1u] = 0;
                wide_out[i] = (WCHAR)(unsigned char)path[i];
            }
            out[chars * 2u] = 0;
            out[chars * 2u + 1u] = 0;
            wide_out[chars] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return (chars + 1u) * 2u;
}

static int ab_open_payload(const unsigned char *payload, DWORD payload_len,
                           DWORD *remote_handle)
{
    static const unsigned char tail[8] = { 0x00,0x00,0x00,0x00,0x3a,0x00,0x3b,0x00 };
    unsigned char dummy[4096];
    DWORD data_len;
    DWORD ack_len;
    if (ab_cmd(payload_len, 0x000C8037u, tail, payload, payload_len,
               dummy, sizeof(dummy), &data_len, &ack_len, 1) != 0) {
        return -1;
    }
    *remote_handle = ack_len;
    /* No cache invalidation needed: the block cache is keyed by file path, not by
     * remote_handle, so a reused handle number cannot alias another file's data. */
    if (g_verbose) {
        char line[128];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "OPEN remote_handle=%lu payload_len=%lu\r\n",
                    (unsigned long)ack_len, (unsigned long)payload_len);
        append_text_log(line);
    }
    return 0;
}

static VFILE *find_vfile(int handle)
{
    int i;
    for (i = 0; i < MAX_VFILES; i++) {
        if (g_files[i].used && g_files[i].handle == handle) {
            return &g_files[i];
        }
    }
    return NULL;
}

static int allocate_vfile(const WCHAR *path, DWORD remote_handle)
{
    int i;
    for (i = 0; i < MAX_VFILES; i++) {
        if (!g_files[i].used) {
            ZeroMemory(&g_files[i], sizeof(g_files[i]));
            g_files[i].used = 1;
            g_files[i].handle = g_next_handle++;
            g_files[i].remote_handle = remote_handle;
            if (g_next_handle < 1) {
                g_next_handle = 1;
            }
            lstrcpynW(g_files[i].path, path, (int)(sizeof(g_files[i].path) / sizeof(g_files[i].path[0])));
            return g_files[i].handle;
        }
    }
    return -1;
}

static int reopen_vfile(VFILE *vf)
{
    unsigned char payload[1024];
    DWORD remote_handle;
    DWORD payload_len = utf16_payload_from_wide(vf->path, payload, sizeof(payload));
    if (payload_len == 0u) {
        return -1;
    }
    if (ab_open_payload(payload, payload_len, &remote_handle) != 0) {
        return -1;
    }
    vf->remote_handle = remote_handle;
    vf->stream_pos = 0;
    vf->eof = 0;
    return 0;
}

static int ab_read_chunk(const VFILE *vf, unsigned char *out, DWORD out_cap,
                         DWORD *valid)
{
    unsigned char tail[8] = { 0,0,0,0,0xff,0xff,0xff,0xff };
    DWORD data_len;
    DWORD ack_len;
    char line[128];
    put_le32(tail, vf->remote_handle);
    if (g_verbose) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "READ local_handle=%d remote_handle=%lu request=%lu\r\n",
                    vf->handle, (unsigned long)vf->remote_handle,
                    (unsigned long)AB_READ_UNIT);
        append_text_log(line);
    }
    if (ab_cmd(AB_READ_UNIT, 0x000D0037u, tail, NULL, 0,
               out, out_cap, &data_len, &ack_len, 1) != 0) {
        return -1;
    }
    if (ack_len > data_len) {
        append_error_log("READ ack larger than data", ack_len);
        ack_len = data_len;
    }
    *valid = ack_len;
    return 0;
}

static int ab_seek(VFILE *vf, LONG offset, DWORD origin, DWORD *newpos)
{
    unsigned char tail[8] = { 0,0,0,0,0,0,0,0 };
    DWORD data_len;
    DWORD ack_len;
    char line[192];

    if (vf == NULL || newpos == NULL) {
        return -1;
    }
    put_le32(tail, vf->remote_handle);
    put_le32(tail + 4, origin);
    if (g_verbose) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "AB SEEK remote_handle=%lu offset=%ld origin=%lu\r\n",
                    (unsigned long)vf->remote_handle, (long)offset,
                    (unsigned long)origin);
        append_text_log(line);
    }
    if (ab_cmd((DWORD)offset, 0x00138037u, tail, NULL, 0,
               NULL, 0, &data_len, &ack_len, 0) != 0) {
        append_text_log("AB SEEK failed\r\n");
        return -1;
    }
    *newpos = ack_len;
    if (g_verbose) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "AB SEEK remote_handle=%lu offset=%ld origin=%lu ack=%lu\r\n",
                    (unsigned long)vf->remote_handle, (long)offset,
                    (unsigned long)origin, (unsigned long)ack_len);
        append_text_log(line);
    }
    return 0;
}

static int sync_vfile_position(VFILE *vf)
{
    DWORD actual;
    if (vf->stream_pos != vf->desired_pos) {
        if (ab_seek(vf, (LONG)vf->desired_pos, 0u, &actual) != 0 ||
            actual != vf->desired_pos) {
            return -1;
        }
        vf->stream_pos = actual;
        vf->desired_pos = actual;
        vf->eof = 0;
    }
    return 0;
}

/* Dedicated index9 (op34) read: fetch exactly nbytes (a4*512, = 4096) at the
 * given absolute file offset, in ONE AB read. This avoids two inefficiencies of
 * routing index9 through impl_read/ab_read_chunk:
 *   1. ab_read_chunk always requests AB_READ_UNIT (0x4000=16384) bytes even when
 *      only 4096 are wanted, so 3/4 of every search read was wasted USB bulk.
 *   2. Because 16384 were consumed from the stream while only 4096 were "used",
 *      stream_pos and desired_pos diverged and forced an extra seek next call.
 * Here we read exactly nbytes and keep stream_pos exact, so a subsequent read of
 * the very next block needs no re-seek. Random jumps still seek (unavoidable). */
static int impl_sector_read(DWORD handle, DWORD offset, DWORD nbytes,
                            unsigned char *dst)
{
    VFILE *vf;
    unsigned char tail[8] = { 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff };
    DWORD data_len = 0;
    DWORD ack_len = 0;
    DWORD actual = 0;
    unsigned __int64 key;

    if (dst == NULL || nbytes == 0u) {
        return 0;
    }
    if (ensure_ready() != 0) {
        return -1;
    }
    vf = find_vfile((int)handle);
    if (vf == NULL) {
        append_error_log("index9 unknown handle", handle);
        return -1;
    }
    /* Cache key = hash of the (immutable, read-only) file path, so a reused
     * remote_handle can never alias another file's cached blocks. */
    key = path_key(vf->path);
    /* Cache hit: serve the block with no USB traffic at all (no seek, no read).
     * Requires the cached block to hold at least the requested bytes. */
    {
        BLOCK_CACHE_ENTRY *e = cache_lookup(key, offset);
        if (e != NULL && e->len >= nbytes) {
            CopyMemory(dst, e->data, nbytes);
            e->lru = ++g_cache_tick;
            return (int)nbytes;
        }
    }
    /* op37 reads from the current position; seek only when not already there. */
    if (vf->stream_pos != offset) {
        if (ab_seek(vf, (LONG)offset, 0u, &actual) != 0 || actual != offset) {
            return -1;
        }
        vf->stream_pos = offset;
    }
    vf->desired_pos = offset;
    vf->eof = 0;
    put_le32(tail, vf->remote_handle);
    if (ab_cmd(nbytes, 0x000D0037u, tail, NULL, 0,
               dst, nbytes, &data_len, &ack_len, 1) != 0) {
        return -1;
    }
    if (ack_len > data_len) {
        ack_len = data_len;
    }
    vf->stream_pos += ack_len;
    vf->desired_pos = vf->stream_pos;
    /* Cache only full reads (short reads at EOF are not re-serveable safely). */
    if (ack_len == nbytes) {
        cache_store(key, offset, dst, ack_len);
    }
    return (int)ack_len;
}

static int impl_init(void)
{
    return ensure_ready() == 0 ? 0 : -1;
}

static int impl_openA(DWORD a0)
{
    unsigned char payload[1024];
    WCHAR wide[512];
    DWORD payload_len;
    DWORD remote_handle;
    if (ensure_ready() != 0) {
        return -1;
    }
    payload_len = utf16_payload_from_ascii((const char *)a0, payload, sizeof(payload), wide, (DWORD)(sizeof(wide) / sizeof(wide[0])));
    if (payload_len == 0u || ab_open_payload(payload, payload_len, &remote_handle) != 0) {
        return -1;
    }
    return allocate_vfile(wide, remote_handle);
}

static int impl_openW(DWORD a0)
{
    unsigned char payload[1024];
    WCHAR wide[512];
    DWORD payload_len;
    DWORD remote_handle;
    if (ensure_ready() != 0) {
        return -1;
    }
    payload_len = utf16_payload_from_wide((const WCHAR *)a0, payload, sizeof(payload));
    if (payload_len == 0u) {
        return -1;
    }
    __try {
        lstrcpynW(wide, (const WCHAR *)a0, (int)(sizeof(wide) / sizeof(wide[0])));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (ab_open_payload(payload, payload_len, &remote_handle) != 0) {
        return -1;
    }
    return allocate_vfile(wide, remote_handle);
}

static int impl_read(DWORD a0, DWORD a1, DWORD a2)
{
    VFILE *vf;
    unsigned char *dst = (unsigned char *)a1;
    DWORD requested = a2;
    DWORD copied = 0;
    unsigned char chunk[AB_READ_UNIT + 4096u];

    if (!dst || requested == 0u) {
        return 0;
    }
    if (ensure_ready() != 0) {
        return -1;
    }
    vf = find_vfile((int)a0);
    if (!vf) {
        return -1;
    }
    if (sync_vfile_position(vf) != 0) {
        return -1;
    }
    while (copied < requested && !vf->eof) {
        DWORD valid;
        DWORD take;
        if (ab_read_chunk(vf, chunk, sizeof(chunk), &valid) != 0) {
            return copied ? (int)copied : -1;
        }
        if (valid == 0u) {
            vf->eof = 1;
            break;
        }
        take = valid;
        if (take > requested - copied) {
            take = requested - copied;
        }
        __try {
            CopyMemory(dst + copied, chunk, take);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return copied ? (int)copied : -1;
        }
        copied += take;
        vf->desired_pos += take;
        vf->stream_pos += valid;
        if (valid < AB_READ_UNIT) {
            vf->eof = 1;
        }
        if (take < valid) {
            vf->desired_pos = vf->desired_pos;
            break;
        }
    }
    return (int)copied;
}

static int impl_seek(DWORD a0, DWORD a1, DWORD a2)
{
    VFILE *vf = find_vfile((int)a0);
    LONG off = (LONG)a1;
    DWORD actual;
    if (!vf) {
        return -1;
    }
    /* The device implements SEEK_SET/CUR/END as origins 0/1/2. */
    if (a2 > 2u) {
        return -1;
    }
    if (ab_seek(vf, off, a2, &actual) != 0) {
        return -1;
    }
    vf->desired_pos = actual;
    vf->stream_pos = actual;
    vf->eof = 0;
    return (int)actual;
}

/* index5 write(handle, buf, len). PASORAMA writes a small record to the on-device
 * history file (e.g. /data/data/com.denesoft.dict/KJ46Hist.bin) EVERY time the user
 * follows a cross-reference link inside an entry (verified: a link click opens
 * KJ46Hist.bin then issues index5 with len=0x2E). The genuine DLL persisted this
 * via AB write cmd 0x27, but we have NO ground-truth capture of that wire frame,
 * and returning -1 (write failed) made PASORAMA retry a few times then tear down
 * every open handle -> this is the root cause of the "clicking a link in an entry
 * crashes" bug. The dictionaries are used read-only, so history persistence is not
 * required; write() only needs to report success so navigation continues. We
 * acknowledge the full length as written WITHOUT touching the device and WITHOUT
 * forging an unverified op27 frame. (To actually persist history later, capture a
 * genuine link-click through the proxy DLL to learn the real op27 layout - see
 * REPLACEMENT_PLAN.md (write path still unanalyzed).) */
static int impl_write(DWORD handle, DWORD buf, DWORD len)
{
    VFILE *vf = find_vfile((int)handle);
    (void)buf;
    if (vf == NULL) {
        append_error_log("write unknown handle", handle);
        return -1;
    }
    if (g_verbose) {
        char line[160];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "WRITE(ack-no-persist) handle=%lu remote_handle=%lu len=%lu\r\n",
                    (unsigned long)handle, (unsigned long)vf->remote_handle,
                    (unsigned long)len);
        append_text_log(line);
    }
    return (int)len;
}

static int impl_close(DWORD a0)
{
    VFILE *vf = find_vfile((int)a0);
    if (!vf) {
        return -1;
    }
    ZeroMemory(vf, sizeof(*vf));
    return 0;
}

static DWORD impl_file_size(DWORD a0)
{
    VFILE *vf = find_vfile((int)a0);
    unsigned char chunk[AB_READ_UNIT + 4096u];
    DWORD total = 0u;
    if (!vf || ensure_ready() != 0 || reopen_vfile(vf) != 0) {
        return 0u;
    }
    for (;;) {
        DWORD valid = 0u;
        if (ab_read_chunk(vf, chunk, sizeof(chunk), &valid) != 0) {
            (void)reopen_vfile(vf);
            return 0u;
        }
        if (valid > MAXDWORD - total) {
            (void)reopen_vfile(vf);
            return 0u;
        }
        total += valid;
        if (valid < AB_READ_UNIT) {
            break;
        }
    }
    vf->desired_pos = 0u;
    if (reopen_vfile(vf) != 0) {
        return 0u;
    }
    return total;
}

static void append_helper_log(int index, DWORD a0, DWORD a1, DWORD a2, DWORD a3,
                              int ret, const char *path_note)
{
    char line[640];
    char path_dump[64];
    if (!g_verbose) {
        return;
    }
    path_dump[0] = '\0';
    if (index == 12) {
        safe_dump_ptr(a0, path_dump, sizeof(path_dump));
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "helper=%02d ret=%d args=[%08lX %08lX %08lX %08lX] path=%s action=%s\r\n",
                index, ret, (unsigned long)a0, (unsigned long)a1,
                (unsigned long)a2, (unsigned long)a3, path_dump,
                path_note ? path_note : "stub");
    append_text_log(line);
}

/*
 * Original USB_PC.dll helper[12] (RVA 0x23D0):
 *   int __cdecl openW_ex(const WCHAR *path, void *unused_out, DWORD flags)
 * PLCommon pushes three arguments and adjusts ESP by 12.  The original helper
 * validates path, ignores unused_out, includes flags in its IOCTL request, and
 * returns a non-negative remote-open result or -1.  The WinUSB transport does
 * not need flags for the observed read-only modes (0 and 0x41).
 */
static int __cdecl usbpc_helper_12(DWORD path, DWORD unused_out, DWORD flags)
{
    int ret;
    (void)unused_out;
    if (g_lock_ready) {
        EnterCriticalSection(&g_lock);
    }
    ret = impl_openW(path);
    append_helper_log(12, path, unused_out, flags, 0, ret,
                      (flags == 0u || flags == 0x41u) ? "openW" : "openW-unknown-flags");
    if (g_lock_ready) {
        LeaveCriticalSection(&g_lock);
    }
    return ret;
}

/*
 * Original helper[13] (RVA 0x2530):
 *   int __cdecl read_ex(handle, buffer, length, out_count)
 * It splits requests at 0x25800 bytes, stores the accumulated byte count when
 * out_count is non-NULL, and returns 1 iff at least one byte was read.
 */
static int __cdecl usbpc_helper_13(DWORD handle, DWORD buffer, DWORD length,
                                   DWORD out_count)
{
    DWORD total = 0u;
    DWORD remaining = length;
    int ret;
    if (g_lock_ready) {
        EnterCriticalSection(&g_lock);
    }
    while (remaining != 0u) {
        DWORD part = remaining > 0x25800u ? 0x25800u : remaining;
        int got = impl_read(handle, buffer + total, part);
        if (got < 0 || (DWORD)got > part) {
            break;
        }
        total += (DWORD)got;
        remaining -= (DWORD)got;
        if ((DWORD)got != part) {
            break;
        }
    }
    if (out_count != 0u) {
        __try {
            *(DWORD *)out_count = total;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            total = 0u;
        }
    }
    ret = total != 0u ? 1 : 0;
    append_helper_log(13, handle, buffer, length, out_count, ret,
                      "read(handle,buffer,length,outCount)");
    if (g_lock_ready) {
        LeaveCriticalSection(&g_lock);
    }
    return ret;
}

/* Original helper[18] (RVA 0x2C80): DWORD __cdecl file_size(handle). */
static DWORD __cdecl usbpc_helper_18(DWORD handle)
{
    DWORD size;
    if (g_lock_ready) {
        EnterCriticalSection(&g_lock);
    }
    size = impl_file_size(handle);
    append_helper_log(18, handle, 0, 0, 0, (int)size, "file-size");
    if (g_lock_ready) {
        LeaveCriticalSection(&g_lock);
    }
    return size;
}

/*
 * Original helper[19] (RVA 0x2E50):
 *   LONG __cdecl seek_ex(int handle, LONG offset, DWORD origin)
 * PLCommon's wrapper at 0x100710C0 pushes exactly three arguments and adds
 * 0x0c to ESP.  The original sends command 0x00138037 and returns the
 * resulting absolute position, or -1 on transport/device failure.
 */
static LONG __cdecl usbpc_helper_19(DWORD handle, LONG offset, DWORD origin)
{
    VFILE *vf;
    DWORD before = 0u;
    DWORD remote = 0u;
    int ret;
    char line[320];

    if (g_lock_ready) {
        EnterCriticalSection(&g_lock);
    }
    vf = find_vfile((int)handle);
    if (vf != NULL) {
        before = vf->desired_pos;
        remote = vf->remote_handle;
    }
    ret = impl_seek(handle, (DWORD)offset, origin);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "helper=19 ret=%d handle=%lu remote_handle=%lu offset=%ld "
                "origin=%lu before=%lu after=%lu action=seek(handle,offset,origin)\r\n",
                ret, (unsigned long)handle, (unsigned long)remote, (long)offset,
                (unsigned long)origin, (unsigned long)before,
                (unsigned long)((ret >= 0) ? (DWORD)ret : before));
    append_text_log(line);
    if (g_lock_ready) {
        LeaveCriticalSection(&g_lock);
    }
    return (LONG)ret;
}

#define DEFINE_HELPER_STUB(index) \
    static int __cdecl usbpc_helper_##index(DWORD a0, DWORD a1, DWORD a2, DWORD a3) \
    { \
        append_helper_log(index, a0, a1, a2, a3, -1, "unimplemented"); \
        return -1; \
    }

DEFINE_HELPER_STUB(0)  DEFINE_HELPER_STUB(1)  DEFINE_HELPER_STUB(2)
DEFINE_HELPER_STUB(3)  DEFINE_HELPER_STUB(4)  DEFINE_HELPER_STUB(5)
DEFINE_HELPER_STUB(6)  DEFINE_HELPER_STUB(7)  DEFINE_HELPER_STUB(8)
DEFINE_HELPER_STUB(9)  DEFINE_HELPER_STUB(10) DEFINE_HELPER_STUB(11)
DEFINE_HELPER_STUB(14) DEFINE_HELPER_STUB(15)
DEFINE_HELPER_STUB(16) DEFINE_HELPER_STUB(17)
DEFINE_HELPER_STUB(20) DEFINE_HELPER_STUB(21)
DEFINE_HELPER_STUB(22) DEFINE_HELPER_STUB(23) DEFINE_HELPER_STUB(24)
DEFINE_HELPER_STUB(25) DEFINE_HELPER_STUB(26) DEFINE_HELPER_STUB(27)
DEFINE_HELPER_STUB(28) DEFINE_HELPER_STUB(29)

/* 30 entries, matching the original table extent at RVA 0xB110. */
static USBPC_HELPER_FN UsbPcHelperTable[30] = {
    usbpc_helper_0, usbpc_helper_1, usbpc_helper_2, usbpc_helper_3,
    usbpc_helper_4, usbpc_helper_5, usbpc_helper_6, usbpc_helper_7,
    usbpc_helper_8, usbpc_helper_9, usbpc_helper_10, usbpc_helper_11,
    (USBPC_HELPER_FN)usbpc_helper_12,
    (USBPC_HELPER_FN)usbpc_helper_13, usbpc_helper_14, usbpc_helper_15,
    usbpc_helper_16, usbpc_helper_17, (USBPC_HELPER_FN)usbpc_helper_18,
    (USBPC_HELPER_FN)usbpc_helper_19,
    usbpc_helper_20, usbpc_helper_21, usbpc_helper_22, usbpc_helper_23,
    usbpc_helper_24, usbpc_helper_25, usbpc_helper_26, usbpc_helper_27,
    usbpc_helper_28, usbpc_helper_29
};

static void try_write_version_buffer(DWORD a0, DWORD a1)
{
    __try {
        char *buf = (char *)a0;
        DWORD cap = a1;
        const char *ver = "1.0.0.0";
        DWORD n = (DWORD)lstrlenA(ver);
        DWORD i;
        if (buf == NULL || cap == 0u) {
            return;
        }
        if (n > cap - 1u) {
            n = cap - 1u;
        }
        for (i = 0; i < n; i++) {
            buf[i] = ver[i];
        }
        buf[n] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static int __cdecl stub_common(int index, DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5)
{
    int ret;
    (void)a5;
    if (g_lock_ready) {
        EnterCriticalSection(&g_lock);
    }
    switch (index) {
    case 0:
        ret = impl_init();
        break;
    case 2:
        ret = impl_openA(a0);
        break;
    case 3:
        ret = impl_openW(a0);
        break;
    case 4:
        ret = impl_read(a0, a1, a2);
        break;
    case 5:
        ret = impl_write(a0, a1, a2);
        break;
    case 6:
        ret = impl_seek(a0, a1, a2);
        break;
    case 8:
        ret = impl_close(a0);
        break;
    case 9:
        {
            /* index9 (op34, "ReadPhysSect") is a RANDOM-ACCESS BLOCK READ into an
             * already-open index file, used during search. Ground truth: proxy
             * capture (usb_pc_proxy.c -> orig_backup, search_za_proxy_capture.pcapng)
             * correlated 498/498 index9 calls to genuine op34 wire frames. Findings:
             *   - a0 is ALWAYS 0 in the genuine path (the a0=2 we chased was a symptom
             *     of our old garbage reads, not a real access pattern).
             *   - a1 = (tag<<16) | handle. handle = a1 & 0xffff is a value WE returned
             *     from index2/openA (verified: openA->0x36 then index9 a1=..0036).
             *   - block index = (a2<<8)|a5, block*4096 is the file offset. a2 is the
             *     HIGH part (up to ~11 bits: observed max a2=0x7FF => files up to ~2GB),
             *     a5 the low byte. a0 is a REDUNDANT copy of a2's high byte: verified on
             *     host over an alphabet+kanji search that a0==(a2>>8) for every call
             *     (1852/1852), so a0 carries no extra information and is ignored. Do NOT
             *     mask a2 to a byte - doing so caps the block at 0xFF and corrupts every
             *     offset with a2>0xFF (the original kanji-search crash: a kanji query
             *     produced a2=0x483,a0=4 and the masked formula read the wrong 4096-byte
             *     block -> PLCommon parsed a bad index entry -> STATUS_STACK_BUFFER_OVERRUN).
             *     The reverse order (a5<<8)|a2 overruns every file, ruling it out.
             *   - a4 = sector count (always 8 => 4096 bytes). a3 = output buffer.
             * The genuine wire op34 tail is [00, a2, a5, <driver session handle>, <ptr>];
             * that session handle is edict.sys-internal and not reproducible from our
             * side, so we deliberately do NOT forge an op34 frame. Instead we serve the
             * read through our proven op37 read path (impl_read), which returns the same
             * real index bytes. See PROXY_DEPLOY.md / INDEX9_ANALYSIS.md for the trail.
             *
             * This replaces the old fabricated sector_addr formula (with a spurious
             * opcode +1 and a5 misplaced into tail[4..7]) that returned wrong data,
             * drove PLCommon into its recovery path (the phantom a0=2 calls) and
             * finally the STATUS_STACK_BUFFER_OVERRUN crash at PLCommon+0xebc8. */
            DWORD handle = a1 & 0xffffu;
            DWORD block = (a2 << 8) | (a5 & 0xffu); /* full a2 (high), a5 (low); a0==a2>>8 redundant */
            ret = impl_sector_read(handle, block << 12, a4 * 512u, (unsigned char *)a3);
        }
        break;
    case 20:
        try_write_version_buffer(a0, a1);
        ret = 1;
        break;
    case 21:
        ret = ensure_ready() == 0 ? 0 : -1;
        break;
    case 23:
        /* index23 is also used as CPLUsbFileApp's periodic "is alive" poll
         * (PLCommon.dll vtable+0x80 @ 0x1006fe60): the caller at
         * PLCommon.dll 0x1002c4b0 requires the raw return value to be
         * exactly 1 to consider the connection healthy (cmp edi,1 / je).
         * Returning 0 (as index21's connect-time success value) makes this
         * periodic check fail every ~800ms once dictionaries are loaded and
         * traffic quiets down, which is the root cause of the "USB cable
         * disconnected" error a few dozen seconds after a successful
         * dictionary load. See INDEX9_ANALYSIS.md 2026-07-06 for the
         * disassembly trail (CPLProject::SetDisplayErrorBox /
         * NeedDisplayErrorBox chain).
         *
         * Also: this is where we actually verify (and recover from) a dead
         * USB pipe. See check_link_alive() - ensure_ready() alone cannot
         * detect a pipe that silently stopped transferring data (observed:
         * WinUsb_WritePipe gle=22) because it only checks whether the WinUSB
         * handle object is still open. Without this, the periodic health
         * check kept reporting "alive" while every real request failed,
         * so PLCommon never noticed the disconnect and eventually crashed
         * retrying requests against a dead pipe. */
        ret = check_link_alive() == 0 ? 1 : -1;
        break;
    case 22:
        close_winusb_device();
        ret = 0;
        break;
    case 24:
        ret = (int)(INT_PTR)UsbPcHelperTable;
        break;
    case 14:
        ret = 1; /* PLCommon: cmp eax,1 -- ONLY 1 is success, 0 and -1 both fail (Codex task05 finding) */
        break;
    default:
        ret = -1;
        break;
    }
    append_call_log(index, a0, a1, a2, a3, ret);
    if (g_lock_ready) {
        LeaveCriticalSection(&g_lock);
    }
    return ret;
}

#define DEFINE_STUB(index) \
    static int __cdecl usbpc_stub_##index(DWORD a0, DWORD a1, DWORD a2, DWORD a3) \
    { \
        volatile DWORD *stack = &a0; \
        return stub_common(index, a0, a1, a2, a3, stack[4], stack[5]); \
    }

DEFINE_STUB(0)
DEFINE_STUB(1)
DEFINE_STUB(2)
DEFINE_STUB(3)
DEFINE_STUB(4)
DEFINE_STUB(5)
DEFINE_STUB(6)
DEFINE_STUB(7)
DEFINE_STUB(8)
/* index9 (ReadPhySect / op34) has a multi-arg ABI that the 4-dword logger cannot
 * capture. Read up to 11 stack dwords via &a0 (cdecl: caller pushes args
 * contiguously) to observe the true argument layout on real hardware. */
static int __cdecl usbpc_stub_9(DWORD a0, DWORD a1, DWORD a2, DWORD a3)
{
    volatile DWORD *stack = &a0;
    if (g_verbose) {
        char line[256];
        int i;
        int n = 0;
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "index=09 STACKARGS ");
        for (i = 0; i < 11 && n < (int)sizeof(line) - 12; i++) {
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "%08lX ",
                             (unsigned long)stack[i]);
        }
        _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "\r\n");
        append_text_log(line);
    }
    return stub_common(9, a0, a1, a2, a3, stack[4], stack[5]);
}
DEFINE_STUB(10)
DEFINE_STUB(11)
DEFINE_STUB(12)
DEFINE_STUB(13)
DEFINE_STUB(14)
DEFINE_STUB(15)
DEFINE_STUB(16)
DEFINE_STUB(17)
DEFINE_STUB(18)
DEFINE_STUB(19)
DEFINE_STUB(20)
DEFINE_STUB(21)
DEFINE_STUB(22)
DEFINE_STUB(23)
DEFINE_STUB(24)
DEFINE_STUB(25)

__declspec(dllexport) USBPC_STUB_FN DllExportTable[26] = {
    usbpc_stub_0,
    usbpc_stub_1,
    usbpc_stub_2,
    usbpc_stub_3,
    usbpc_stub_4,
    usbpc_stub_5,
    usbpc_stub_6,
    usbpc_stub_7,
    usbpc_stub_8,
    usbpc_stub_9,
    usbpc_stub_10,
    usbpc_stub_11,
    usbpc_stub_12,
    usbpc_stub_13,
    usbpc_stub_14,
    usbpc_stub_15,
    usbpc_stub_16,
    usbpc_stub_17,
    usbpc_stub_18,
    usbpc_stub_19,
    usbpc_stub_20,
    usbpc_stub_21,
    usbpc_stub_22,
    usbpc_stub_23,
    usbpc_stub_24,
    usbpc_stub_25
};

__declspec(dllexport) void __cdecl InitialDll(void)
{
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char verbose_env[8] = "";
        char cache_env[8] = "";
        InitializeCriticalSection(&g_lock);
        g_lock_ready = 1;
        g_link.dev = INVALID_HANDLE_VALUE;
        GetEnvironmentVariableA("USBPC_VERBOSE", verbose_env, (DWORD)sizeof(verbose_env));
        g_verbose = (verbose_env[0] == '1');
        /* Cache is on by default (path-keyed = correct); USBPC_NOCACHE=1 disables. */
        GetEnvironmentVariableA("USBPC_NOCACHE", cache_env, (DWORD)sizeof(cache_env));
        g_cache_enabled = (cache_env[0] != '1');
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_lock_ready) {
            close_winusb_device();
            DeleteCriticalSection(&g_lock);
            g_lock_ready = 0;
        }
    }
    return TRUE;
}
