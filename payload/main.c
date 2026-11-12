/*
 * PS5 DevLoop - resident input and screen-capture payload.
 * Copyright (C) 2026 Gary Riches
 * The command server's frame follows ps5vkctl (PS5_Vulkan), Copyright (C) 2026 Mihawk-99.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ps5vkctl (PS5_Vulkan) lets a run launch and close a title without a hand on
 * the console. What it cannot do is press a button or say what the screen
 * shows, so every run that needs either still needs a person. This payload is
 * the other half: it stays resident and answers one command per connection on
 * port 9112. This copy is cut down to what testing Playden games uses (the
 * pad, the screen, files, processes and logs); DevLoop itself has the rest:
 *
 *   ping                      is the agent there, and what state the pad is in
 *   pad open [USERID] [keep]  create a virtual DualSense, give it to a user, and
 *                             (unless "keep") disconnect any real controller so
 *                             that the virtual pad is the one titles read
 *   pad close                 remove it
 *   pad status                handle, device id, user, last insert result
 *   pad N ...                 any pad command for virtual pad N (1-4; default 1):
 *                             "pad 2 open" adds a second pad, for the next signed-in
 *                             user without one (pads 2-4 never disconnect controllers)
 *   pad list / pad close all  every pad's state / close them all
 *   pad set B LX LY RX RY L2 R2   hold this state (B is a hex button mask)
 *   pad tap B MS              press B for MS milliseconds, then release
 *   pad handle N              bring-up: use N as the insert handle
 *   pad bind DEVICE [USERID]  bring-up: bind a device id to a user by hand
 *   pad evict DEVICE          disconnect another controller, so that the
 *                             virtual pad is its user's first, the one titles read
 *   list-captures             the newest files under the console's captures
 *   latest-capture            send the newest capture: a header line, then bytes
 *   screenshot [HOLD_MS]      hold Create (the console's "take screenshot"
 *                             shortcut), wait for the new file, send it, and
 *                             delete it from the console's gallery
 *   delete-capture PATH       delete a capture as the gallery does
 *   kernel-log [MS]           dump the kernel log for a while (bring-up)
 *   ps                        list processes (pid, app id, title id, command)
 *   ls PATH                   list a directory
 *   get PATH                  send a file's bytes
 *   put PATH SIZE             write the SIZE bytes that follow to a file
 *   rm PATH / mv FROM TO      delete, move or rename a file
 *   close-app [APP_ID]        close a running app (default: the foreground one)
 *   locate-title              the foreground app's ids (app id, pid, title id)
 *   hwinfo                    model, serial, cpu + all SoC temps, cpu clock (and per
 *                             core), fan %, SoC power
 *   df                        free space per mounted volume
 *   GET / , GET /api/hwinfo   a browser gets the live dashboard, hosted here
 *   GET /api/run?c=CMD        the dashboard runs any command above; the reply
 *                             is served as text, or as the picture/file it sends
 *   kill PID                  end a process
 *   klog-stream [MS]          stream the kernel log until you disconnect
 *   (a UDP probe to the command port is answered with an identifier: discovery)
 *   (at start, a flyout shows the dashboard's address, http://<console>:<port>/)
 *   quit                      remove the pad and leave the console
 *
 * The pad is the system's own virtual device (libScePad's
 * scePadVirtualDevice*, the calls Remote Play goes through), so the home
 * screen and every title see it as a controller. The call order, the 120-byte
 * sample and the way the device id is recovered from the kernel log are what
 * SplashDown, Control4Free, OmniPad-PS5 and Ghostpad established on hardware:
 *
 *   - libSceMbus has no SDK stub and libScePad imports from it: dlopen it first;
 *   - scePadInit, then scePadSetProcessPrivilege(1), in that order;
 *   - AddDevice answers 0x803b0006 ("assignment pending") on a PS5 even when
 *     the device exists. Its id is only in the kernel log, on the login
 *     manager's DEVICE_ADDED line, subType 22;
 *   - sceMbusBindDeviceWithUserId gives the device to a user, which is what
 *     choosing a profile on the "Who's using this controller?" screen does.
 *
 * Which handle InsertData takes differs by firmware (the device id, its low
 * word, or a small index), so "pad open" tries each candidate with a neutral
 * sample and reports what every step returned. On firmware 12.20 and 13.60 it
 * is the device id (README.md, "What the console showed").
 *
 * The screen comes from the console's own screenshot, which is what a person
 * holding Create gets: the whole composited picture, system UI included, in
 * the format the console is set to (JXR when HDR is on). It costs the 1.2 s the
 * console wants Create held. The picture is then deleted through
 * libSceContentDelete, which removes it as the gallery's own Delete does.
 *
 * A title that can write its own frame is asked instead ("frame"): it is quicker
 * by an order of magnitude and leaves nothing behind, but shows only what the
 * title drew. The title's side is a file it looks for and removes
 * (AGENTS.md, "Giving a title a frame").
 *
 * Beyond input and sight, the payload also controls the console: list and send
 * and receive files (ls/get/put/rm/mv), list processes (ps), launch and close
 * titles (launch/close), open a URL, and power the console (rest/reboot/off).
 * These use the console's own services, linked or dlopened, run with the
 * payload's elevated credentials.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <machine/reg.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>

#include "dashboard_html.h" // the live dashboard, embedded so the console hosts it

#define PS5LOOP_PORT 9112
#define PS5LOOP_LINE 1024
#define PS5LOOP_LOG "/data/ps5loop.log"
#define PS5LOOP_KLOG_PORT 3232

/* The virtual device type every known caller uses, on both consoles. */
#define PS5LOOP_DEVICE_TYPE 3
/* The user id that creates a device nobody owns yet. */
#define PS5LOOP_UNASSIGNED_USER 1
/* AddDevice's answer when the device exists and waits for a user. */
#define PS5LOOP_ASSIGNMENT_PENDING 0x803b0006u
/* The credentials the pad service and libSceMbus accept from a payload. */
#define PS5LOOP_AUTHID 0x4800000000010003ull
/* How often the held state is inserted: a real pad reports every 4 ms. */
#define PS5LOOP_INSERT_INTERVAL_US 8000
/* The Create button, and how long the console wants it held for a screenshot. */
#define PS5LOOP_BUTTON_CREATE 0x00000001u
#define PS5LOOP_BUTTON_L2 0x00000100u
#define PS5LOOP_BUTTON_R2 0x00000200u
#define PS5LOOP_CREATE_HOLD_MS 1200
#define PS5LOOP_SHOT_WAIT_MS 10000

/* Where the console keeps what Create captures. */
static const char *const capture_roots[] = {"/user/av_contents/photo", "/user/av_contents"};

/* One pad sample, the 120 bytes scePadRead fills and InsertData takes. */
typedef struct pad_touch
{
    uint16_t x;
    uint16_t y;
    uint8_t id;
    uint8_t reserved[3];
} pad_touch_t;

typedef struct pad_data
{
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
    uint8_t reserved0[2];
    float orientation[4];
    float acceleration[3];
    float angular_velocity[3];
    uint8_t touch_count;
    uint8_t reserved1[7];
    pad_touch_t touch[2];
    uint8_t connected;
    uint8_t reserved2[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t reserved3[15];
} pad_data_t;
_Static_assert(sizeof(pad_data_t) == 120, "the pad sample is 120 bytes");

/* AddDevice's parameter block: its own size, the user, six unknown words. */
typedef struct add_device_param
{
    int32_t size;
    int32_t user_id;
    int32_t unknown[6];
} add_device_param_t;

/* libScePad and libSceUserService, through the payload SDK's stub libraries. */
extern int32_t scePadInit(void);
extern int32_t scePadSetProcessPrivilege(int32_t privilege);
extern int32_t scePadVirtualDeviceAddDevice(void *param, int32_t device_type);
extern int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *data);
extern int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);
extern int sceUserServiceInitialize(void *param);
extern int sceUserServiceGetForegroundUser(int *user_id);

/* The physical DualSense, to attempt lightbar control (SceShellUI owns it). */
typedef struct pad_lightbar { uint8_t r, g, b, reserve; } pad_lightbar_t;
extern int scePadInit(void);
extern int scePadOpen(int user_id, int type, int index, void *param);
extern int scePadClose(int handle);
extern int scePadSetLightBar(int handle, pad_lightbar_t *param);

/* libSceMbus, resolved at run time. */
static int32_t (*mbus_bind)(uint64_t device_id, int32_t user_id);
static int32_t (*mbus_disconnect)(uint64_t device_id);

/* libSceScreenShot and libSceContentDelete, resolved at run time. Their
 * arguments are read from the libraries' own code (firmware 13.60):
 *   - sceScreenShotCapture asks the screenshot service for a picture of the
 *     screen and waits, 16 ms at a time, until the service names the file it
 *     saved, in a buffer of 0x400 bytes; a callback that returns non-zero ends
 *     the wait. Types 0 and 1 are accepted.
 *   - sceContentDeleteInitialize refuses any parameter block whose second
 *     word is not 0x4000; sceContentDeleteByPath then removes a capture as the
 *     gallery's own Delete does: the file, its thumbnail and its database row. */
typedef struct content_delete_param
{
    uint64_t unknown0;
    uint64_t buffer_size;
    uint64_t unknown[6];
} content_delete_param_t;
#define PS5LOOP_CAPTURE_PATH 0x400
#define PS5LOOP_CONTENT_DELETE_BUFFER 0x4000
static int32_t (*screenshot_capture)(int32_t type, char *path, int (*cancel)(void *), void *user);
static int32_t (*content_delete_by_path)(const char *path);
static bool g_capture_libraries;

/* The virtual pads: up to four, each its own device. Pad 1 (slot 0) is the one
   every command addresses unless another is named ("pad 2 tap ..."). */
#define PS5LOOP_PADS 4
typedef struct pad_slot
{
    bool open;
    int32_t handle;
    uint64_t device_id;
    int32_t user_id;
    int32_t last_insert;
    uint32_t inserts;
    pad_data_t state;
} pad_slot_t;
static struct
{
    pthread_mutex_t lock;
    bool libraries; /* libSceMbus loaded and the pad service initialised */
    pad_slot_t slot[PS5LOOP_PADS];
} g_pad = {.lock = PTHREAD_MUTEX_INITIALIZER,
           .slot = {[0 ... PS5LOOP_PADS - 1] = {.handle = -1, .user_id = -1}}};

// How many pads are open.
static int pads_open(void)
{
    int count = 0;
    for (int n = 0; n < PS5LOOP_PADS; n++)
        count += g_pad.slot[n].open;
    return count;
}

// Log one line to stdout (klog carries a payload's output) and to the log file
// the FTP server can hand back.
static void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    vprintf(format, arguments);
    printf("\n");
    fflush(stdout);
    FILE *const file = fopen(PS5LOOP_LOG, "a");
    if (file != NULL)
    {
        vfprintf(file, format, copy);
        fputc('\n', file);
        fclose(file);
    }
    va_end(copy);
    va_end(arguments);
}

/* When a browser invokes a command over HTTP (GET /api/run), the command's
 * socket output -- its reply line and any file or picture bytes -- is collected
 * here instead of being written to the socket, so the HTTP handler can wrap it
 * in a response. One client is served at a time (the accept loop is serial), so
 * a single buffer serves; the background threads never produce command output. */
static struct
{
    char *buf;
    size_t cap, len;
    bool on, oom;
} g_capture;

// Every command-output write goes through here: into the capture buffer while a
// browser's /api/run runs, to the socket otherwise. Returns len while capturing
// so a caller's "short write" check sees a complete write.
static ssize_t out(int client, const void *data, size_t len, int flags)
{
    if (!g_capture.on)
        return send(client, data, len, flags);
    if (!g_capture.oom && len > 0)
    {
        if (g_capture.len + len > g_capture.cap)
        {
            size_t want = g_capture.cap != 0 ? g_capture.cap : 8192;
            while (want < g_capture.len + len)
                want *= 2;
            char *const bigger = realloc(g_capture.buf, want);
            if (bigger == NULL)
                g_capture.oom = true;
            else
            {
                g_capture.buf = bigger;
                g_capture.cap = want;
            }
        }
        if (!g_capture.oom)
        {
            memcpy(g_capture.buf + g_capture.len, data, len);
            g_capture.len += len;
        }
    }
    return (ssize_t)len;
}

// Answer one line, newline terminated.
static void reply(int client, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void reply(int client, const char *format, ...)
{
    char line[PS5LOOP_LINE];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    log_line("ps5loop: -> %s", line);
    if (out(client, line, strlen(line), 0) < 0 || out(client, "\n", 1, 0) < 0)
        log_line("ps5loop: reply failed: %s", strerror(errno));
}

// Add to a report that a reply carries back, so one "pad open" tells the whole
// story of what each call returned.
static void note(char *report, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
static void note(char *report, size_t size, const char *format, ...)
{
    const size_t used = strlen(report);
    if (used + 2 >= size)
        return;
    if (used > 0)
        report[used] = ' ';
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(report + used + (used > 0), size - used - (used > 0), format, arguments);
    va_end(arguments);
}

static void elevate(char *report, size_t size);

static uint64_t monotonic_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

// A neutral sample: sticks centred, identity orientation, connected. The
// service discards a sample whose orientation is not a rotation.
static void pad_neutral(pad_data_t *data)
{
    memset(data, 0, sizeof(*data));
    data->lx = data->ly = data->rx = data->ry = 128;
    data->orientation[3] = 1.0f;
    data->acceleration[1] = 1.0f;
    data->connected = 1;
    data->connected_count = 1;
}

// One InsertData call, stamped as a real pad stamps its reports.
static int32_t pad_insert(int32_t handle, const pad_data_t *data)
{
    pad_data_t stamped = *data;
    stamped.timestamp = monotonic_us();
    return scePadVirtualDeviceInsertData(handle, &stamped);
}

// Keep the held state arriving, as a controller's reports do: a device that
// goes quiet reads as released, and then as gone.
static void *insert_thread(void *argument)
{
    (void)argument;
    for (;;)
    {
        pthread_mutex_lock(&g_pad.lock);
        for (int n = 0; n < PS5LOOP_PADS; n++)
        {
            pad_slot_t *const pad = &g_pad.slot[n];
            if (pad->open)
            {
                pad->last_insert = pad_insert(pad->handle, &pad->state);
                pad->inserts++;
            }
        }
        pthread_mutex_unlock(&g_pad.lock);
        usleep(PS5LOOP_INSERT_INTERVAL_US);
    }
    return NULL;
}

// Discovery beacon: answer a UDP probe on the command port so a client can find
// the console on the LAN without its address hardcoded. It replies with an
// identifier, never anything that needs the sender to be trusted.
static void *discover_thread(void *argument)
{
    (void)argument;
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return NULL;
    int reuse = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PS5LOOP_PORT);
    if (bind(sock, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        close(sock);
        return NULL;
    }
    for (;;)
    {
        char probe[64];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        const ssize_t n = recvfrom(sock, probe, sizeof(probe), 0, (struct sockaddr *)&from, &fromlen);
        if (n < 0)
            continue;
        char answer[96];
        const int len = snprintf(answer, sizeof(answer), "ps5loop fw=0x%08x port=%d pid=%d",
                                 kernel_get_fw_version(), PS5LOOP_PORT, (int)getpid());
        if (len > 0)
            (void)sendto(sock, answer, (size_t)len, 0, (struct sockaddr *)&from, fromlen);
    }
    return NULL;
}

// Hold a state on pad n until the next one.
static void pad_hold(int n, uint32_t buttons, int lx, int ly, int rx, int ry, int l2, int r2)
{
    pthread_mutex_lock(&g_pad.lock);
    pad_data_t *const state = &g_pad.slot[n].state;
    pad_neutral(state);
    state->buttons = buttons;
    state->lx = (uint8_t)lx;
    state->ly = (uint8_t)ly;
    state->rx = (uint8_t)rx;
    state->ry = (uint8_t)ry;
    // The console reads the triggers from their analog bytes: the bit alone
    // does not register.
    state->l2 = (uint8_t)((buttons & PS5LOOP_BUTTON_L2) != 0 && l2 == 0 ? 255 : l2);
    state->r2 = (uint8_t)((buttons & PS5LOOP_BUTTON_R2) != 0 && r2 == 0 ? 255 : r2);
    pthread_mutex_unlock(&g_pad.lock);
}

// The kernel log, for the one line that names a new device: the klog server
// when the console runs one (/dev/klog has a single reader, and the server is
// it), the device itself otherwise. -1 when neither opens.
static int klog_open(void)
{
    const int connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection >= 0)
    {
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(PS5LOOP_KLOG_PORT);
        if (connect(connection, (struct sockaddr *)&address, sizeof(address)) == 0)
            return connection;
        close(connection);
    }
    return open("/dev/klog", O_RDONLY | O_NONBLOCK);
}

// Read what the log has, waiting at most that long for more. 0 when it stayed
// quiet, -1 when the log closed.
static ssize_t klog_read(int log, char *buffer, size_t size, int wait_ms)
{
    struct pollfd wait = {.fd = log, .events = POLLIN};
    if (poll(&wait, 1, wait_ms) <= 0)
        return 0;
    const ssize_t bytes = read(log, buffer, size);
    if (bytes < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    return bytes <= 0 ? -1 : bytes;
}

// The device id on a "device added" line, 0 for any other line. A line that
// names a subtype must name the virtual pad's (22): anything looser could
// return a real controller connected at the same moment.
static uint64_t klog_device_added(const char *line)
{
    if (strstr(line, "DEVICE_ADDED") == NULL)
        return 0;
    const char *subtype = strstr(line, "subType");
    if (subtype == NULL)
        subtype = strstr(line, "deviceType");
    if (subtype != NULL)
    {
        subtype += strcspn(subtype, ":=");
        if (strtoul(subtype + (*subtype != '\0'), NULL, 0) != 22)
            return 0;
    }
    const char *id = strstr(line, "DeviceId");
    if (id == NULL)
        id = strstr(line, "deviceId");
    if (id == NULL)
        return 0;
    id += strcspn(id, ":=");
    return strtoull(id + (*id != '\0'), NULL, 16);
}

// Follow the log for the new device's id, for at most that long.
static uint64_t klog_wait_device(int log, int wait_ms, char *report, size_t size)
{
    char line[1024];
    size_t used = 0;
    uint64_t device_id = 0;
    const uint64_t deadline = monotonic_us() + (uint64_t)wait_ms * 1000u;
    while (device_id == 0 && monotonic_us() < deadline)
    {
        char chunk[512];
        const ssize_t bytes = klog_read(log, chunk, sizeof(chunk), 100);
        if (bytes < 0)
            break;
        for (ssize_t index = 0; index < bytes && device_id == 0; index++)
        {
            if (chunk[index] != '\n' && used + 1 < sizeof(line))
            {
                line[used++] = chunk[index];
                continue;
            }
            line[used] = '\0';
            used = 0;
            if (strstr(line, "DEVICE_ADDED") != NULL)
                log_line("ps5loop: klog: %s", line);
            device_id = klog_device_added(line);
        }
    }
    if (device_id == 0)
        note(report, size, "klog=no-device-line");
    return device_id;
}

// Disconnect every real controller the login manager lists, so that the virtual
// pad becomes its user's first and titles read it. The login manager keeps a
// controller table in the kernel log: a "[CIM]  Pad ..." header, then one
// "[CIM] [n] 0x<device> 0x<user> ..." row per slot. A fresh reader of the log is
// replayed the backlog, so opening the log here and taking the newest table
// gives the current controllers. Reading it in the payload needs no klog server
// on the host, so it works on a console that runs none (firmware 12.20).
// Returns how many disconnects the service accepted.
static int klog_evict_cim(uint64_t keep_device, char *report, size_t size)
{
    const int log = klog_open();
    if (log < 0)
    {
        note(report, size, "evict=noklog");
        return 0;
    }
    const size_t cap = 1u << 17;
    char *const buffer = malloc(cap);
    size_t used = 0;
    const uint64_t deadline = monotonic_us() + 800000u;
    while (buffer != NULL && used + 1 < cap && monotonic_us() < deadline)
    {
        const ssize_t bytes = klog_read(log, buffer + used, cap - 1 - used, 100);
        if (bytes < 0)
            break;
        used += (size_t)bytes;
    }
    close(log);
    if (buffer == NULL)
    {
        note(report, size, "evict=nomem");
        return 0;
    }
    buffer[used] = '\0';
    // The last table printed is the current one.
    char *table = buffer;
    for (char *at = buffer; (at = strstr(at, "[CIM]  Pad")) != NULL; at++)
        table = at;
    int evicted = 0, listed = 0;
    for (char *line = table; line != NULL && *line != '\0';)
    {
        char *const end = strchr(line, '\n');
        if (end != NULL)
            *end = '\0';
        // A slot row reads "[CIM] [<n>] 0x<device> ..."; the header ("[CIM]  Pad")
        // and other "[CIM]" lines do not match this shape and are skipped.
        const char *const row = strstr(line, "[CIM] [");
        if (row != NULL && row[7] >= '0' && row[7] <= '9' && listed < 8)
        {
            const char *const hex = strstr(row, "0x");
            const uint64_t device = hex != NULL ? strtoull(hex + 2, NULL, 16) : 0;
            // never one of ours: the pad being opened, or another open virtual pad
            bool ours = device == keep_device;
            for (int n = 0; n < PS5LOOP_PADS && !ours; n++)
                ours = g_pad.slot[n].open && g_pad.slot[n].device_id == device;
            if (device != 0 && !ours && device != 0xffffffffffffffffull)
            {
                listed++;
                const int32_t result = mbus_disconnect != NULL ? mbus_disconnect(device) : -1;
                log_line("ps5loop: evict 0x%llx -> 0x%08x", (unsigned long long)device,
                         (uint32_t)result);
                if (result == 0)
                    evicted++;
            }
        }
        line = end != NULL ? end + 1 : NULL;
    }
    free(buffer);
    note(report, size, "evicted=%d/%d", evicted, listed);
    return evicted;
}

// Load libSceMbus, raise this process to the credentials the pad service
// accepts, and initialise the service. Once per process.
static int pad_libraries(char *report, size_t size)
{
    if (g_pad.libraries)
        return 0;
    elevate(report, size);

    // libScePad's imports from libSceMbus are unresolved in a payload: a pad
    // call before this load can end the process.
    void *mbus = dlopen("/system/common/lib/libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (mbus == NULL)
        mbus = dlopen("libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (mbus == NULL)
    {
        note(report, size, "libSceMbus=not-loaded");
        return -1;
    }
    mbus_bind = (int32_t (*)(uint64_t, int32_t))dlsym(mbus, "sceMbusBindDeviceWithUserId");
    mbus_disconnect = (int32_t (*)(uint64_t))dlsym(mbus, "sceMbusDisconnectDevice");
    note(report, size, "mbus_bind=%s", mbus_bind != NULL ? "found" : "missing");

    const int32_t init = scePadInit();
    const int32_t privilege = scePadSetProcessPrivilege(1);
    note(report, size, "padInit=0x%08x privilege=0x%08x", (uint32_t)init, (uint32_t)privilege);
    if (init != 0)
        return -1;
    g_pad.libraries = true;
    return 0;
}

// Find the handle InsertData accepts for a new device: whatever AddDevice
// returned or wrote back, then the device id and its low word.
static int32_t pad_find_handle(int32_t added, const add_device_param_t *param, uint64_t device_id,
                               char *report, size_t size)
{
    int32_t candidates[10];
    int count = 0;
    if (added > 0)
        candidates[count++] = added;
    for (int index = 0; index < 6; index++)
        if (param->unknown[index] > 0)
            candidates[count++] = param->unknown[index];
    if (device_id != 0)
    {
        candidates[count++] = (int32_t)(device_id & 0xffffffffu);
        if ((device_id >> 32) != 0)
            candidates[count++] = (int32_t)(device_id >> 32);
    }
    pad_data_t neutral;
    pad_neutral(&neutral);
    for (int index = 0; index < count; index++)
    {
        const int32_t result = pad_insert(candidates[index], &neutral);
        note(report, size, "insert(0x%x)=0x%08x", (uint32_t)candidates[index], (uint32_t)result);
        if (result == 0)
            return candidates[index];
    }
    return -1;
}

// A signed-in user for another pad: the first one in the login list that no open
// pad already belongs to. -1 when every signed-in user has a pad.
typedef struct login_user_list
{
    int32_t user_id[4];
} login_user_list_t;
extern int sceUserServiceGetLoginUserIdList(login_user_list_t *list);

static int32_t pad_free_user(void)
{
    login_user_list_t list;
    memset(&list, 0xff, sizeof(list));
    if (sceUserServiceGetLoginUserIdList(&list) != 0)
        return -1;
    for (int i = 0; i < 4; i++)
    {
        const int32_t user = list.user_id[i];
        if (user <= PS5LOOP_UNASSIGNED_USER)
            continue;
        bool taken = false;
        for (int n = 0; n < PS5LOOP_PADS && !taken; n++)
            taken = g_pad.slot[n].open && g_pad.slot[n].user_id == user;
        if (!taken)
            return user;
    }
    return -1;
}

// Create virtual pad n and give it to a user. Returns 0 when samples reach it;
// the report says what every step returned either way. Pad 1 defaults to the
// foreground user; pads 2-4 to the next signed-in user without a pad (none: the
// console asks "Who's using this controller?").
static int pad_open(int n, int32_t user_id, bool evict, char *report, size_t size)
{
    pad_slot_t *const pad = &g_pad.slot[n];
    if (pad->open)
    {
        note(report, size, "already open");
        return 0;
    }
    if (pad_libraries(report, size) != 0)
        return -1;
    if (user_id <= PS5LOOP_UNASSIGNED_USER)
    {
        if (n > 0)
        {
            user_id = pad_free_user();
            if (user_id < 0)
                note(report, size, "no-free-user");
        }
        else if (sceUserServiceGetForegroundUser(&user_id) != 0)
            user_id = -1;
    }
    note(report, size, "pad=%d user=0x%08x", n + 1, (uint32_t)user_id);

    // The log replays what it holds to a new reader: let that pass, so the
    // device line that follows is this device's.
    const int log = klog_open();
    if (log < 0)
        note(report, size, "klog=unavailable");
    else
    {
        char drain[1024];
        while (klog_read(log, drain, sizeof(drain), 300) > 0)
            continue;
    }

    // Created for user 1 the device arrives with no owner, and is then bound.
    // Created for the real user, AddDevice answers 0x803b0001 and makes nothing
    // (firmware 13.60).
    add_device_param_t param;
    memset(&param, 0, sizeof(param));
    param.size = (int32_t)sizeof(param);
    param.user_id = PS5LOOP_UNASSIGNED_USER;
    const int32_t added = scePadVirtualDeviceAddDevice(&param, PS5LOOP_DEVICE_TYPE);
    note(report, size, "add=0x%08x", (uint32_t)added);

    uint64_t device_id = 0;
    if (log >= 0)
        device_id = klog_wait_device(log, 3000, report, size);
    note(report, size, "device=0x%llx", (unsigned long long)device_id);
    if (added != 0 && (uint32_t)added != PS5LOOP_ASSIGNMENT_PENDING && device_id == 0)
    {
        if (log >= 0)
            close(log);
        return -1;
    }

    // Without this the console shows "Who's using this controller?" and waits
    // for a person: binding is what choosing a profile there does.
    if (device_id != 0 && user_id > PS5LOOP_UNASSIGNED_USER && mbus_bind != NULL)
        note(report, size, "bind=0x%08x", (uint32_t)mbus_bind(device_id, user_id));
    else
        note(report, size, "bind=skipped");

    if (log >= 0)
        close(log);

    // A title reads its user's first controller; while a real one holds that
    // place the virtual pad is the second and no title opens it. Read the login
    // manager's controller table from the kernel log and disconnect the others,
    // so the virtual pad is first. (PS on a real pad brings it back.) --beside
    // keeps them, for the home screen.
    if (evict && mbus_disconnect != NULL)
        klog_evict_cim(device_id, report, size);

    const int32_t handle = pad_find_handle(added, &param, device_id, report, size);
    if (handle < 0)
    {
        pad->device_id = device_id;
        return -1;
    }
    pthread_mutex_lock(&g_pad.lock);
    pad_neutral(&pad->state);
    pad->handle = handle;
    pad->device_id = device_id;
    pad->user_id = user_id;
    pad->inserts = 0;
    pad->last_insert = 0;
    pad->open = true;
    pthread_mutex_unlock(&g_pad.lock);
    return 0;
}

static void pad_close(int n, char *report, size_t size)
{
    pad_slot_t *const pad = &g_pad.slot[n];
    pthread_mutex_lock(&g_pad.lock);
    const bool was_open = pad->open;
    const int32_t handle = pad->handle;
    pad->open = false;
    pad->handle = -1;
    pthread_mutex_unlock(&g_pad.lock);
    if (!was_open)
    {
        note(report, size, "not open");
        return;
    }
    pad_data_t neutral;
    pad_neutral(&neutral);
    (void)pad_insert(handle, &neutral);
    note(report, size, "delete=0x%08x", (uint32_t)scePadVirtualDeviceDeleteDevice(handle));
}

// Close every open pad (quit).
static void pad_close_all(char *report, size_t size)
{
    for (int n = 0; n < PS5LOOP_PADS; n++)
        if (g_pad.slot[n].open)
        {
            note(report, size, "pad%d:", n + 1);
            pad_close(n, report, size);
        }
}

// The first open pad, or -1: what the screenshot shortcut presses Create on.
static int pad_first_open(void)
{
    for (int n = 0; n < PS5LOOP_PADS; n++)
        if (g_pad.slot[n].open)
            return n;
    return -1;
}

// Press and release on pad n, timed here and not across the network.
static void pad_tap(int n, uint32_t buttons, int hold_ms)
{
    pad_hold(n, buttons, 128, 128, 128, 128, 0, 0);
    usleep((useconds_t)hold_ms * 1000u);
    pad_hold(n, 0, 128, 128, 128, 128, 0, 0);
    usleep(50000);
}

/* The newest captures found by one walk of the capture folders. */
#define PS5LOOP_NEWEST 5
typedef struct capture
{
    char path[512];
    time_t modified;
    off_t size;
} capture_t;

static bool is_picture(const char *name)
{
    const char *const extension = strrchr(name, '.');
    return extension != NULL &&
           (strcasecmp(extension, ".jxr") == 0 || strcasecmp(extension, ".png") == 0 ||
            strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0);
}

// Walk a folder for pictures, keeping the newest, newest first.
static void captures_walk(const char *folder, int depth, capture_t *newest)
{
    DIR *const directory = opendir(folder);
    if (directory == NULL)
        return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;
        capture_t found;
        snprintf(found.path, sizeof(found.path), "%s/%s", folder, entry->d_name);
        struct stat status;
        if (stat(found.path, &status) != 0)
            continue;
        if (S_ISDIR(status.st_mode))
        {
            if (depth < 6)
                captures_walk(found.path, depth + 1, newest);
            continue;
        }
        if (!S_ISREG(status.st_mode) || !is_picture(entry->d_name))
            continue;
        found.modified = status.st_mtime;
        found.size = status.st_size;
        for (int index = 0; index < PS5LOOP_NEWEST; index++)
        {
            if (found.modified <= newest[index].modified)
                continue;
            memmove(&newest[index + 1], &newest[index],
                    sizeof(capture_t) * (size_t)(PS5LOOP_NEWEST - 1 - index));
            newest[index] = found;
            break;
        }
    }
    closedir(directory);
}

static void send_file(int client, const capture_t *capture);

static void captures_newest(capture_t *newest)
{
    memset(newest, 0, sizeof(capture_t) * PS5LOOP_NEWEST);
    for (size_t root = 0; root < sizeof(capture_roots) / sizeof(capture_roots[0]); root++)
    {
        captures_walk(capture_roots[root], 0, newest);
        if (newest[0].path[0] != '\0')
            return;
    }
}

// Send a file: a header line with its size, then its bytes.
static void send_file(int client, const capture_t *capture)
{
    const int file = open(capture->path, O_RDONLY);
    if (file < 0)
    {
        reply(client, "err open %s: %s", capture->path, strerror(errno));
        return;
    }
    reply(client, "ok file size=%lld mtime=%lld path=%s", (long long)capture->size,
          (long long)capture->modified, capture->path);
    char buffer[65536];
    ssize_t bytes;
    while ((bytes = read(file, buffer, sizeof(buffer))) > 0)
    {
        for (ssize_t sent = 0; sent < bytes;)
        {
            const ssize_t step = out(client, buffer + sent, (size_t)(bytes - sent), 0);
            if (step <= 0)
            {
                close(file);
                return;
            }
            sent += step;
        }
    }
    close(file);
}

// The credentials the system's services accept from a payload.
static void elevate(char *report, size_t size)
{
    static const uint8_t all_caps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const int authid = kernel_set_ucred_authid(getpid(), PS5LOOP_AUTHID);
    const int caps = kernel_set_ucred_caps(getpid(), all_caps);
    note(report, size, "authid=%d caps=%d", authid, caps);
}

// Load the screenshot and content-delete libraries. Once per process.
static void capture_libraries(char *report, size_t size)
{
    if (g_capture_libraries)
        return;
    g_capture_libraries = true;
    elevate(report, size);
    void *const screenshot = dlopen("/system/common/lib/libSceScreenShot.sprx", RTLD_NOW);
    if (screenshot != NULL)
        screenshot_capture = (int32_t (*)(int32_t, char *, int (*)(void *), void *))dlsym(
            screenshot, "sceScreenShotCapture");
    void *const deleter = dlopen("/system/common/lib/libSceContentDelete.sprx", RTLD_NOW);
    if (deleter != NULL)
    {
        int32_t (*const initialize)(const content_delete_param_t *) =
            (int32_t (*)(const content_delete_param_t *))dlsym(deleter,
                                                               "sceContentDeleteInitialize");
        content_delete_param_t param;
        memset(&param, 0, sizeof(param));
        param.buffer_size = PS5LOOP_CONTENT_DELETE_BUFFER;
        const int32_t initialized = initialize != NULL ? initialize(&param) : -1;
        note(report, size, "deleteInit=0x%08x", (uint32_t)initialized);
        if (initialized == 0)
            content_delete_by_path =
                (int32_t (*)(const char *))dlsym(deleter, "sceContentDeleteByPath");
    }
    note(report, size, "capture=%s delete=%s", screenshot_capture != NULL ? "found" : "missing",
         content_delete_by_path != NULL ? "found" : "missing");
    log_line("ps5loop: capture libraries: %s", report);
}

// Remove a capture the way the gallery does, so that a run leaves no picture,
// thumbnail or database row behind. The service knows captures by their path
// under /user, with or without that prefix: try both.
static int32_t capture_delete(const char *path)
{
    if (content_delete_by_path == NULL)
        return -1;
    int32_t result = content_delete_by_path(path);
    if (result != 0 && strncmp(path, "/user/", 6) == 0)
        result = content_delete_by_path(path + 5);
    log_line("ps5loop: delete %s -> 0x%08x", path, (uint32_t)result);
    return result;
}

// Take the console's own screenshot: hold Create as a person would, wait for a
// picture newer than the press whose size has stopped growing, send it, and
// delete it from the console. (Asking the screenshot service instead,
// "direct-screenshot", never answers a payload.)
static void take_shot(int client, int hold_ms)
{
    char report[PS5LOOP_LINE - 128] = {0};
    const int shooter = pad_first_open(); // Create works from any controller
    if (shooter < 0)
    {
        reply(client,
              "err screenshot needs the pad: pad open first (latest-capture sends the newest one)");
        return;
    }
    capture_libraries(report, sizeof(report));
    capture_t before[PS5LOOP_NEWEST];
    captures_newest(before);
    pad_tap(shooter, PS5LOOP_BUTTON_CREATE, hold_ms);
    off_t last_size = -1;
    const uint64_t deadline = monotonic_us() + PS5LOOP_SHOT_WAIT_MS * 1000ull;
    while (monotonic_us() < deadline)
    {
        usleep(300000);
        capture_t after[PS5LOOP_NEWEST];
        captures_newest(after);
        if (after[0].path[0] == '\0' || strcmp(after[0].path, before[0].path) == 0)
            continue;
        if (after[0].size > 0 && after[0].size == last_size)
        {
            send_file(client, &after[0]);
            (void)capture_delete(after[0].path);
            return;
        }
        last_size = after[0].size;
    }
    reply(client, "err screenshot no new capture within %d ms (newest is still %s)", PS5LOOP_SHOT_WAIT_MS,
          before[0].path[0] != '\0' ? before[0].path : "none");
}

/* ---- Controlling the console: files, processes, apps, power. ---------------
 *
 * These reach the console's own services, which the payload's credentials
 * (elevate()) let it use. Paths are the console's real paths; a file op escapes
 * the payload's sandbox for the length of the op so that system paths resolve
 * (as the PS5 payload SDK's list_files sample does). */

static void elevate(char *report, size_t size);

// Raise this process to the console's credentials, discarding the report.
static void ensure_elevated(void)
{
    char report[80] = {0};
    elevate(report, sizeof(report));
}

// Run against the whole filesystem, not the payload's jail, for one operation.
static intptr_t fs_escape(void)
{
    const pid_t pid = getpid();
    const intptr_t saved = kernel_get_proc_rootdir(pid);
    kernel_set_proc_rootdir(pid, kernel_get_root_vnode());
    return saved;
}

static void fs_restore(intptr_t saved)
{
    kernel_set_proc_rootdir(getpid(), saved);
}

// Send a header line then size bytes of a buffer, so the client reads it as a
// file: a text answer (ps, ls) or a whole file (get) travel the same way.
static void send_bytes(int client, const char *what, const char *buffer, size_t size)
{
    reply(client, "ok file size=%zu mtime=0 path=%s", size, what);
    for (size_t sent = 0; sent < size;)
    {
        const ssize_t step = out(client, buffer + sent, size - sent, 0);
        if (step <= 0)
            break;
        sent += (size_t)step;
    }
}

// Append printf output to a growing text buffer; returns the buffer (possibly
// moved) or NULL if it could not grow.
static char *text_append(char *text, size_t *cap, size_t *used, const char *format, ...)
    __attribute__((format(printf, 4, 5)));
static char *text_append(char *text, size_t *cap, size_t *used, const char *format, ...)
{
    if (text == NULL)
        return NULL;
    if (*cap - *used < 1280)
    {
        char *const bigger = realloc(text, *cap * 2);
        if (bigger == NULL)
        {
            free(text);
            return NULL;
        }
        text = bigger;
        *cap *= 2;
    }
    va_list arguments;
    va_start(arguments, format);
    *used += (size_t)vsnprintf(text + *used, *cap - *used, format, arguments);
    va_end(arguments);
    return text;
}

// ps: every process, with the title id of the ones that are apps.
typedef struct app_info
{
    uint32_t app_id;
    uint64_t unknown1;
    uint32_t app_type;
    char title_id[10];
    char unknown2[0x3c];
} app_info_t;
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

static void list_processes(int client)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0)
    {
        reply(client, "err ps: %s", strerror(errno));
        return;
    }
    char *const procs = malloc(len);
    if (procs == NULL || sysctl(mib, 4, procs, &len, NULL, 0) != 0)
    {
        free(procs);
        reply(client, "err ps: %s", strerror(errno));
        return;
    }
    size_t cap = 1u << 16, used = 0;
    char *text = malloc(cap);
    text = text_append(text, &cap, &used, "%6s %6s %-10s %-12s %s\n", "PID", "PPID", "APPID",
                       "TITLEID", "COMMAND");
    for (char *ptr = procs; text != NULL && ptr + sizeof(struct kinfo_proc) <= procs + len;)
    {
        const struct kinfo_proc *const ki = (const struct kinfo_proc *)ptr;
        if (ki->ki_structsize == 0)
            break;
        ptr += ki->ki_structsize;
        app_info_t info;
        if (sceKernelGetAppInfo(ki->ki_pid, &info) != 0)
            memset(&info, 0, sizeof(info));
        char app_id[12] = "-";
        if (info.app_id != 0)
            snprintf(app_id, sizeof(app_id), "0x%x", info.app_id);
        text = text_append(text, &cap, &used, "%6u %6u %-10s %-12s %s\n", (unsigned)ki->ki_pid,
                           (unsigned)ki->ki_ppid, app_id,
                           info.title_id[0] != '\0' ? info.title_id : "-", ki->ki_comm);
    }
    free(procs);
    if (text == NULL)
        reply(client, "err ps: out of memory");
    else
        send_bytes(client, "ps", text, used);
    free(text);
}

// ls: one line per entry, "<type> <size> <name>".
static void list_dir(int client, const char *path)
{
    const intptr_t saved = fs_escape();
    DIR *const dir = opendir(path);
    if (dir == NULL)
    {
        const int err = errno;
        fs_restore(saved);
        reply(client, "err ls %s: %s", path, strerror(err));
        return;
    }
    size_t cap = 1u << 16, used = 0;
    char *text = malloc(cap);
    struct dirent *entry;
    while (text != NULL && (entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char full[1100];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        struct stat st;
        const char kind = stat(full, &st) == 0 ? (S_ISDIR(st.st_mode) ? 'd' : '-') : '?';
        text = text_append(text, &cap, &used, "%c %12lld %s\n", kind,
                           kind == '-' ? (long long)st.st_size : 0LL, entry->d_name);
    }
    closedir(dir);
    fs_restore(saved);
    if (text == NULL)
        reply(client, "err ls: out of memory");
    else
        send_bytes(client, path, text, used);
    free(text);
}


// get: send a file's bytes.
static void get_file(int client, const char *path)
{
    const intptr_t saved = fs_escape();
    const int file = open(path, O_RDONLY);
    struct stat st;
    if (file < 0 || fstat(file, &st) != 0)
    {
        const int err = errno;
        if (file >= 0)
            close(file);
        fs_restore(saved);
        reply(client, "err get %s: %s", path, strerror(err));
        return;
    }
    reply(client, "ok file size=%lld mtime=%lld path=%s", (long long)st.st_size,
          (long long)st.st_mtime, path);
    char buffer[65536];
    for (;;)
    {
        const ssize_t got = read(file, buffer, sizeof(buffer));
        if (got <= 0)
            break;
        bool broken = false;
        for (ssize_t sent = 0; sent < got;)
        {
            const ssize_t step = out(client, buffer + sent, (size_t)(got - sent), 0);
            if (step <= 0)
            {
                broken = true;
                break;
            }
            sent += step;
        }
        if (broken)
            break;
    }
    close(file);
    fs_restore(saved);
}

// put: write size bytes to path. `body` is whatever of them already arrived
// with the command line; the rest is read from the connection.
static void put_file(int client, const char *path, size_t size, const char *body, size_t body_len)
{
    const intptr_t saved = fs_escape();
    const int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    fs_restore(saved);
    if (file < 0)
    {
        reply(client, "err put %s: %s", path, strerror(errno));
        return;
    }
    size_t written = 0;
    if (body_len > size)
        body_len = size;
    if (body_len > 0 && write(file, body, body_len) == (ssize_t)body_len)
        written = body_len;
    char buffer[65536];
    while (written < size)
    {
        const size_t want = size - written < sizeof(buffer) ? size - written : sizeof(buffer);
        const ssize_t got = recv(client, buffer, want, 0);
        if (got <= 0 || write(file, buffer, (size_t)got) != got)
            break;
        written += (size_t)got;
    }
    close(file);
    if (written == size)
        reply(client, "ok put %zu bytes %s", written, path);
    else
        reply(client, "err put %s: wrote %zu of %zu bytes", path, written, size);
}

static void remove_path(int client, const char *path)
{
    const intptr_t saved = fs_escape();
    const int result = unlink(path);
    const int err = errno;
    fs_restore(saved);
    reply(client, result == 0 ? "ok rm %s" : "err rm %s: %s", path, strerror(err));
}

static void move_path(int client, const char *from, const char *to)
{
    const intptr_t saved = fs_escape();
    const int result = rename(from, to);
    const int err = errno;
    fs_restore(saved);
    if (result == 0)
        reply(client, "ok mv %s %s", from, to);
    else
        reply(client, "err mv %s -> %s: %s", from, to, strerror(err));
}

// open-url: the console's web browser, as the SDK's browser sample launches it.
int sceSystemServiceLaunchWebBrowser(const char *uri, void *param);

// settings: read the system registry as JSON. There is no privileged "dump all"
// call (the tool-enumeration interface answers 0x800d0218), but every value is
// readable by its regId, which is (category << 24) | (sub << 16) | entry. Scan
// that space: an int with sceRegMgrGetInt, else a string with sceRegMgrGetStr.
// A (cat, sub) whose first entries are all empty is skipped, so the scan is
// quick; the keys are the regIds in hex.
int sceRegMgrGetInt(uint32_t reg_id, int *value);
int sceRegMgrGetStr(uint32_t reg_id, char *value, size_t size);
int sceRegMgrSetInt(uint32_t reg_id, int value);
static void json_escape(char *out, size_t size, const char *in);

// Apps: launch a title by id, close a running app by its app id (from ps).
int sceSystemServiceLaunchApp(const char *title_id, const char *argv[], void *param);
int sceLncUtilKillApp(uint32_t app_id);
int sceSystemServiceGetAppIdOfRunningBigApp(void);

static void close_app(int client, const char *app_id_text)
{
    ensure_elevated();
    const uint32_t app_id = app_id_text != NULL ? (uint32_t)strtoul(app_id_text, NULL, 0)
                                                : (uint32_t)sceSystemServiceGetAppIdOfRunningBigApp();
    if (app_id == 0)
    {
        reply(client, "err close-app: no app id (give one from ps, or no big app is running)");
        return;
    }
    const int32_t result = sceLncUtilKillApp(app_id);
    reply(client, "%s close-app appId=0x%x result=0x%08x", result == 0 ? "ok" : "err", app_id,
          (uint32_t)result);
}

// locate-title: what is in the foreground right now. The running "big app" gives
// the app id; the process list maps it to a pid, title id and command.
static void locate_title(int client)
{
    ensure_elevated();
    const int big = sceSystemServiceGetAppIdOfRunningBigApp();
    if (big <= 0)
    {
        reply(client, "ok locate-title none (no foreground app)");
        return;
    }
    const uint32_t app_id = (uint32_t)big;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t len = 0;
    char title[12] = "-", comm[32] = "-";
    int found_pid = -1;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) == 0)
    {
        char *const procs = malloc(len);
        if (procs != NULL && sysctl(mib, 4, procs, &len, NULL, 0) == 0)
        {
            for (char *ptr = procs; ptr + sizeof(struct kinfo_proc) <= procs + len;)
            {
                const struct kinfo_proc *const ki = (const struct kinfo_proc *)ptr;
                if (ki->ki_structsize == 0)
                    break;
                ptr += ki->ki_structsize;
                app_info_t info;
                if (sceKernelGetAppInfo(ki->ki_pid, &info) == 0 && info.app_id == app_id)
                {
                    found_pid = ki->ki_pid;
                    snprintf(title, sizeof(title), "%s", info.title_id[0] != '\0' ? info.title_id : "-");
                    snprintf(comm, sizeof(comm), "%s", ki->ki_comm);
                    break;
                }
            }
        }
        free(procs);
    }
    reply(client, "ok locate-title appId=0x%x pid=%d titleId=%s comm=%s", app_id, found_pid, title,
          comm);
}

// exec: run a binary and send back what it writes to stdout/stderr, then its
// exit code. posix_spawn runs through the sandbox escape, so an absolute console
// path resolves. A command DevLoop already has (ls, ps, ...) is usually better;
// this is for running homebrew tools that live on the console.
extern char **environ;

// Power: off and reboot do not return when they take; standby is "rest". The
// reply goes out first, since the request may end the process before it would.
int sceSystemServiceRequestPowerOff(void);
int sceSystemServiceRequestReboot(void);
int sceSystemStateMgrEnterStandby(void);

// uninstall: remove an installed title through the app installer service.
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppUnInstall(const char *title_id);

// install: install a .pkg already on the console, through the app installer.
// Verification is limited without a package to try; the call returns its result
// and the installer's status word. The path is a console path (e.g. /data/x.pkg).
int sceAppInstUtilAppInstallPkg(const char *path, int *status);

// A static buffer whose address ping reports, so peek/poke can be verified.
static char g_probe[32] = "DEVLOOP-MDBG-PROBE";

// notify: an on-screen message. The system draws these, so they work from a
// background payload (modal dialogs would need the caller's own video context).
//  - toast:  the kernel notification request (a 45-byte header then the
//            message) shows the familiar banner.
//  - flyout: the notification service draws the richer interactive card, from a
//            full JSON template (an incomplete one is accepted but not drawn).
int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);
int sceNotificationSend(int user_id, bool is_logged, const char *payload);

typedef struct notify_request
{
    char header[45];
    char message[3075];
} notify_request_t;

// Escape a string for embedding in a JSON double-quoted value.
static void json_escape(char *out, size_t size, const char *in)
{
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 2 < size; i++)
    {
        if (in[i] == '"' || in[i] == '\\')
            out[j++] = '\\';
        if ((unsigned char)in[i] >= 0x20)
            out[j++] = in[i];
    }
    out[j] = '\0';
}

// Send a flyout card; returns sceNotificationSend's result (-1 on out of memory).
static int32_t flyout_send(const char *message)
{
    char safe[320];
    json_escape(safe, sizeof(safe), message);
    char *const json = malloc(2048);
    if (json == NULL)
        return -1;
    snprintf(json, 2048,
             "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
             "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\","
             "\"isImmediate\":true,\"priority\":100,\"viewData\":{\"icon\":{\"type\":\"Predefined\","
             "\"parameters\":{\"icon\":\"download\"}},\"message\":{\"body\":\"%s\"},"
             "\"subMessage\":{\"body\":\"DevLoop\"}},\"platformViews\":{\"previewDisabled\":"
             "{\"viewData\":{\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
             "\"message\":{\"body\":\"%s\"}}}}},\"createdDateTime\":\"2025-01-01T00:00:00.000Z\","
             "\"localNotificationId\":\"%u\"}",
             safe, safe, (unsigned)(monotonic_us() & 0x7fffffffu));
    const int32_t result = sceNotificationSend(0xFE, true, json);
    free(json);
    return result;
}

// The console's LAN address, as the routing table picks it: connecting a UDP
// socket sends nothing, it only chooses the source address getsockname reports.
// False when there is no route yet (the network is still coming up).
static bool local_address(char *out, size_t size)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return false;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0xC0000201u); // 192.0.2.1 (TEST-NET-1): any routed address does
    struct sockaddr_in me;
    socklen_t length = sizeof(me);
    memset(&me, 0, sizeof(me));
    const bool ok = connect(sock, (struct sockaddr *)&to, sizeof(to)) == 0 &&
                    getsockname(sock, (struct sockaddr *)&me, &length) == 0 &&
                    me.sin_addr.s_addr != htonl(INADDR_ANY) &&
                    me.sin_addr.s_addr != htonl(INADDR_LOOPBACK) &&
                    inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)size) != NULL;
    close(sock);
    return ok;
}

// At start, a flyout with the dashboard's address, so a browser can be pointed at
// it without looking anything up. A payload loaded at boot may run before the
// network is up, so this waits (up to a minute) for an address before giving up
// on it and showing the port alone.
static void *announce_thread(void *argument)
{
    const unsigned port = (unsigned)(uintptr_t)argument;
    char address[INET_ADDRSTRLEN] = "";
    for (int tries = 0; tries < 60 && !local_address(address, sizeof(address)); tries++)
        usleep(1000000);
    ensure_elevated();
    char message[160];
    if (address[0] != '\0')
        snprintf(message, sizeof(message), "DevLoop dashboard: http://%s:%u/", address, port);
    else
        snprintf(message, sizeof(message), "DevLoop is running: dashboard on port %u", port);
    const int32_t result = flyout_send(message);
    log_line("ps5loop: announced %s (0x%08x)", address[0] != '\0' ? address : "no address",
             (uint32_t)result);
    return NULL;
}

// hwinfo: model, serial, temperatures and cpu clock (the SDK's hwinfo sample).
int sceKernelGetHwModelName(char *);
int sceKernelGetHwSerialNumber(char *);
long sceKernelGetCpuFrequency(void);
int sceKernelGetCpuTemperature(int *);
int sceKernelGetSocSensorTemperature(int, int *);
// Current (not manual) fan duty: raw value, percent = raw/10. The /dev/icc_fan
// GET_DUTY ioctl only reports the *manual* duty (0 in auto mode), so use this.
int sceKernelGetCurrentFanDuty(uint16_t *, void *);
// SoC power, from the AMD power-management processor (/dev/mp1, ioctl 0xc070ae1f).
// It fills 0x70 bytes: 28 little-endian uint32. Probed on 12.20: [0..2] and [3..5]
// are two supply rails, each as power mW, voltage mV, current mA (mW = mV x mA
// holds on every sample); the rest (what looks like per-rail peaks, an input
// voltage and a temperature) is not reported here. Which rail feeds the CPU and
// which the GPU is not established, so they are reported as rails A and B.
int sceKernelGetSocPowerConsumption(void *);
// Identity and state, as orezraey's 13.60 probes measured them. On 12.20 the SoC
// id, CPU mode and version calls answer the same way (probed); the cpumask call
// is not exported there, so cpuMask stays 0 and only hw.ncpu is known:
//   sceKernelGetMainSocId()        the SoC's id word (0x00840f80 on a CFI-2014)
//   sceKernelGetCpumode()          the CPU mode, a small integer
//   sceKernelGetAvailableCpumask() one bit per core this process may run on
//   sceKernelGetProsperoSystemSwVersion(v)  0x28 bytes: a size, the version as a
//                                  string ("13.600.007") and as the firmware word
int sceKernelGetMainSocId(void);
int sceKernelGetCpumode(void);
// (the cpumask call has no SDK stub and is missing on 12.20: it is looked up in
// libkernel at run time and skipped when absent)
typedef struct sw_version
{
    uint64_t size;
    char text[0x1c];
    uint32_t version;
} sw_version_t;
int sceKernelGetProsperoSystemSwVersion(sw_version_t *);
#define HWINFO_POWER_WORDS 28
// Per-core clocks: libkernel asks the SMU (/dev/mp1, ioctl 0xc00cae1e
// {u64 core; u32 MHz}) once per core and fills eight uint32 MHz values
// (probed on 12.20: 8 x 3200 with a game in front).
int sceKernelGetCpuCoreClock(uint32_t *);
#define HWINFO_CORES 8

// The SoC exposes many thermal sensors, not one: sceKernelGetSocSensorTemperature
// answers for ids 0..63 (probed on 13.60; a few succeed on 12.20). Scan them all
// and keep the plausible readings, so hwinfo reports the whole thermal picture
// and the hottest point, not just sensor 0.
#define HWINFO_SOC_SENSORS 64

typedef struct hwinfo
{
    uint32_t fw;
    char model[256];
    char serial[256];
    long cpuMHz;
    int cpuC;
    int soc0;    // sensor 0, kept as the back-compatible socC
    int socMax;  // the hottest valid sensor
    int socMaxId;
    int socN;    // how many sensors answered plausibly
    int fanPct;  // current fan duty 0..100, or -1 if the call is unavailable
    uint32_t socId;
    int cpuMode;
    int ncpu;         // hw.ncpu: the cores the SoC has
    uint64_t cpuMask; // the cores available to this process
    long bootTime;    // kern.boottime, unix seconds; 0 if unavailable
    long uptime;      // seconds since boot (the monotonic clock)
    char sw[0x20];    // the system software version as the system prints it
    bool powerOk; // the SoC power call answered
    uint32_t railmW[2], railmV[2], railmA[2];
    bool coresOk; // the per-core clock call answered
    uint32_t coreMHz[HWINFO_CORES];
    int socIds[HWINFO_SOC_SENSORS];
    int socVals[HWINFO_SOC_SENSORS];
} hwinfo_t;

static void hwinfo_read(hwinfo_t *h)
{
    memset(h, 0, sizeof(*h));
    strcpy(h->model, "?");
    strcpy(h->serial, "?");
    h->fw = kernel_get_fw_version();
    (void)sceKernelGetHwModelName(h->model);
    (void)sceKernelGetHwSerialNumber(h->serial);
    (void)sceKernelGetCpuTemperature(&h->cpuC);
    h->cpuMHz = sceKernelGetCpuFrequency() / 1000000;
    h->socMax = 0;
    h->socMaxId = -1;
    for (int id = 0; id < HWINFO_SOC_SENSORS; id++)
    {
        int value = 0;
        if (sceKernelGetSocSensorTemperature(id, &value) != 0)
            continue;
        if (value < -40 || value > 150) // drop the known anomalous reading (id 64 ~ -49)
            continue;
        h->socIds[h->socN] = id;
        h->socVals[h->socN] = value;
        h->socN++;
        if (id == 0)
            h->soc0 = value;
        if (h->socMaxId < 0 || value > h->socMax)
        {
            h->socMax = value;
            h->socMaxId = id;
        }
    }
    h->fanPct = -1;
    {
        uint16_t raw = 0;
        unsigned char scratch[64] = {0};
        if (sceKernelGetCurrentFanDuty(&raw, scratch) == 0)
        {
            int p = raw / 10;
            if (p > 100)
                p = 100;
            h->fanPct = p;
        }
    }
    h->socId = (uint32_t)sceKernelGetMainSocId();
    h->cpuMode = sceKernelGetCpumode();
    {
        static uint64_t (*cpumask)(void);
        if (cpumask == NULL)
        {
            void *const kernel = dlopen("libkernel_sys.sprx", RTLD_NOW | RTLD_GLOBAL);
            if (kernel != NULL)
                cpumask = (uint64_t (*)(void))dlsym(kernel, "sceKernelGetAvailableCpumask");
        }
        if (cpumask != NULL)
            h->cpuMask = cpumask();
    }
    {
        size_t length = sizeof(h->ncpu);
        if (sysctlbyname("hw.ncpu", &h->ncpu, &length, NULL, 0) != 0)
            h->ncpu = 0;
        struct timeval boot = {0};
        length = sizeof(boot);
        if (sysctlbyname("kern.boottime", &boot, &length, NULL, 0) == 0)
            h->bootTime = (long)boot.tv_sec;
        struct timespec now = {0};
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
            h->uptime = (long)now.tv_sec;
        sw_version_t version;
        memset(&version, 0, sizeof(version));
        version.size = sizeof(version);
        if (sceKernelGetProsperoSystemSwVersion(&version) == 0)
        {
            version.text[sizeof(version.text) - 1] = '\0';
            snprintf(h->sw, sizeof(h->sw), "%s", version.text);
        }
    }
    {
        uint32_t words[HWINFO_POWER_WORDS + 4] = {0}; // a little slack past the 0x70 bytes
        if (sceKernelGetSocPowerConsumption(words) == 0)
        {
            h->powerOk = true;
            for (int rail = 0; rail < 2; rail++)
            {
                h->railmW[rail] = words[rail * 3];
                h->railmV[rail] = words[rail * 3 + 1];
                h->railmA[rail] = words[rail * 3 + 2];
            }
        }
    }
    {
        uint32_t cores[HWINFO_CORES * 2] = {0}; // slack, in case a firmware reports more
        if (sceKernelGetCpuCoreClock(cores) == 0)
        {
            h->coresOk = true;
            memcpy(h->coreMHz, cores, sizeof(h->coreMHz));
        }
    }
}

static void hwinfo(int client)
{
    hwinfo_t h;
    hwinfo_read(&h);
    reply(client,
          "ok fw=0x%08x model=%s serial=%s cpuC=%d socC=%d socMax=%d socMaxId=%d socN=%d cpuMHz=%ld "
          "fanPct=%d socmW=%d railAmW=%d railBmW=%d sw=%s socId=0x%08x cpuMode=%d ncpu=%d "
          "cpuMask=0x%llx uptime=%ld",
          h.fw, h.model, h.serial, h.cpuC, h.soc0, h.socMax, h.socMaxId, h.socN, h.cpuMHz, h.fanPct,
          h.powerOk ? (int)(h.railmW[0] + h.railmW[1]) : -1, h.powerOk ? (int)h.railmW[0] : -1,
          h.powerOk ? (int)h.railmW[1] : -1, h.sw[0] != '\0' ? h.sw : "?", h.socId, h.cpuMode,
          h.ncpu, (unsigned long long)h.cpuMask, h.uptime);
}

// The same reading as a JSON object, for the dashboard the console hosts. The
// caller frees the buffer; NULL on out of memory.
static char *hwinfo_json(void)
{
    hwinfo_t h;
    hwinfo_read(&h);
    char model[520], serial[520], sw[80];
    json_escape(sw, sizeof(sw), h.sw);
    json_escape(model, sizeof(model), h.model);
    json_escape(serial, sizeof(serial), h.serial);
    size_t cap = 1u << 12, used = 0;
    char *json = malloc(cap);
    json = text_append(json, &cap, &used,
                       "{\"ok\":true,\"fw\":\"0x%08x\",\"model\":\"%s\",\"serial\":\"%s\","
                       "\"cpuC\":%d,\"socC\":%d,\"socMax\":%d,\"socMaxId\":%d,\"socN\":%d,"
                       "\"cpuMHz\":%ld,\"fanPct\":%d,",
                       h.fw, model, serial, h.cpuC, h.soc0, h.socMax, h.socMaxId, h.socN, h.cpuMHz,
                       h.fanPct);
    json = text_append(json, &cap, &used,
                       "\"sw\":\"%s\",\"socId\":\"0x%08x\",\"cpuMode\":%d,\"ncpu\":%d,"
                       "\"cores\":%d,\"cpuMask\":\"0x%llx\",\"bootTime\":%ld,\"uptime\":%ld,",
                       sw, h.socId, h.cpuMode, h.ncpu, __builtin_popcountll(h.cpuMask),
                       (unsigned long long)h.cpuMask, h.bootTime, h.uptime);
    // "power": per-rail readings, or null when the call is unavailable
    if (h.powerOk)
        json = text_append(json, &cap, &used,
                           "\"power\":{\"mW\":[%u,%u],\"mV\":[%u,%u],\"mA\":[%u,%u]},",
                           h.railmW[0], h.railmW[1], h.railmV[0], h.railmV[1], h.railmA[0],
                           h.railmA[1]);
    else
        json = text_append(json, &cap, &used, "\"power\":null,");
    // "pads": the virtual pads, for the dashboard's connected lights: open, and
    // whether the last sample reached the system (lastInsert 0). No user names:
    // the page shows which pad, not whose. "pad" repeats pad 1, as older pages read it.
    {
        pad_slot_t copy[PS5LOOP_PADS];
        pthread_mutex_lock(&g_pad.lock);
        memcpy(copy, g_pad.slot, sizeof(copy));
        pthread_mutex_unlock(&g_pad.lock);
        json = text_append(json, &cap, &used, "\"pads\":[");
        for (int n = 0; json != NULL && n < PS5LOOP_PADS; n++)
            json = text_append(json, &cap, &used,
                               "%s{\"n\":%d,\"open\":%s,\"device\":\"0x%llx\",\"user\":\"0x%08x\","
                               "\"lastInsert\":\"0x%08x\",\"inserts\":%u}",
                               n ? "," : "", n + 1, copy[n].open ? "true" : "false",
                               (unsigned long long)copy[n].device_id, (uint32_t)copy[n].user_id,
                               (uint32_t)copy[n].last_insert, copy[n].inserts);
        json = text_append(json, &cap, &used,
                           "],\"pad\":{\"open\":%s,\"device\":\"0x%llx\",\"user\":\"0x%08x\","
                           "\"lastInsert\":\"0x%08x\",\"inserts\":%u},",
                           copy[0].open ? "true" : "false", (unsigned long long)copy[0].device_id,
                           (uint32_t)copy[0].user_id, (uint32_t)copy[0].last_insert, copy[0].inserts);
    }
    if (h.coresOk)
    {
        json = text_append(json, &cap, &used, "\"coreMHz\":[");
        for (int i = 0; json != NULL && i < HWINFO_CORES; i++)
            json = text_append(json, &cap, &used, "%s%u", i ? "," : "", h.coreMHz[i]);
        json = text_append(json, &cap, &used, "],");
    }
    else
        json = text_append(json, &cap, &used, "\"coreMHz\":null,");
    json = text_append(json, &cap, &used, "\"socSensors\":[");
    for (int i = 0; json != NULL && i < h.socN; i++)
        json = text_append(json, &cap, &used, "%s{\"id\":%d,\"c\":%d}", i ? "," : "", h.socIds[i],
                           h.socVals[i]);
    return text_append(json, &cap, &used, "]}");
}

// thermal-probe: the temperature-looking sources libkernel reaches that hwinfo
// does not report, read raw so their meaning can be worked out. Every request
// here is one a libkernel getter makes (found by cross-referencing its /dev
// strings on 12.20) and is direction OUT only: the driver fills a buffer and
// nothing is written to the hardware. Nothing is interpreted; a failed read
// prints its errno.
#define MP1_DEV "/dev/mp1"
#define MP1_METRICS 0x406cae01ul // 108 bytes OUT
#define ICC_THERMAL_DEV "/dev/icc_thermal"
#define ICC_THERMAL_GET 0x40019003ul // 1 byte OUT

// ===== hwdetail: the ICC's counters and logs, the SMU clock table, system facts =====
// Slow-changing readings, served apart from hwinfo so the dashboard can fetch
// them every few seconds instead of four times a second. Every request is one a
// libkernel getter makes (cross-referenced on 12.20, README "Hardware calls"),
// direction OUT or a zeroed INOUT "get": nothing is written to the hardware.
// Values the probes could not decode are sent raw, as hex, for the page to show.
int sceKernelGetSocClock(void *);
#define SRTC_EPOCH 1325376000L // the ICC and srtc count seconds from 2012-01-01 UTC

// ===== Console hardware: indicator LED, fan and beeper (ICC) =====
// The indicator LED goes through /dev/icc_indicator's dynamic-LED ioctls -- a
// direct device path, so it needs no scePad handle (which SceShellUI owns, the
// blocker for lightbar output). State is a 130-byte blob: a 4-byte header
// (count at [2..3]) then 42-byte per-channel blocks { id; step_count; steps }.
// A held colour is one 5-byte step. Channels: 0x01 blue, 0x11 white, 0x21 amber.
// Protocol from PS5-Suite / ps5-beeper-LED live-hardware research.
#define ICC_INDICATOR_DEV "/dev/icc_indicator"
#define ICC_LED_STATE_SIZE 130
#define ICC_LED_GET 0x40829505ul
#define ICC_LED_SET 0x80829504ul
#define ICC_BLOCK_START 4
#define ICC_BLOCK_SIZE 42

// Fan: /dev/icc_fan. One ioctl sets the manual temperature threshold (30-90 C,
// the layout etaHEN uses: temp at byte 5); another reads the manual duty, which
// reports fan speed rather than the threshold. Changing the threshold changes
// cooling, so it is clamped and only set when asked.
#define ICC_FAN_DEV "/dev/icc_fan"
#define ICC_FAN_SET_THRESHOLD 0xC01C8F07ul
#define ICC_FAN_GET_DUTY 0xC0068F06ul

// Beeper: the sceKernelIccSetBuzzer* calls are not stubs in this SDK, so resolve
// them from a loaded libkernel at run time (PS5-Suite resolves them the same
// way). `beep` sounds a pattern (once); `mute` and `dim` are persistent.
//
// "dim" is sceKernelIccSetBuzzerDimSetting, which libkernel passes to the ICC as
// one byte (ioctl 0x8001950c on /dev/icc_indicator, no range check); the getters
// read it back (0x4001950d dim, 0x4001950f off). The ICC gives it its meaning. By
// ear on a CFI-2015 (12.20): 0 is the loudest and sounds like a different
// speaker (likely undimmed drive), 1 is softer, 2 softer still in the same tone.
// So 0 = full, 1 = soft, 2 = softest; "vol" is kept as an alias for "dim".
#define ICC_INDICATOR_BUZZER_DIM_GET 0x4001950dul
#define ICC_INDICATOR_BUZZER_OFF_GET 0x4001950ful

// df: free space per mounted volume.
static void df(int client)
{
    struct statfs *mounts = NULL;
    const int count = getmntinfo(&mounts, MNT_NOWAIT);
    size_t cap = 1u << 14, used = 0;
    char *text = malloc(cap);
    text = text_append(text, &cap, &used, "%-26s %10s %10s  %s\n", "MOUNT", "SIZE_MB", "FREE_MB",
                       "FROM");
    for (int i = 0; text != NULL && i < count; i++)
    {
        const long long bs = mounts[i].f_bsize;
        text = text_append(text, &cap, &used, "%-26s %10lld %10lld  %s\n", mounts[i].f_mntonname,
                           (long long)mounts[i].f_blocks * bs / (1024 * 1024),
                           (long long)mounts[i].f_bfree * bs / (1024 * 1024),
                           mounts[i].f_mntfromname);
    }
    if (text == NULL)
        reply(client, "err df: out of memory");
    else
        send_bytes(client, "df", text, used);
    free(text);
}

// kill: end any process by pid (close-app is for registered apps).
static void kill_pid(int client, int pid)
{
    if (kill(pid, SIGKILL) == 0)
        reply(client, "ok kill %d", pid);
    else
        reply(client, "err kill %d: %s", pid, strerror(errno));
}

#define SCAN_MAX_SPAN (16u << 20) // 16 MB per scan, to bound memory and time
#define SCAN_MAX_HITS 4096        // report at most this many matches

// Answer one command. Returns 0 to keep serving, -1 to leave the console.
// A minimal HTTP/1.1 response on the same port the text protocol uses: the
// first word of a browser's request is GET (or HEAD), which the dispatcher
// routes here. One request per connection; the server closes it after.
static void http_send(int client, const char *status, const char *ctype, const void *body,
                      size_t len, bool head_only)
{
    char header[256];
    const int n = snprintf(header, sizeof(header),
                           "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                           status, ctype, len);
    if (n > 0 && send(client, header, (size_t)n, 0) < 0)
        return;
    if (head_only || len == 0)
        return;
    for (size_t sent = 0; sent < len;)
    {
        const ssize_t step = send(client, (const char *)body + sent, len - sent, 0);
        if (step <= 0)
            break;
        sent += (size_t)step;
    }
}

// handle_command runs any text command; the dashboard's /api/run reaches it
// through http_run below, which captures the output instead of writing it.
static int handle_command(int client, char *line, const char *body, size_t body_len);

// Percent-decode a URL component into dst; '+' becomes a space.
static void url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < dst_size; i++)
    {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) &&
            isxdigit((unsigned char)src[i + 2]))
        {
            const char hex[3] = {src[i + 1], src[i + 2], '\0'};
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        else if (src[i] == '+')
            dst[o++] = ' ';
        else
            dst[o++] = src[i];
    }
    dst[o] = '\0';
}

// The content type to serve a captured "ok file" reply as, by its path's
// extension; NULL means "serve it as text" (ps, ls, settings, a text file).
static const char *content_type_for(const char *path)
{
    const char *const dot = strrchr(path, '.');
    if (dot != NULL)
    {
        if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)
            return "image/jpeg";
        if (strcasecmp(dot, ".png") == 0)
            return "image/png";
        if (strcasecmp(dot, ".jxr") == 0)
            return "image/vnd.ms-photo";
    }
    return NULL;
}

// Run one text command for a browser and wrap its output in an HTTP response.
// The command's socket output is captured (see out()): a single "ok file ..."
// reply (frame, screenshot, get, ps, ls, ...) is served as that file -- a
// picture inline, text as text -- and every other reply is served as text.
static void http_run(int client, const char *query, bool head_only)
{
    const char *c = NULL;
    if (query != NULL && strncmp(query, "c=", 2) == 0)
        c = query + 2;
    else if (query != NULL)
    {
        const char *const at = strstr(query, "&c=");
        if (at != NULL)
            c = at + 3;
    }
    if (c == NULL)
    {
        http_send(client, "400 Bad Request", "text/plain; charset=utf-8",
                  "usage: /api/run?c=<command>", 27, head_only);
        return;
    }
    char raw[PS5LOOP_LINE];
    snprintf(raw, sizeof(raw), "%s", c);
    char *const amp = strchr(raw, '&');
    if (amp != NULL)
        *amp = '\0';
    char decoded[PS5LOOP_LINE] = {0};
    url_decode(decoded, sizeof(decoded), raw);
    // These need a connection the browser cannot give (an upload body, a live
    // stream) or would end the payload: they are not offered over HTTP.
    char verb[32] = {0};
    (void)sscanf(decoded, "%31s", verb);
    if (verb[0] == '\0' || strcmp(verb, "put") == 0 || strcmp(verb, "quit") == 0 ||
        strcmp(verb, "klog-stream") == 0 || strcmp(verb, "GET") == 0 || strcmp(verb, "HEAD") == 0)
    {
        http_send(client, "400 Bad Request", "text/plain; charset=utf-8",
                  "command not available over HTTP", 31, head_only);
        return;
    }
    g_capture.len = 0;
    g_capture.oom = false;
    g_capture.on = true;
    (void)handle_command(client, decoded, NULL, 0);
    g_capture.on = false;
    if (g_capture.oom || g_capture.buf == NULL)
    {
        http_send(client, "500 Internal Server Error", "text/plain; charset=utf-8",
                  "capture failed", 14, head_only);
        return;
    }
    char *const data = g_capture.buf;
    const size_t total = g_capture.len;
    size_t head = 0;
    while (head < total && data[head] != '\n')
        head++;
    // "ok file size=N ... path=P\n" then N bytes: serve the bytes as the file.
    if (total >= 8 && memcmp(data, "ok file ", 8) == 0 && head < total)
    {
        data[head] = '\0';
        const char *const key = strstr(data, "path=");
        const char *const ctype = key != NULL ? content_type_for(key + 5) : NULL;
        http_send(client, "200 OK", ctype != NULL ? ctype : "text/plain; charset=utf-8",
                  data + head + 1, total - head - 1, head_only);
        return;
    }
    http_send(client, "200 OK", "text/plain; charset=utf-8", data, total, head_only);
}

static void http_serve_page(int client, const char *verb, const char *url)
{
    const bool head_only = strcmp(verb, "HEAD") == 0;
    if (url == NULL)
        url = "/";
    if (strcmp(url, "/") == 0 || strcmp(url, "/index.html") == 0)
    {
        http_send(client, "200 OK", "text/html; charset=utf-8", DASHBOARD_HTML, DASHBOARD_HTML_LEN,
                  head_only);
        return;
    }
    if (strcmp(url, "/api/hwinfo") == 0)
    {
        char *const json = hwinfo_json();
        if (json == NULL)
            http_send(client, "500 Internal Server Error", "text/plain", "oom", 3, head_only);
        else
            http_send(client, "200 OK", "application/json", json, strlen(json), head_only);
        free(json);
        return;
    }
    if (strncmp(url, "/api/run", 8) == 0 && (url[8] == '\0' || url[8] == '?'))
    {
        http_run(client, url[8] == '?' ? url + 9 : NULL, head_only);
        return;
    }
    http_send(client, "404 Not Found", "text/plain; charset=utf-8", "not found", 9, head_only);
}

// keyboard: a diagnostic, like `lightbar`. It invokes the system's on-screen
// keyboard (libSceImeDialog) from the payload itself, and demonstrates directly
// that a modal CommonDialog cannot be shown from a background payload rather than
// only asserting it. `sceImeDialogInit` succeeds (result=0, status RUNNING) but
// the dialog never draws: a CommonDialog renders into the *calling* process's
// framebuffer and must be pumped from its render loop, and this payload has no
// video context. Borrowing the foreground app's identity (sceKernelSetAppInfo)
// is attempted and reported too; it fails (0x80020010) and would not help, since
// the missing piece is the render loop, not the app id. The command still works
// as an async probe: "open" starts the dialog, "status" reports RUNNING (and, if
// a firmware ever rendered it, the entered text once done), "close" tears it
// down. Text is UTF-16 on the console and UTF-8 on the wire. The real path to put
// text into a field is the pad-steering typist in the dashboard (tools/ps5web.html).
#define KEYBOARD_MAX_TEXT 512

typedef struct
{
    int32_t user_id;
    int32_t type; // 0 = default, 1 = basic latin, 2 = URL, 3 = mail, 4 = number
    uint64_t supported_languages;
    int32_t enter_label;
    int32_t input_method;
    void *filter;
    uint32_t option;
    uint32_t max_text_length;
    uint16_t *input_text_buffer;
    float pos_x;
    float pos_y;
    int32_t horizontal_alignment;
    int32_t vertical_alignment;
    const uint16_t *placeholder;
    const uint16_t *title;
    int8_t reserved[16];
} ime_dialog_param_t;

typedef struct
{
    int32_t end_status; // 0 = OK, 1 = cancelled by the person, 2 = aborted
    int8_t reserved[12];
} ime_dialog_result_t;

int sceImeDialogInit(const ime_dialog_param_t *param, const void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(ime_dialog_result_t *result);
int sceImeDialogAbort(void);
int sceImeDialogTerm(void);

int sceKernelSetAppInfo(pid_t pid, app_info_t *info);

static int handle_command(int client, char *line, const char *body, size_t body_len)
{
    char report[PS5LOOP_LINE - 64] = {0};
    char *const command = strtok(line, " \t");
    if (command == NULL)
    {
        reply(client, "err empty");
        return 0;
    }
    if (strcmp(command, "GET") == 0 || strcmp(command, "HEAD") == 0)
    {
        // a browser: serve the live dashboard and its /api/hwinfo over HTTP
        http_serve_page(client, command, strtok(NULL, " \t"));
        return 0;
    }
    if (strcmp(command, "ping") == 0)
    {
        reply(client, "ok ps5loop 1 pid=%d fw=0x%08x pad=%s pads=%d probe=0x%llx kproc=0x%lx",
              (int)getpid(), kernel_get_fw_version(), g_pad.slot[0].open ? "open" : "closed",
              pads_open(), (unsigned long long)(uintptr_t)g_probe, (long)kernel_get_proc(getpid()));
        return 0;
    }
    if (strcmp(command, "quit") == 0)
    {
        pad_close_all(report, sizeof(report));
        reply(client, "ok bye %s", report);
        return -1;
    }
    if (strcmp(command, "list-captures") == 0)
    {
        capture_t newest[PS5LOOP_NEWEST];
        captures_newest(newest);
        for (int index = 0; index < PS5LOOP_NEWEST && newest[index].path[0] != '\0'; index++)
            note(report, sizeof(report), "%s(%lld)", newest[index].path,
                 (long long)newest[index].size);
        reply(client, "ok shots %s", report[0] != '\0' ? report : "none");
        return 0;
    }
    if (strcmp(command, "latest-capture") == 0)
    {
        capture_t newest[PS5LOOP_NEWEST];
        captures_newest(newest);
        if (newest[0].path[0] == '\0')
            reply(client, "err latest no captures under %s", capture_roots[0]);
        else
            send_file(client, &newest[0]);
        return 0;
    }
    if (strcmp(command, "screenshot") == 0)
    {
        const char *const hold = strtok(NULL, " \t");
        const int hold_ms = hold != NULL ? atoi(hold) : PS5LOOP_CREATE_HOLD_MS;
        take_shot(client, hold_ms > 0 && hold_ms <= 5000 ? hold_ms : PS5LOOP_CREATE_HOLD_MS);
        return 0;
    }
    if (strcmp(command, "delete-capture") == 0)
    {
        // Delete a capture as the gallery does: file, thumbnail, database row.
        const char *const path = strtok(NULL, " \t");
        capture_libraries(report, sizeof(report));
        if (path == NULL || strncmp(path, "/user/av_contents/", 18) != 0 || strstr(path, "..") != NULL)
            reply(client, "err usage: delete-capture /user/av_contents/... (got %s)",
                  path != NULL ? path : "(none)");
        else
            reply(client, "ok delete-capture result=0x%08x %s", (uint32_t)capture_delete(path), path);
        return 0;
    }
    if (strcmp(command, "kernel-log") == 0)
    {
        // bring-up: dump the kernel log for a while, so another firmware's device
        // and controller lines can be read from the host (the format the device
        // id and the controller table are recovered from differs by firmware).
        const char *const ms = strtok(NULL, " \t");
        int wait_ms = ms != NULL ? atoi(ms) : 1500;
        if (wait_ms < 1 || wait_ms > 15000)
            wait_ms = 1500;
        const int log = klog_open();
        if (log < 0)
        {
            reply(client, "err klog: %s", strerror(errno));
            return 0;
        }
        const size_t cap = 1u << 20;
        char *const buffer = malloc(cap);
        size_t used = 0;
        const uint64_t deadline = monotonic_us() + (uint64_t)wait_ms * 1000u;
        while (buffer != NULL && used + 1 < cap && monotonic_us() < deadline)
        {
            const ssize_t bytes = klog_read(log, buffer + used, cap - 1 - used, 100);
            if (bytes < 0)
                break;
            used += (size_t)bytes;
        }
        close(log);
        reply(client, "ok file size=%zu mtime=0 path=klog", used);
        for (size_t sent = 0; buffer != NULL && sent < used;)
        {
            const ssize_t step = out(client, buffer + sent, used - sent, 0);
            if (step <= 0)
                break;
            sent += (size_t)step;
        }
        free(buffer);
        return 0;
    }
    if (strcmp(command, "ps") == 0)
    {
        list_processes(client);
        return 0;
    }
    if (strcmp(command, "ls") == 0)
    {
        const char *const path = strtok(NULL, " \t");
        if (path == NULL || path[0] != '/')
            reply(client, "err usage: ls /PATH");
        else
            list_dir(client, path);
        return 0;
    }
    if (strcmp(command, "get") == 0)
    {
        const char *const path = strtok(NULL, " \t");
        if (path == NULL || path[0] != '/')
            reply(client, "err usage: get /PATH");
        else
            get_file(client, path);
        return 0;
    }
    if (strcmp(command, "put") == 0)
    {
        const char *const path = strtok(NULL, " \t");
        const char *const size = strtok(NULL, " \t");
        if (path == NULL || size == NULL || path[0] != '/')
            reply(client, "err usage: put /PATH SIZE (then SIZE bytes)");
        else
            put_file(client, path, strtoull(size, NULL, 10), body, body_len);
        return 0;
    }
    if (strcmp(command, "rm") == 0)
    {
        const char *const path = strtok(NULL, " \t");
        if (path == NULL || path[0] != '/')
            reply(client, "err usage: rm /PATH");
        else
            remove_path(client, path);
        return 0;
    }
    if (strcmp(command, "mv") == 0)
    {
        const char *const from = strtok(NULL, " \t");
        const char *const to = strtok(NULL, " \t");
        if (from == NULL || to == NULL || from[0] != '/' || to[0] != '/')
            reply(client, "err usage: mv /FROM /TO");
        else
            move_path(client, from, to);
        return 0;
    }
    if (strcmp(command, "close-app") == 0)
    {
        close_app(client, strtok(NULL, " \t"));
        return 0;
    }
    if (strcmp(command, "locate-title") == 0)
    {
        locate_title(client);
        return 0;
    }
    if (strcmp(command, "hwinfo") == 0)
    {
        hwinfo(client);
        return 0;
    }
    if (strcmp(command, "df") == 0)
    {
        df(client);
        return 0;
    }
    if (strcmp(command, "kill") == 0)
    {
        const char *const pid = strtok(NULL, " \t");
        if (pid == NULL)
            reply(client, "err usage: kill PID");
        else
            kill_pid(client, atoi(pid));
        return 0;
    }
    if (strcmp(command, "klog-stream") == 0)
    {
        const char *const ms = strtok(NULL, " \t");
        long cap_ms = ms != NULL ? atol(ms) : 0;
        if (cap_ms < 0 || cap_ms > 600000)
            cap_ms = 600000;
        const int log = klog_open();
        if (log < 0)
        {
            reply(client, "err klog: %s", strerror(errno));
            return 0;
        }
        reply(client, "ok klog-stream (until you disconnect)");
        char buffer[8192];
        const uint64_t deadline = cap_ms != 0 ? monotonic_us() + (uint64_t)cap_ms * 1000u : 0;
        for (;;)
        {
            const ssize_t bytes = klog_read(log, buffer, sizeof(buffer), 200);
            if (bytes < 0)
                break;
            if (bytes > 0 && send(client, buffer, (size_t)bytes, 0) <= 0)
                break;
            if (deadline != 0 && monotonic_us() > deadline)
                break;
        }
        close(log);
        return 0;
    }
    if (strcmp(command, "pad") != 0)
    {
        reply(client, "err unknown %s", command);
        return 0;
    }

    // "pad [N] ACTION ...": N (1-4) picks the pad; without it, pad 1.
    const char *action = strtok(NULL, " \t");
    int n = 0;
    if (action != NULL && action[0] >= '1' && action[0] <= '0' + PS5LOOP_PADS && action[1] == '\0')
    {
        n = action[0] - '1';
        action = strtok(NULL, " \t");
    }
    pad_slot_t *const pad = &g_pad.slot[n];
    const char *arguments[7] = {0};
    int count = 0;
    while (count < 7 && (arguments[count] = strtok(NULL, " \t")) != NULL)
        count++;
    if (action == NULL)
    {
        reply(client, "err usage: pad [1-4] open|close|status|set|tap|handle|bind|evict, or pad list");
        return 0;
    }
    if (strcmp(action, "list") == 0)
    {
        char line[PS5LOOP_LINE - 64];
        size_t used = 0;
        for (int k = 0; k < PS5LOOP_PADS; k++)
        {
            const pad_slot_t *const slot = &g_pad.slot[k];
            const int written =
                slot->open ? snprintf(line + used, sizeof(line) - used,
                                      " %d:open,device=0x%llx,user=0x%08x,last=0x%08x", k + 1,
                                      (unsigned long long)slot->device_id, (uint32_t)slot->user_id,
                                      (uint32_t)slot->last_insert)
                           : snprintf(line + used, sizeof(line) - used, " %d:closed", k + 1);
            if (written > 0 && used + (size_t)written < sizeof(line))
                used += (size_t)written;
        }
        line[used] = '\0';
        reply(client, "ok pads open=%d%s", pads_open(), line);
        return 0;
    }
    if (strcmp(action, "open") == 0)
    {
        // "pad [N] open [USER] [keep]": keep leaves real controllers connected.
        // Pads 2-4 always keep them: only pad 1 needs to be its user's first.
        const bool keep = n > 0 || (count > 0 && strcmp(arguments[0], "keep") == 0) ||
                          (count > 1 && strcmp(arguments[1], "keep") == 0);
        const int32_t user =
            (count > 0 && strcmp(arguments[0], "keep") != 0) ? (int32_t)strtoul(arguments[0], NULL, 0) : -1;
        const int status = pad_open(n, user, !keep, report, sizeof(report));
        reply(client, "%s pad %s%s", status == 0 ? "ok" : "err", status == 0 ? "open " : "", report);
        return 0;
    }
    if (strcmp(action, "close") == 0)
    {
        if (count > 0 && strcmp(arguments[0], "all") == 0)
            pad_close_all(report, sizeof(report));
        else
            pad_close(n, report, sizeof(report));
        reply(client, "ok pad closed %s", report);
        return 0;
    }
    if (strcmp(action, "status") == 0)
    {
        reply(client, "ok pad %d %s handle=0x%x device=0x%llx user=0x%08x inserts=%u last=0x%08x", n + 1,
              pad->open ? "open" : "closed", (uint32_t)pad->handle,
              (unsigned long long)pad->device_id, (uint32_t)pad->user_id, pad->inserts,
              (uint32_t)pad->last_insert);
        return 0;
    }
    if (strcmp(action, "bind") == 0 && count >= 1)
    {
        if (pad_libraries(report, sizeof(report)) != 0 || mbus_bind == NULL)
        {
            reply(client, "err pad bind %s", report);
            return 0;
        }
        const uint64_t device_id = strtoull(arguments[0], NULL, 16);
        int user = count > 1 ? (int)strtoul(arguments[1], NULL, 0) : -1;
        if (user <= 0)
            (void)sceUserServiceGetForegroundUser(&user);
        reply(client, "ok pad bind device=0x%llx user=0x%08x result=0x%08x",
              (unsigned long long)device_id, (uint32_t)user, (uint32_t)mbus_bind(device_id, user));
        return 0;
    }
    if (strcmp(action, "evict") == 0 && count >= 1)
    {
        // A title reads its user's first controller. While a real one holds
        // that place the virtual pad is the second, which no title opens:
        // disconnecting the real one (PS on it brings it back) leaves ours first.
        const uint64_t device_id = strtoull(arguments[0], NULL, 16);
        bool ours = false;
        for (int k = 0; k < PS5LOOP_PADS; k++)
            ours = ours || (g_pad.slot[k].open && g_pad.slot[k].device_id == device_id);
        if (pad_libraries(report, sizeof(report)) != 0 || mbus_disconnect == NULL)
            reply(client, "err pad evict %s", report);
        else if (device_id == 0 || ours)
            reply(client, "err pad evict: name another device's id, not a virtual pad's");
        else
            reply(client, "ok pad evict device=0x%llx result=0x%08x",
                  (unsigned long long)device_id, (uint32_t)mbus_disconnect(device_id));
        return 0;
    }
    if (strcmp(action, "handle") == 0 && count >= 1)
    {
        if (pad_libraries(report, sizeof(report)) != 0)
        {
            reply(client, "err pad handle %s", report);
            return 0;
        }
        pthread_mutex_lock(&g_pad.lock);
        pad_neutral(&pad->state);
        pad->handle = (int32_t)strtoul(arguments[0], NULL, 0);
        pad->open = true;
        const int32_t result = pad_insert(pad->handle, &pad->state);
        pthread_mutex_unlock(&g_pad.lock);
        reply(client, "ok pad %d handle=0x%x insert=0x%08x", n + 1, (uint32_t)pad->handle,
              (uint32_t)result);
        return 0;
    }
    if (!pad->open)
    {
        reply(client, "err pad %d %s: that pad is not open", n + 1, action);
        return 0;
    }
    if (strcmp(action, "set") == 0 && count == 7)
    {
        int axes[6];
        for (int index = 0; index < 6; index++)
        {
            axes[index] = atoi(arguments[index + 1]);
            if (axes[index] < 0 || axes[index] > 255)
            {
                reply(client, "err pad set: sticks and triggers are 0 to 255");
                return 0;
            }
        }
        pad_hold(n, (uint32_t)strtoul(arguments[0], NULL, 16), axes[0], axes[1], axes[2], axes[3],
                 axes[4], axes[5]);
        reply(client, "ok pad set");
        return 0;
    }
    if (strcmp(action, "tap") == 0 && count == 2)
    {
        const int hold_ms = atoi(arguments[1]);
        if (hold_ms <= 0 || hold_ms > 10000)
        {
            reply(client, "err pad tap: hold 1 to 10000 ms");
            return 0;
        }
        pad_tap(n, (uint32_t)strtoul(arguments[0], NULL, 16), hold_ms);
        reply(client, "ok pad tap last=0x%08x", (uint32_t)pad->last_insert);
        return 0;
    }
    reply(client, "err usage: pad %s", action);
    return 0;
}

// Read one command line and answer it. Returns 0 to keep serving, -1 to leave.
// Bytes after the line's newline (an upload body, for "put") are handed on.
static int serve_client(int client)
{
    char buffer[1200] = {0};
    size_t used = 0;
    char *newline = NULL;
    while (used + 1 < sizeof(buffer))
    {
        const ssize_t bytes = recv(client, buffer + used, sizeof(buffer) - 1 - used, 0);
        if (bytes <= 0)
            break;
        used += (size_t)bytes;
        newline = memchr(buffer, '\n', used);
        if (newline != NULL)
            break;
    }
    char line[1024];
    size_t line_len = newline != NULL ? (size_t)(newline - buffer) : used;
    if (line_len >= sizeof(line))
        line_len = sizeof(line) - 1;
    memcpy(line, buffer, line_len);
    line[line_len] = '\0';
    while (line_len > 0 && (line[line_len - 1] == '\r'))
        line[--line_len] = '\0';
    const char *body = newline != NULL ? newline + 1 : buffer + used;
    const size_t body_len = newline != NULL ? (size_t)(buffer + used - (newline + 1)) : 0;
    if (line_len == 0 && body_len == 0)
        return 0;
    log_line("ps5loop: command %s", line);
    return handle_command(client, line, body, body_len);
}

// Open a bound, listening TCP socket on the command port, or -1. Used at start
// and to rebind after the network drops across rest/resume.
static int open_listener(unsigned port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 4) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[])
{
    unsigned port = PS5LOOP_PORT;
    if (argc > 1)
    {
        const unsigned long requested = strtoul(argv[1], NULL, 10);
        if (requested > 0 && requested < 65536)
            port = (unsigned)requested;
    }
    (void)signal(SIGPIPE, SIG_IGN);
    const int user_init = sceUserServiceInitialize(NULL);
    log_line("ps5loop: starting, port %u, pid %d, user service 0x%08x", port, (int)getpid(),
             (uint32_t)user_init);

    int server = open_listener(port);
    if (server < 0)
    {
        // A second copy finds the port taken and leaves the first one alone.
        log_line("ps5loop: bind or listen failed: %s", strerror(errno));
        return 1;
    }
    pthread_t inserter;
    if (pthread_create(&inserter, NULL, insert_thread, NULL) != 0)
    {
        log_line("ps5loop: no insert thread: %s", strerror(errno));
        close(server);
        return 1;
    }
    pthread_t discoverer;
    if (pthread_create(&discoverer, NULL, discover_thread, NULL) == 0)
        pthread_detach(discoverer);
    else
        log_line("ps5loop: no discovery beacon: %s", strerror(errno));
    log_line("ps5loop: listening");
    pthread_t announcer;
    if (pthread_create(&announcer, NULL, announce_thread, (void *)(uintptr_t)port) == 0)
        pthread_detach(announcer);
    for (;;)
    {
        const int client = accept(server, NULL, NULL);
        if (client < 0)
        {
            if (errno == EINTR)
                continue;
            // The network can drop across rest/resume; rebind and keep serving
            // instead of leaving, so the payload survives standby.
            log_line("ps5loop: accept failed: %s; rebinding", strerror(errno));
            close(server);
            server = -1;
            while (server < 0)
            {
                usleep(1000000);
                server = open_listener(port);
            }
            log_line("ps5loop: re-listening");
            continue;
        }
        const int leaving = serve_client(client);
        close(client);
        if (leaving != 0)
        {
            log_line("ps5loop: leaving");
            break;
        }
    }
    close(server);
    return 0;
}
