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
 * port 9112:
 *
 *   ping                      is the agent there, and what state the pad is in
 *   pad open [USERID] [keep]  create a virtual DualSense, give it to a user, and
 *                             (unless "keep") disconnect any real controller so
 *                             that the virtual pad is the one titles read
 *   pad close                 remove it
 *   pad status                handle, device id, user, last insert result
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
 *   direct-screenshot [TYPE]  bring-up: ask the screenshot service directly
 *                             (it answers no payload on firmware 12.20 or 13.60)
 *   frame REQUEST FOLDER [MS] ask a running title for its frame: create the
 *                             file REQUEST, which the title takes as "write a
 *                             picture into FOLDER", send the picture, delete it
 *   ps                        list processes (pid, app id, title id, command)
 *   ls PATH                   list a directory
 *   get PATH                  send a file's bytes
 *   put PATH SIZE             write the SIZE bytes that follow to a file
 *   rm PATH / mv FROM TO      delete, move or rename a file
 *   open-url URL              open a URL in the web browser
 *   launch TITLE_ID           launch a registered title
 *   close-app [APP_ID]        close a running app (default: the foreground one)
 *   install /PATH.pkg         install a .pkg already on the console
 *   exec /PATH [ARGS]         run a console binary, send back its output
 *   locate-title              the foreground app's ids (app id, pid, title id)
 *   module PID BASENAME       where a module is mapped in a process
 *   modules PID               every loaded module in a process (handle/base/entry)
 *   dlsym PID BASENAME SYM    resolve a symbol's address in a process
 *   call PID FUNC [ARGS]      call a function in a process (<=6 int args) -> RAX
 *   uninstall TITLE_ID        uninstall a title
 *   power off|reboot|rest     power off, reboot, or enter rest mode
 *   notify TYPE MESSAGE       show a message: TYPE is toast or flyout
 *   hwinfo                    model, serial, cpu + all SoC temps, cpu clock, fan %
 *   df                        free space per mounted volume
 *   led BWA|off|auto|breathe B W A [MS]|chase [MS]   the console indicator LED
 *   lightbar R G B            attempt the physical pad lightbar (shell owns it)
 *   fan [set TEMP]            read the fan duty, or set the threshold (30..90 C)
 *   fan hold TEMP [SECS]      re-apply the threshold every SECS s (default 7); fan hold off
 *   beeper [PATTERN]|mute 0|1|vol 0..2   beep, mute, or set the beeper volume
 *   GET / , GET /api/hwinfo   a browser gets the live dashboard, hosted here
 *   GET /api/run?c=CMD        the dashboard runs any command above; the reply
 *                             is served as text, or as the picture/file it sends
 *   kill PID                  end a process
 *   peek PID ADDR LEN         read a process's memory
 *   poke PID ADDR HEX         write a process's memory (read back and verified)
 *   scan PID ADDR LEN W VALUE find a W-byte integer in a span (W is 1|2|4|8)
 *   scan-aob PID ADDR LEN PAT find a byte pattern in a span (?? = wildcard)
 *   kern-read ADDR LEN        read kernel memory (absolute address)
 *   kern-write ADDR HEX       write kernel memory (read back and verified)
 *   klog-stream [MS]          stream the kernel log until you disconnect
 *   http http://URL           fetch a URL and send the response
 *   (a UDP probe to the command port is answered with an identifier: discovery)
 *   settings                  read the system registry as JSON
 *   setting-set REGID VALUE   write one integer setting (read back and verified)
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

/* The virtual pad. */
static struct
{
    pthread_mutex_t lock;
    bool libraries; /* libSceMbus loaded and the pad service initialised */
    bool open;
    int32_t handle;
    uint64_t device_id;
    int32_t user_id;
    int32_t last_insert;
    uint32_t inserts;
    pad_data_t state;
} g_pad = {.lock = PTHREAD_MUTEX_INITIALIZER, .handle = -1, .user_id = -1};

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
        if (g_pad.open)
        {
            g_pad.last_insert = pad_insert(g_pad.handle, &g_pad.state);
            g_pad.inserts++;
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

// Hold a state until the next one.
static void pad_hold(uint32_t buttons, int lx, int ly, int rx, int ry, int l2, int r2)
{
    pthread_mutex_lock(&g_pad.lock);
    pad_neutral(&g_pad.state);
    g_pad.state.buttons = buttons;
    g_pad.state.lx = (uint8_t)lx;
    g_pad.state.ly = (uint8_t)ly;
    g_pad.state.rx = (uint8_t)rx;
    g_pad.state.ry = (uint8_t)ry;
    // The console reads the triggers from their analog bytes: the bit alone
    // does not register.
    g_pad.state.l2 = (uint8_t)((buttons & PS5LOOP_BUTTON_L2) != 0 && l2 == 0 ? 255 : l2);
    g_pad.state.r2 = (uint8_t)((buttons & PS5LOOP_BUTTON_R2) != 0 && r2 == 0 ? 255 : r2);
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
            if (device != 0 && device != keep_device && device != 0xffffffffffffffffull)
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

// Create the virtual pad and give it to a user. Returns 0 when samples reach
// it; the report says what every step returned either way.
static int pad_open(int32_t user_id, bool evict, char *report, size_t size)
{
    if (g_pad.open)
    {
        note(report, size, "already open");
        return 0;
    }
    if (pad_libraries(report, size) != 0)
        return -1;
    if (user_id <= PS5LOOP_UNASSIGNED_USER && sceUserServiceGetForegroundUser(&user_id) != 0)
        user_id = -1;
    note(report, size, "user=0x%08x", (uint32_t)user_id);

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
        g_pad.device_id = device_id;
        return -1;
    }
    pthread_mutex_lock(&g_pad.lock);
    pad_neutral(&g_pad.state);
    g_pad.handle = handle;
    g_pad.device_id = device_id;
    g_pad.user_id = user_id;
    g_pad.inserts = 0;
    g_pad.open = true;
    pthread_mutex_unlock(&g_pad.lock);
    return 0;
}

static void pad_close(char *report, size_t size)
{
    pthread_mutex_lock(&g_pad.lock);
    const bool was_open = g_pad.open;
    const int32_t handle = g_pad.handle;
    g_pad.open = false;
    g_pad.handle = -1;
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

// Press and release, timed here and not across the network.
static void pad_tap(uint32_t buttons, int hold_ms)
{
    pad_hold(buttons, 128, 128, 128, 128, 0, 0);
    usleep((useconds_t)hold_ms * 1000u);
    pad_hold(0, 128, 128, 128, 128, 0, 0);
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

// Ask a running title for its frame. The request is a file the title removes
// when it has seen it; the answer is the next new picture in the folder, which
// is sent and then deleted, so that nothing of a run stays in the title's folder.
static void grab(int client, const char *request, const char *folder, int wait_ms)
{
    capture_t before[PS5LOOP_NEWEST];
    memset(before, 0, sizeof(before));
    captures_walk(folder, 6, before);
    const int file = open(request, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (file < 0)
    {
        reply(client, "err frame %s: %s", request, strerror(errno));
        return;
    }
    close(file);
    (void)chmod(request, 0666);
    off_t last_size = -1;
    const uint64_t deadline = monotonic_us() + (uint64_t)wait_ms * 1000u;
    while (monotonic_us() < deadline)
    {
        usleep(15000);
        capture_t after[PS5LOOP_NEWEST];
        memset(after, 0, sizeof(after));
        captures_walk(folder, 6, after);
        if (after[0].path[0] == '\0' || strcmp(after[0].path, before[0].path) == 0)
            continue;
        if (after[0].size > 0 && after[0].size == last_size)
        {
            send_file(client, &after[0]);
            (void)unlink(after[0].path);
            return;
        }
        last_size = after[0].size;
    }
    // A request nobody took must not wait for the next launch by hand.
    const bool taken = unlink(request) != 0;
    reply(client, "err frame no picture in %s within %d ms (the title %s the request)", folder,
          wait_ms, taken ? "took" : "never took");
}

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

// Ends a capture the service never finishes.
static int capture_expired(void *deadline)
{
    return monotonic_us() > *(const uint64_t *)deadline;
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

// Take the screen from the screenshot service, send the picture and delete it
// from the console. Returns false, having sent nothing, when the service gave
// no picture.
static bool direct_shot(int client, int type, int wait_ms, char *report, size_t size)
{
    capture_libraries(report, size);
    if (screenshot_capture == NULL)
        return false;
    char named[PS5LOOP_CAPTURE_PATH] = {0};
    uint64_t deadline = monotonic_us() + (uint64_t)wait_ms * 1000u;
    const int32_t result = screenshot_capture(type, named, capture_expired, &deadline);
    named[sizeof(named) - 1] = '\0';
    note(report, size, "capture=0x%08x named=%s", (uint32_t)result, named);
    log_line("ps5loop: sceScreenShotCapture -> 0x%08x %s", (uint32_t)result, named);
    if (result != 0 || named[0] != '/')
        return false;
    // The service names the file as the media database does, under /user.
    char path[PS5LOOP_CAPTURE_PATH + 8];
    struct stat status;
    snprintf(path, sizeof(path), "%s", named);
    if (stat(path, &status) != 0)
    {
        snprintf(path, sizeof(path), "/user%s", named);
        if (stat(path, &status) != 0)
        {
            note(report, size, "file=missing");
            return false;
        }
    }
    char *const bytes = malloc((size_t)status.st_size);
    const int file = open(path, O_RDONLY);
    off_t have = 0;
    while (bytes != NULL && file >= 0 && have < status.st_size)
    {
        const ssize_t step = read(file, bytes + have, (size_t)(status.st_size - have));
        if (step <= 0)
            break;
        have += step;
    }
    if (file >= 0)
        close(file);
    if (have != status.st_size)
    {
        free(bytes);
        note(report, size, "file=unreadable");
        return false;
    }
    const int32_t deleted = capture_delete(path);
    reply(client, "ok file size=%lld mtime=%lld deleted=0x%08x path=%s", (long long)have,
          (long long)status.st_mtime, (uint32_t)deleted, path);
    for (off_t sent = 0; sent < have;)
    {
        const ssize_t step = out(client, bytes + sent, (size_t)(have - sent), 0);
        if (step <= 0)
            break;
        sent += step;
    }
    free(bytes);
    return true;
}

// Take the console's own screenshot: hold Create as a person would, wait for a
// picture newer than the press whose size has stopped growing, send it, and
// delete it from the console. (Asking the screenshot service instead,
// "direct-screenshot", never answers a payload.)
static void take_shot(int client, int hold_ms)
{
    char report[PS5LOOP_LINE - 128] = {0};
    if (!g_pad.open)
    {
        reply(client,
              "err screenshot needs the pad: pad open first (latest-capture sends the newest one)");
        return;
    }
    capture_libraries(report, sizeof(report));
    capture_t before[PS5LOOP_NEWEST];
    captures_newest(before);
    pad_tap(PS5LOOP_BUTTON_CREATE, hold_ms);
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

// SHA-256 (FIPS 180-4), self-contained so the payload needs no crypto stub.
typedef struct { uint32_t state[8]; uint64_t bitlen; uint8_t buf[64]; size_t len; } sha256_ctx;

static uint32_t sha256_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++)
    {
        const uint32_t s0 = sha256_ror(w[i - 15], 7) ^ sha256_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = sha256_ror(w[i - 2], 17) ^ sha256_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for (int i = 0; i < 64; i++)
    {
        const uint32_t S1 = sha256_ror(e, 6) ^ sha256_ror(e, 11) ^ sha256_ror(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + k[i] + w[i];
        const uint32_t S0 = sha256_ror(a, 2) ^ sha256_ror(a, 13) ^ sha256_ror(a, 22);
        const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85; c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c; c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
    c->bitlen = 0; c->len = 0;
}

static void sha256_update(sha256_ctx *c, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        c->buf[c->len++] = data[i];
        if (c->len == 64) { sha256_block(c, c->buf); c->bitlen += 512; c->len = 0; }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    const uint64_t bits = c->bitlen + (uint64_t)c->len * 8;
    size_t i = c->len;
    c->buf[i++] = 0x80;
    if (i > 56) { while (i < 64) c->buf[i++] = 0; sha256_block(c, c->buf); i = 0; }
    while (i < 56) c->buf[i++] = 0;
    for (int j = 7; j >= 0; j--) c->buf[i++] = (uint8_t)(bits >> (j * 8));
    sha256_block(c, c->buf);
    for (int j = 0; j < 8; j++)
    {
        out[j * 4] = (uint8_t)(c->state[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(c->state[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(c->state[j] >> 8);
        out[j * 4 + 3] = (uint8_t)c->state[j];
    }
}

// hash: SHA-256 of a file as hex. Reads through the sandbox escape like `get`,
// so it can verify a transfer (compare with sha256sum on the host) without
// pulling the whole file back.
static void hash_file(int client, const char *path)
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
        reply(client, "err hash %s: %s", path, strerror(err));
        return;
    }
    sha256_ctx ctx;
    sha256_init(&ctx);
    char buffer[65536];
    ssize_t got;
    long long total = 0;
    while ((got = read(file, buffer, sizeof(buffer))) > 0)
    {
        sha256_update(&ctx, (const uint8_t *)buffer, (size_t)got);
        total += got;
    }
    const int read_err = got < 0 ? errno : 0;
    close(file);
    fs_restore(saved);
    if (read_err != 0)
    {
        reply(client, "err hash %s: %s", path, strerror(read_err));
        return;
    }
    uint8_t digest[32];
    sha256_final(&ctx, digest);
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    reply(client, "ok hash sha256=%s size=%lld path=%s", hex, total, path);
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

static void open_url(int client, const char *url)
{
    ensure_elevated();
    const int32_t result = sceSystemServiceLaunchWebBrowser(url, NULL);
    reply(client, "%s open-url result=0x%08x %s", result == 0 ? "ok" : "err", (uint32_t)result, url);
}

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

// setting-set: write one integer registry value by its regId and read it back.
// The caller names the regId (as with poke, a wrong one can misconfigure the
// console), so this is deliberately for a known key from a `settings` dump.
static void setting_set(int client, uint32_t reg_id, int value)
{
    ensure_elevated();
    const int rc = sceRegMgrSetInt(reg_id, value);
    if (rc != 0)
    {
        reply(client, "err setting-set 0x%08x: result=0x%08x", reg_id, (uint32_t)rc);
        return;
    }
    int check = 0;
    const bool verified = sceRegMgrGetInt(reg_id, &check) == 0 && check == value;
    reply(client, "ok setting-set 0x%08x=%d verify=%s", reg_id, value, verified ? "ok" : "mismatch");
}

static void settings_dump(int client)
{
    ensure_elevated();
    size_t cap = 1u << 16, used = 0;
    char *json = malloc(cap);
    json = text_append(json, &cap, &used, "{");
    bool first = true;
    for (uint32_t cat = 0; cat <= 0xFF && json != NULL; cat++)
        for (uint32_t sub = 0; sub <= 0xFF && json != NULL; sub++)
        {
            bool any = false;
            for (uint32_t entry = 0; entry < 0x20 && json != NULL; entry++)
            {
                const uint32_t reg_id = (cat << 24) | (sub << 16) | entry;
                int value = 0;
                char text[400];
                if (sceRegMgrGetInt(reg_id, &value) == 0)
                {
                    json = text_append(json, &cap, &used, "%s\n  \"0x%08x\": %d",
                                       first ? "" : ",", reg_id, value);
                    any = true;
                    first = false;
                }
                else if (sceRegMgrGetStr(reg_id, text, sizeof(text)) == 0 && text[0] != '\0')
                {
                    char safe[820];
                    json_escape(safe, sizeof(safe), text);
                    json = text_append(json, &cap, &used, "%s\n  \"0x%08x\": \"%s\"",
                                       first ? "" : ",", reg_id, safe);
                    any = true;
                    first = false;
                }
                else if (!any && entry >= 3)
                    break; /* this (cat, sub) namespace looks empty */
            }
        }
    if (json != NULL)
        json = text_append(json, &cap, &used, "\n}\n");
    if (json == NULL)
        reply(client, "err settings: out of memory");
    else
        send_bytes(client, "settings", json, used);
    free(json);
}

// Apps: launch a title by id, close a running app by its app id (from ps).
int sceSystemServiceLaunchApp(const char *title_id, const char *argv[], void *param);
int sceLncUtilKillApp(uint32_t app_id);
int sceSystemServiceGetAppIdOfRunningBigApp(void);

static void launch_app(int client, const char *title_id)
{
    ensure_elevated();
    const char *argv[] = {NULL};
    const int32_t result = sceSystemServiceLaunchApp(title_id, argv, NULL);
    if (result > 0)
        reply(client, "ok launch %s appId=0x%x", title_id, (uint32_t)result);
    else
        reply(client, "err launch %s result=0x%08x", title_id, (uint32_t)result);
}

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

// module / dlsym: where a named shared object is mapped in a process, and the
// address of a symbol in it. kernel_dynlib_* read another process's link map.
static void module_info(int client, pid_t pid, const char *basename)
{
    uint32_t handle = 0;
    if (kernel_dynlib_handle(pid, basename, &handle) != 0)
    {
        reply(client, "err module pid=%d %s: not found", (int)pid, basename);
        return;
    }
    reply(client, "ok module pid=%d %s handle=0x%x base=0x%lx entry=0x%lx", (int)pid, basename,
          handle, (long)kernel_dynlib_mapbase_addr(pid, handle),
          (long)kernel_dynlib_entry_addr(pid, handle));
}

static void dlsym_lookup(int client, pid_t pid, const char *basename, const char *symbol)
{
    uint32_t handle = 0;
    if (kernel_dynlib_handle(pid, basename, &handle) != 0)
    {
        reply(client, "err dlsym pid=%d %s: module not found", (int)pid, basename);
        return;
    }
    const intptr_t addr = kernel_dynlib_dlsym(pid, handle, symbol);
    if (addr == 0)
        reply(client, "err dlsym pid=%d %s %s: not found", (int)pid, basename, symbol);
    else
        reply(client, "ok dlsym pid=%d %s %s=0x%lx", (int)pid, basename, symbol, (long)addr);
}

// call: run a function in a process with up to six integer args and return RAX.
// ptrace thread-hijack: attach, save the registers, point RIP at the function
// with args in the SysV registers and a return address that faults so the ret
// traps back to us, continue, read RAX at the fault, restore, detach. ptrace is
// often restricted on PS5, so attach may simply fail -- a clean error, no crash.
static void call_in_process(int client, pid_t pid, intptr_t func, const uint64_t *args, int nargs)
{
    ensure_elevated();
    if (ptrace(PT_ATTACH, pid, NULL, 0) != 0)
    {
        reply(client, "err call: attach pid=%d: %s (ptrace may be restricted here)", (int)pid,
              strerror(errno));
        return;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    struct reg saved, regs;
    if (ptrace(PT_GETREGS, pid, (caddr_t)&saved, 0) != 0)
    {
        const int err = errno;
        ptrace(PT_DETACH, pid, NULL, 0);
        reply(client, "err call: getregs pid=%d: %s", (int)pid, strerror(err));
        return;
    }
    regs = saved;
    regs.r_rdi = nargs > 0 ? (__int64_t)args[0] : 0;
    regs.r_rsi = nargs > 1 ? (__int64_t)args[1] : 0;
    regs.r_rdx = nargs > 2 ? (__int64_t)args[2] : 0;
    regs.r_rcx = nargs > 3 ? (__int64_t)args[3] : 0;
    regs.r_r8 = nargs > 4 ? (__int64_t)args[4] : 0;
    regs.r_r9 = nargs > 5 ? (__int64_t)args[5] : 0;
    // A return address that faults: when the function returns, RIP becomes this
    // and the process stops with a signal we catch. Put it in scratch stack.
    const uint64_t sentinel = 0x4141414141414141ull;
    uint64_t sp = ((uint64_t)saved.r_rsp - 1024) & ~0xFull;
    sp -= 8;
    mdbg_copyin(pid, &sentinel, (intptr_t)sp, sizeof(sentinel));
    regs.r_rsp = (__int64_t)sp;
    regs.r_rip = (__int64_t)func;
    if (ptrace(PT_SETREGS, pid, (caddr_t)&regs, 0) != 0)
    {
        const int err = errno;
        ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0);
        ptrace(PT_DETACH, pid, NULL, 0);
        reply(client, "err call: setregs pid=%d: %s", (int)pid, strerror(err));
        return;
    }
    ptrace(PT_CONTINUE, pid, (caddr_t)1, 0);
    waitpid(pid, &status, 0);
    struct reg after;
    uint64_t result = 0;
    if (ptrace(PT_GETREGS, pid, (caddr_t)&after, 0) == 0)
        result = (uint64_t)after.r_rax;
    ptrace(PT_SETREGS, pid, (caddr_t)&saved, 0); // put the thread back
    ptrace(PT_DETACH, pid, NULL, 0);
    reply(client, "ok call pid=%d func=0x%lx rax=0x%llx", (int)pid, (long)func,
          (unsigned long long)result);
}

// modules: every loaded module in a process. There is no enumeration call, so
// walk the dynlib handle ids (they are small and sequential) and report the ones
// with a mapping. Names need a kernel link-map walk, so this lists base/entry;
// use `module PID NAME` for a named lookup.
static void module_list(int client, pid_t pid)
{
    size_t cap = 1u << 13, used = 0;
    char *text = malloc(cap);
    text = text_append(text, &cap, &used, "%-8s %-18s %s\n", "HANDLE", "BASE", "ENTRY");
    int found = 0;
    for (uint32_t handle = 0; text != NULL && handle < 256; handle++)
    {
        const intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
        if (base <= 0)
            continue;
        text = text_append(text, &cap, &used, "0x%-6x 0x%-16lx 0x%lx\n", handle, (long)base,
                           (long)kernel_dynlib_entry_addr(pid, handle));
        found++;
    }
    if (text == NULL)
        reply(client, "err modules: out of memory");
    else if (found == 0)
        reply(client, "err modules pid=%d: none (bad pid?)", (int)pid);
    else
        send_bytes(client, "modules", text, used);
    free(text);
}

// exec: run a binary and send back what it writes to stdout/stderr, then its
// exit code. posix_spawn runs through the sandbox escape, so an absolute console
// path resolves. A command DevLoop already has (ls, ps, ...) is usually better;
// this is for running homebrew tools that live on the console.
extern char **environ;

static void exec_command(int client, char *argline)
{
    ensure_elevated();
    char *argv[32];
    int argc = 0;
    for (char *tok = strtok(argline, " \t"); tok != NULL && argc < 31; tok = strtok(NULL, " \t"))
        argv[argc++] = tok;
    argv[argc] = NULL;
    if (argc == 0)
    {
        reply(client, "err usage: exec /PATH [ARGS]");
        return;
    }
    int pipefd[2];
    if (pipe(pipefd) != 0)
    {
        reply(client, "err exec: pipe %s", strerror(errno));
        return;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 2);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    pid_t child = 0;
    const intptr_t saved = fs_escape();
    const int rc = posix_spawn(&child, argv[0], &fa, NULL, argv, environ);
    fs_restore(saved);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (rc != 0)
    {
        close(pipefd[0]);
        reply(client, "err exec %s: %s", argv[0], strerror(rc));
        return;
    }
    size_t cap = 1u << 14, used = 0;
    char *out = malloc(cap);
    char buffer[4096];
    ssize_t got;
    while (out != NULL && (got = read(pipefd[0], buffer, sizeof(buffer))) > 0)
    {
        if (cap - used < (size_t)got + 1)
        {
            const size_t want = used + (size_t)got + 1;
            size_t grow = cap * 2 > want ? cap * 2 : want;
            char *const bigger = realloc(out, grow);
            if (bigger == NULL)
            {
                free(out);
                out = NULL;
                break;
            }
            out = bigger;
            cap = grow;
        }
        memcpy(out + used, buffer, (size_t)got);
        used += (size_t)got;
    }
    close(pipefd[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (out == NULL)
        reply(client, "err exec: out of memory");
    else
    {
        char header[64];
        snprintf(header, sizeof(header), "exec pid=%d rc=%d", (int)child,
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        send_bytes(client, header, out, used);
    }
    free(out);
}

// Power: off and reboot do not return when they take; standby is "rest". The
// reply goes out first, since the request may end the process before it would.
int sceSystemServiceRequestPowerOff(void);
int sceSystemServiceRequestReboot(void);
int sceSystemStateMgrEnterStandby(void);

static void power(int client, const char *what)
{
    ensure_elevated();
    reply(client, "ok power %s requested", what);
    if (strcmp(what, "off") == 0)
        sceSystemServiceRequestPowerOff();
    else if (strcmp(what, "reboot") == 0)
        sceSystemServiceRequestReboot();
    else
        sceSystemStateMgrEnterStandby();
}

// uninstall: remove an installed title through the app installer service.
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppUnInstall(const char *title_id);

static void uninstall_app(int client, const char *title_id)
{
    ensure_elevated();
    sceAppInstUtilInitialize();
    const int32_t result = sceAppInstUtilAppUnInstall(title_id);
    reply(client, "%s uninstall %s result=0x%08x", result == 0 ? "ok" : "err", title_id,
          (uint32_t)result);
}

// install: install a .pkg already on the console, through the app installer.
// Verification is limited without a package to try; the call returns its result
// and the installer's status word. The path is a console path (e.g. /data/x.pkg).
int sceAppInstUtilAppInstallPkg(const char *path, int *status);

static void install_pkg(int client, const char *path)
{
    ensure_elevated();
    sceAppInstUtilInitialize();
    int status = 0;
    const int32_t result = sceAppInstUtilAppInstallPkg(path, &status);
    reply(client, "%s install %s result=0x%08x status=0x%08x", result == 0 ? "ok" : "err", path,
          (uint32_t)result, (uint32_t)status);
}

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

static void notify_toast(int client, const char *message)
{
    notify_request_t request;
    memset(&request, 0, sizeof(request));
    snprintf(request.message, sizeof(request.message), "%s", message);
    const int32_t result = sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    reply(client, "%s notify toast result=0x%08x", result == 0 ? "ok" : "err", (uint32_t)result);
}

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

static void notify_flyout(int client, const char *message)
{
    char safe[320];
    json_escape(safe, sizeof(safe), message);
    char *const json = malloc(2048);
    if (json == NULL)
    {
        reply(client, "err notify: out of memory");
        return;
    }
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
    reply(client, "%s notify flyout result=0x%08x", result == 0 ? "ok" : "err", (uint32_t)result);
}

static void notify(int client, const char *type, const char *message)
{
    ensure_elevated();
    if (type == NULL || strcmp(type, "toast") == 0)
        notify_toast(client, message);
    else if (strcmp(type, "flyout") == 0)
        notify_flyout(client, message);
    else
        reply(client, "err usage: notify toast|flyout MESSAGE");
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
}

static void hwinfo(int client)
{
    hwinfo_t h;
    hwinfo_read(&h);
    reply(client,
          "ok fw=0x%08x model=%s serial=%s cpuC=%d socC=%d socMax=%d socMaxId=%d socN=%d cpuMHz=%ld "
          "fanPct=%d",
          h.fw, h.model, h.serial, h.cpuC, h.soc0, h.socMax, h.socMaxId, h.socN, h.cpuMHz, h.fanPct);
}

// The same reading as a JSON object, for the dashboard the console hosts. The
// caller frees the buffer; NULL on out of memory.
static char *hwinfo_json(void)
{
    hwinfo_t h;
    hwinfo_read(&h);
    char model[520], serial[520];
    json_escape(model, sizeof(model), h.model);
    json_escape(serial, sizeof(serial), h.serial);
    size_t cap = 1u << 12, used = 0;
    char *json = malloc(cap);
    json = text_append(json, &cap, &used,
                       "{\"ok\":true,\"fw\":\"0x%08x\",\"model\":\"%s\",\"serial\":\"%s\","
                       "\"cpuC\":%d,\"socC\":%d,\"socMax\":%d,\"socMaxId\":%d,\"socN\":%d,"
                       "\"cpuMHz\":%ld,\"fanPct\":%d,\"socSensors\":[",
                       h.fw, model, serial, h.cpuC, h.soc0, h.socMax, h.socMaxId, h.socN, h.cpuMHz,
                       h.fanPct);
    for (int i = 0; json != NULL && i < h.socN; i++)
        json = text_append(json, &cap, &used, "%s{\"id\":%d,\"c\":%d}", i ? "," : "", h.socIds[i],
                           h.socVals[i]);
    return text_append(json, &cap, &used, "]}");
}

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

static int icc_led_block(const uint8_t *st, uint8_t id)
{
    const int count = st[2] | (st[3] << 8);
    for (int i = 0; i < count && ICC_BLOCK_START + (i + 1) * ICC_BLOCK_SIZE <= ICC_LED_STATE_SIZE; i++)
    {
        const int at = ICC_BLOCK_START + i * ICC_BLOCK_SIZE;
        if (st[at] == id)
            return at;
    }
    return -1;
}

// Hold the given levels (0-255) on the three channels; -1 leaves one unchanged.
// Each touched channel is rewritten as one step, the encoding the ICC accepts.
static int icc_led_set(int blue, int white, int amber)
{
    const int fd = open(ICC_INDICATOR_DEV, O_RDWR);
    if (fd < 0)
        return -errno;
    uint8_t st[ICC_LED_STATE_SIZE];
    if (ioctl(fd, ICC_LED_GET, st) != 0)
    {
        const int err = errno;
        close(fd);
        return -err;
    }
    const uint8_t ids[3] = {0x01, 0x11, 0x21};
    const int lv[3] = {blue, white, amber};
    int patched = 0;
    for (int i = 0; i < 3; i++)
    {
        if (lv[i] < 0)
            continue;
        const int at = icc_led_block(st, ids[i]);
        if (at < 0)
            continue;
        memset(st + at, 0, ICC_BLOCK_SIZE);
        const uint8_t prog[7] = {ids[i], 1, 0x02, (uint8_t)lv[i], 0x05, 0x02, 0x00};
        memcpy(st + at, prog, sizeof(prog));
        patched = 1;
    }
    int rc = 0;
    if (!patched)
        rc = -ENODEV;
    else if (ioctl(fd, ICC_LED_SET, st) != 0)
        rc = -errno;
    close(fd);
    return rc;
}

// The dynamic-LED ioctl holds only static levels, so breathe and chase are
// animated in software by a thread, which also re-asserts the colour because
// the shell periodically reclaims the LED. mode 0 = auto (we stop touching it,
// the system takes it back), 1 = static hold, 2 = breathe, 3 = chase.
static pthread_mutex_t g_led_lock = PTHREAD_MUTEX_INITIALIZER;
static struct
{
    int mode, b, w, a, period_ms;
} g_led = {0, 0, 0, 0, 4000};

static void *led_fx_thread(void *argument)
{
    (void)argument;
    for (;;)
    {
        pthread_mutex_lock(&g_led_lock);
        const int mode = g_led.mode, b = g_led.b, w = g_led.w, a = g_led.a;
        const int period = g_led.period_ms > 0 ? g_led.period_ms : 4000;
        pthread_mutex_unlock(&g_led_lock);
        if (mode == 0)
        {
            usleep(250000);
            continue;
        }
        if (mode == 1)
        {
            (void)icc_led_set(b, w, a);
            usleep(1000000); // a held colour, re-asserted each second
        }
        else if (mode == 2)
        {
            const double t = (double)(monotonic_us() / 1000u % (uint64_t)period);
            const double half = period / 2.0;
            const double frac = t < half ? t / half : (period - t) / half;
            (void)icc_led_set((int)(b * frac), (int)(w * frac), (int)(a * frac));
            usleep(40000);
        }
        else
        {
            const int step = period / 3 > 0 ? period / 3 : 1;
            const uint64_t phase = (monotonic_us() / 1000u / (uint64_t)step) % 3;
            (void)icc_led_set(phase == 0 ? 255 : 0, phase == 1 ? 255 : 0, phase == 2 ? 255 : 0);
            usleep(40000);
        }
    }
    return NULL;
}

static void led_set_effect(int client, int mode, int b, int w, int a, int period, const char *name)
{
    ensure_elevated();
    if (mode != 0)
    {
        const int rc = icc_led_set(b, w, a); // probe the device and show a first frame
        if (rc != 0)
        {
            reply(client, "err led: %s", strerror(-rc));
            return;
        }
    }
    pthread_mutex_lock(&g_led_lock);
    g_led.mode = mode;
    g_led.b = b;
    g_led.w = w;
    g_led.a = a;
    if (period > 0)
        g_led.period_ms = period;
    pthread_mutex_unlock(&g_led_lock);
    if (mode == 0)
        reply(client, "ok led auto (the system controls the LED)");
    else
        reply(client, "ok led %s blue=%d white=%d amber=%d", name, b, w, a);
}

// Fan: /dev/icc_fan. One ioctl sets the manual temperature threshold (30-90 C,
// the layout etaHEN uses: temp at byte 5); another reads the manual duty, which
// reports fan speed rather than the threshold. Changing the threshold changes
// cooling, so it is clamped and only set when asked.
#define ICC_FAN_DEV "/dev/icc_fan"
#define ICC_FAN_SET_THRESHOLD 0xC01C8F07ul
#define ICC_FAN_GET_DUTY 0xC0068F06ul

static void fan_command(int client, const char *temp_text)
{
    ensure_elevated();
    const int fd = open(ICC_FAN_DEV, O_RDONLY);
    if (fd < 0)
    {
        reply(client, "err fan: %s", strerror(errno));
        return;
    }
    if (temp_text != NULL)
    {
        int temp = atoi(temp_text);
        if (temp < 30)
            temp = 30;
        if (temp > 90)
            temp = 90;
        unsigned char data[10] = {0, 0, 0, 0, 0, (unsigned char)temp, 0, 0, 0, 0};
        const int rc = ioctl(fd, ICC_FAN_SET_THRESHOLD, data);
        const int err = errno;
        close(fd);
        if (rc == 0)
            reply(client, "ok fan threshold=%dC", temp);
        else
            reply(client, "err fan set: %s", strerror(err));
        return;
    }
    unsigned char duty[6] = {0};
    const int rc = ioctl(fd, ICC_FAN_GET_DUTY, duty);
    const int err = errno;
    close(fd);
    if (rc != 0)
        reply(client, "err fan duty: %s", strerror(err));
    else
        reply(client, "ok fan duty=%u,%u,%u,%u,%u,%u", duty[0], duty[1], duty[2], duty[3], duty[4],
              duty[5]);
}

// Apply the manual threshold once (clamped). Returns the ioctl result, or -1 if
// the device would not open.
static int fan_set_threshold(int temp)
{
    if (temp < 30)
        temp = 30;
    if (temp > 90)
        temp = 90;
    const int fd = open(ICC_FAN_DEV, O_RDONLY);
    if (fd < 0)
        return -1;
    unsigned char data[10] = {0, 0, 0, 0, 0, (unsigned char)temp, 0, 0, 0, 0};
    const int rc = ioctl(fd, ICC_FAN_SET_THRESHOLD, data);
    close(fd);
    return rc;
}

// The system reverts a manual threshold (on game launch, XMB transitions...), so
// a one-shot `fan set` does not stick. `fan hold TEMP [SECS]` re-applies it every
// SECS seconds in the background (idea from owendswang/ps5-fan-control, which runs
// as a dedicated daemon; here it is one resident thread). `gen` is bumped to tell
// the current holder to exit, so a restart never leaves two threads running.
static struct
{
    volatile int gen;
    volatile int running;
    int temp;
    int interval;
} g_fan_hold;

static void *fan_hold_thread(void *arg)
{
    const int my_gen = (int)(intptr_t)arg;
    while (g_fan_hold.gen == my_gen)
    {
        fan_set_threshold(g_fan_hold.temp);
        for (int i = 0; i < g_fan_hold.interval && g_fan_hold.gen == my_gen; i++)
            sleep(1);
    }
    return NULL;
}

static void fan_hold_command(int client, const char *temp_text, const char *secs_text)
{
    if (temp_text == NULL)
    {
        if (g_fan_hold.running)
            reply(client, "ok fan hold on temp=%dC interval=%ds", g_fan_hold.temp,
                  g_fan_hold.interval);
        else
            reply(client, "ok fan hold off");
        return;
    }
    if (strcmp(temp_text, "off") == 0 || strcmp(temp_text, "stop") == 0)
    {
        if (!g_fan_hold.running)
            reply(client, "ok fan hold already off");
        else
        {
            g_fan_hold.running = 0;
            g_fan_hold.gen++; // the holder thread sees the change and exits within 1 s
            reply(client, "ok fan hold off (threshold left at %dC)", g_fan_hold.temp);
        }
        return;
    }
    ensure_elevated();
    int temp = atoi(temp_text);
    if (temp < 30)
        temp = 30;
    if (temp > 90)
        temp = 90;
    int interval = secs_text != NULL ? atoi(secs_text) : 7;
    if (interval < 1)
        interval = 1;
    if (interval > 3600)
        interval = 3600;
    if (g_fan_hold.running)
    {
        g_fan_hold.temp = temp; // the running holder re-reads these each cycle
        g_fan_hold.interval = interval;
        reply(client, "ok fan hold updated temp=%dC interval=%ds", temp, interval);
        return;
    }
    if (fan_set_threshold(temp) != 0)
    {
        reply(client, "err fan hold: %s", strerror(errno));
        return;
    }
    g_fan_hold.temp = temp;
    g_fan_hold.interval = interval;
    const int my_gen = ++g_fan_hold.gen;
    pthread_t th;
    if (pthread_create(&th, NULL, fan_hold_thread, (void *)(intptr_t)my_gen) != 0)
    {
        reply(client, "err fan hold: cannot start thread");
        return;
    }
    pthread_detach(th);
    g_fan_hold.running = 1;
    reply(client, "ok fan hold temp=%dC interval=%ds", temp, interval);
}

// Beeper: the sceKernelIccSetBuzzer* calls are not stubs in this SDK, so resolve
// them from a loaded libkernel at run time (PS5-Suite resolves them the same
// way). `beep` sounds a pattern (once); `mute` and `vol` are persistent.
static int (*icc_buzzer_fn(const char *symbol))(int)
{
    const char *const libs[] = {"libkernel_sys.sprx", "libkernel.sprx", "libkernel_web.sprx"};
    for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); i++)
    {
        void *const lib = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
        if (lib != NULL)
        {
            int (*fn)(int) = (int (*)(int))dlsym(lib, symbol);
            if (fn != NULL)
                return fn;
        }
    }
    return NULL;
}

static void beeper_command(int client, const char *action, int value)
{
    ensure_elevated();
    const char *symbol = "sceKernelIccSetBuzzer"; // pattern 0..3, one shot
    const char *what = "pattern";
    if (action != NULL && strcmp(action, "mute") == 0)
    {
        symbol = "sceKernelIccSetBuzzerOffSetting"; // 1 = off (persistent)
        what = "mute";
    }
    else if (action != NULL && strcmp(action, "vol") == 0)
    {
        symbol = "sceKernelIccSetBuzzerDimSetting"; // 0..2 volume (persistent)
        what = "vol";
    }
    int (*fn)(int) = icc_buzzer_fn(symbol);
    if (fn == NULL)
    {
        reply(client, "err beeper: %s unavailable", symbol);
        return;
    }
    const int rc = fn(value);
    reply(client, "%s beeper %s=%d result=0x%08x", rc == 0 ? "ok" : "err", what, value,
          (uint32_t)rc);
}

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

// peek: read a process's memory through the debugger.
static void peek(int client, pid_t pid, intptr_t addr, size_t len)
{
    if (len == 0 || len > (1u << 20))
    {
        reply(client, "err peek: length 1..1048576");
        return;
    }
    char *const buffer = malloc(len);
    if (buffer == NULL)
    {
        reply(client, "err peek: out of memory");
        return;
    }
    if (mdbg_copyout(pid, addr, buffer, len) != 0)
        reply(client, "err peek pid=%d addr=0x%lx: %s", (int)pid, (long)addr, strerror(errno));
    else
        send_bytes(client, "peek", buffer, len);
    free(buffer);
}

// poke: write a hex string into a process's memory.
static void poke(int client, pid_t pid, intptr_t addr, const char *hex)
{
    const size_t hexlen = strlen(hex);
    const size_t len = hexlen / 2;
    if (len == 0 || hexlen % 2 != 0 || len > (1u << 20))
    {
        reply(client, "err poke: give an even-length hex string");
        return;
    }
    char *const buffer = malloc(len);
    if (buffer == NULL)
    {
        reply(client, "err poke: out of memory");
        return;
    }
    for (size_t i = 0; i < len; i++)
    {
        const char byte[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        buffer[i] = (char)strtoul(byte, NULL, 16);
    }
    if (mdbg_copyin(pid, buffer, addr, len) != 0)
    {
        free(buffer);
        reply(client, "err poke pid=%d addr=0x%lx: %s", (int)pid, (long)addr, strerror(errno));
        return;
    }
    // truthful write: read it back and confirm the bytes took.
    char *const check = malloc(len);
    bool verified = false;
    if (check != NULL && mdbg_copyout(pid, addr, check, len) == 0)
        verified = memcmp(check, buffer, len) == 0;
    free(check);
    free(buffer);
    reply(client, "ok poke %zu bytes pid=%d addr=0x%lx verify=%s", len, (int)pid, (long)addr,
          verified ? "ok" : "mismatch");
}

// Read a span of a process's memory into a fresh buffer, in chunks the debugger
// accepts. Returns NULL on allocation failure or an unreadable span.
static char *mem_read_span(pid_t pid, intptr_t addr, size_t len)
{
    char *const buffer = malloc(len);
    if (buffer == NULL)
        return NULL;
    for (size_t done = 0; done < len;)
    {
        const size_t step = len - done < 0x10000 ? len - done : 0x10000;
        if (mdbg_copyout(pid, addr + (intptr_t)done, buffer + done, step) != 0)
        {
            free(buffer);
            return NULL;
        }
        done += step;
    }
    return buffer;
}

#define SCAN_MAX_SPAN (16u << 20) // 16 MB per scan, to bound memory and time
#define SCAN_MAX_HITS 4096        // report at most this many matches

// scan: find a little-endian integer of WIDTH bytes equal to VALUE in a span of
// a process's memory, stepping by WIDTH. Answers a text list of hex addresses.
static void mem_scan(int client, pid_t pid, intptr_t addr, size_t len, int width, uint64_t value)
{
    if (width != 1 && width != 2 && width != 4 && width != 8)
    {
        reply(client, "err scan: width is 1, 2, 4 or 8");
        return;
    }
    if (len == 0 || len > SCAN_MAX_SPAN)
    {
        reply(client, "err scan: length 1..%u", SCAN_MAX_SPAN);
        return;
    }
    char *const buffer = mem_read_span(pid, addr, len);
    if (buffer == NULL)
    {
        reply(client, "err scan pid=%d addr=0x%lx: %s", (int)pid, (long)addr, strerror(errno));
        return;
    }
    const uint64_t mask = width == 8 ? ~0ull : ((1ull << (width * 8)) - 1);
    size_t cap = 1u << 16, used = 0, hits = 0;
    char *text = malloc(cap);
    for (size_t off = 0; text != NULL && off + (size_t)width <= len; off += (size_t)width)
    {
        uint64_t here = 0;
        memcpy(&here, buffer + off, (size_t)width);
        if ((here & mask) != (value & mask))
            continue;
        if (hits < SCAN_MAX_HITS)
            text = text_append(text, &cap, &used, "0x%lx\n", (long)(addr + (intptr_t)off));
        hits++;
    }
    free(buffer);
    if (text == NULL)
        reply(client, "err scan: out of memory");
    else
    {
        char header[96];
        snprintf(header, sizeof(header), "scan hits=%zu shown=%zu", hits,
                 hits < SCAN_MAX_HITS ? hits : (size_t)SCAN_MAX_HITS);
        send_bytes(client, header, text, used);
    }
    free(text);
}

// scan-aob: find a byte pattern in a span. The pattern is hex byte pairs; "??"
// is a wildcard. Answers a text list of hex addresses.
static void mem_scan_aob(int client, pid_t pid, intptr_t addr, size_t len, const char *pattern)
{
    if (len == 0 || len > SCAN_MAX_SPAN)
    {
        reply(client, "err scan-aob: length 1..%u", SCAN_MAX_SPAN);
        return;
    }
    const size_t plen = strlen(pattern) / 2;
    if (plen == 0 || strlen(pattern) % 2 != 0 || plen > 256)
    {
        reply(client, "err scan-aob: pattern is 1..256 hex byte pairs (?? = wildcard)");
        return;
    }
    unsigned char bytes[256];
    unsigned char wild[256];
    for (size_t i = 0; i < plen; i++)
    {
        const char hi = pattern[i * 2], lo = pattern[i * 2 + 1];
        if (hi == '?' && lo == '?')
        {
            wild[i] = 1;
            bytes[i] = 0;
        }
        else
        {
            const char pair[3] = {hi, lo, '\0'};
            char *end = NULL;
            bytes[i] = (unsigned char)strtoul(pair, &end, 16);
            wild[i] = 0;
            if (end != pair + 2)
            {
                reply(client, "err scan-aob: bad hex at byte %zu", i);
                return;
            }
        }
    }
    char *const buffer = mem_read_span(pid, addr, len);
    if (buffer == NULL)
    {
        reply(client, "err scan-aob pid=%d addr=0x%lx: %s", (int)pid, (long)addr, strerror(errno));
        return;
    }
    size_t cap = 1u << 16, used = 0, hits = 0;
    char *text = malloc(cap);
    const unsigned char *const data = (const unsigned char *)buffer;
    for (size_t off = 0; text != NULL && off + plen <= len; off++)
    {
        size_t i = 0;
        for (; i < plen; i++)
            if (!wild[i] && data[off + i] != bytes[i])
                break;
        if (i != plen)
            continue;
        if (hits < SCAN_MAX_HITS)
            text = text_append(text, &cap, &used, "0x%lx\n", (long)(addr + (intptr_t)off));
        hits++;
    }
    free(buffer);
    if (text == NULL)
        reply(client, "err scan-aob: out of memory");
    else
    {
        char header[96];
        snprintf(header, sizeof(header), "scan-aob hits=%zu shown=%zu", hits,
                 hits < SCAN_MAX_HITS ? hits : (size_t)SCAN_MAX_HITS);
        send_bytes(client, header, text, used);
    }
    free(text);
}

// kern-read / kern-write: raw kernel memory, by absolute kernel address. The
// kernel R/W primitives come from the loader's exploit (ps5/kernel.h).
static void kern_read(int client, intptr_t kaddr, size_t len)
{
    if (len == 0 || len > (1u << 20))
    {
        reply(client, "err kern-read: length 1..1048576");
        return;
    }
    char *const buffer = malloc(len);
    if (buffer == NULL)
    {
        reply(client, "err kern-read: out of memory");
        return;
    }
    if (kernel_copyout(kaddr, buffer, len) != 0)
        reply(client, "err kern-read addr=0x%lx: %s", (long)kaddr, strerror(errno));
    else
        send_bytes(client, "kern-read", buffer, len);
    free(buffer);
}

static void kern_write(int client, intptr_t kaddr, const char *hex)
{
    const size_t hexlen = strlen(hex);
    const size_t len = hexlen / 2;
    if (len == 0 || hexlen % 2 != 0 || len > (1u << 20))
    {
        reply(client, "err kern-write: give an even-length hex string");
        return;
    }
    char *const buffer = malloc(len);
    if (buffer == NULL)
    {
        reply(client, "err kern-write: out of memory");
        return;
    }
    for (size_t i = 0; i < len; i++)
    {
        const char byte[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        buffer[i] = (char)strtoul(byte, NULL, 16);
    }
    if (kernel_copyin(buffer, kaddr, len) != 0)
    {
        free(buffer);
        reply(client, "err kern-write addr=0x%lx: %s", (long)kaddr, strerror(errno));
        return;
    }
    char *const check = malloc(len);
    bool verified = false;
    if (check != NULL && kernel_copyout(kaddr, check, len) == 0)
        verified = memcmp(check, buffer, len) == 0;
    free(check);
    free(buffer);
    reply(client, "ok kern-write %zu bytes addr=0x%lx verify=%s", len, (long)kaddr,
          verified ? "ok" : "mismatch");
}

// http: a plain HTTP/1.1 GET over a raw socket, for http:// URLs.
static void http_get(int client, const char *url)
{
    if (strncmp(url, "http://", 7) != 0)
    {
        reply(client, "err http: only http:// URLs (https needs TLS)");
        return;
    }
    char host[256];
    const char *const after = url + 7;
    const char *const slash = strchr(after, '/');
    const char *const path = slash != NULL ? slash : "/";
    size_t hostlen = slash != NULL ? (size_t)(slash - after) : strlen(after);
    if (hostlen >= sizeof(host))
        hostlen = sizeof(host) - 1;
    memcpy(host, after, hostlen);
    host[hostlen] = '\0';
    int port = 80;
    char *const colon = strchr(host, ':');
    if (colon != NULL)
    {
        *colon = '\0';
        port = atoi(colon + 1);
    }
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *info = NULL;
    if (getaddrinfo(host, port_text, &hints, &info) != 0 || info == NULL)
    {
        reply(client, "err http: cannot resolve %s", host);
        return;
    }
    const int sock = socket(info->ai_family, info->ai_socktype, 0);
    if (sock < 0 || connect(sock, info->ai_addr, info->ai_addrlen) != 0)
    {
        if (sock >= 0)
            close(sock);
        freeaddrinfo(info);
        reply(client, "err http: cannot connect to %s:%d", host, port);
        return;
    }
    freeaddrinfo(info);
    char request[1024];
    const int reqlen = snprintf(request, sizeof(request),
                                "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ps5loop\r\n"
                                "Connection: close\r\n\r\n",
                                path, host);
    (void)send(sock, request, (size_t)reqlen, 0);
    size_t cap = 1u << 16, used = 0;
    char *body = malloc(cap);
    while (body != NULL)
    {
        if (cap - used < 4096)
        {
            char *const bigger = realloc(body, cap * 2);
            if (bigger == NULL)
            {
                free(body);
                body = NULL;
                break;
            }
            body = bigger;
            cap *= 2;
        }
        const ssize_t got = recv(sock, body + used, cap - used, 0);
        if (got <= 0)
            break;
        used += (size_t)got;
        if (used > (8u << 20))
            break;
    }
    close(sock);
    if (body == NULL)
        reply(client, "err http: out of memory");
    else
        send_bytes(client, url, body, used);
    free(body);
}

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

// lightbar: attempt the *physical* DualSense lightbar. SceShellUI owns the pad
// session, so scePadOpen is expected to answer 0x809b0081 (already open) and
// there is no handle to set a colour on. This command exists to demonstrate that
// directly rather than only asserting it; if a firmware ever frees the pad, it
// also becomes a real control.
static void lightbar_try(int client, int r, int g, int b)
{
    const int init = scePadInit();
    int user = -1;
    (void)sceUserServiceGetForegroundUser(&user);
    const int handle = scePadOpen(user, 0 /* SCE_PAD_PORT_TYPE_STANDARD */, 0, NULL);
    if (handle < 0)
    {
        reply(client,
              "err lightbar: init=0x%08x scePadOpen user=0x%08x result=0x%08x — SceShellUI owns the pad",
              (uint32_t)init, (uint32_t)user, (uint32_t)handle);
        return;
    }
    pad_lightbar_t colour = {(uint8_t)r, (uint8_t)g, (uint8_t)b, 0};
    const int set = scePadSetLightBar(handle, &colour);
    scePadClose(handle);
    reply(client, "%s lightbar handle=0x%x set=0x%08x r=%d g=%d b=%d",
          set == 0 ? "ok" : "err", handle, (uint32_t)set, r, g, b);
}

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
        reply(client, "ok ps5loop 1 pid=%d fw=0x%08x pad=%s probe=0x%llx kproc=0x%lx", (int)getpid(),
              kernel_get_fw_version(), g_pad.open ? "open" : "closed",
              (unsigned long long)(uintptr_t)g_probe, (long)kernel_get_proc(getpid()));
        return 0;
    }
    if (strcmp(command, "quit") == 0)
    {
        pad_close(report, sizeof(report));
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
    if (strcmp(command, "direct-screenshot") == 0)
    {
        // bring-up: the screenshot service alone, with its type argument
        const char *const type = strtok(NULL, " \t");
        if (!direct_shot(client, type != NULL ? atoi(type) : 0, 5000, report, sizeof(report)))
            reply(client, "err capture %s", report);
        return 0;
    }
    if (strcmp(command, "frame") == 0)
    {
        const char *const request = strtok(NULL, " \t");
        const char *const folder = strtok(NULL, " \t");
        const char *const wait = strtok(NULL, " \t");
        const int wait_ms = wait != NULL ? atoi(wait) : 3000;
        if (request == NULL || folder == NULL || request[0] != '/' || folder[0] != '/')
            reply(client, "err usage: frame /REQUEST/FILE /PICTURE/FOLDER [MS]");
        else
            grab(client, request, folder, wait_ms > 0 && wait_ms <= 30000 ? wait_ms : 3000);
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
    if (strcmp(command, "hash") == 0)
    {
        const char *const path = strtok(NULL, " \t");
        if (path == NULL || path[0] != '/')
            reply(client, "err usage: hash /PATH");
        else
            hash_file(client, path);
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
    if (strcmp(command, "open-url") == 0)
    {
        const char *const url = strtok(NULL, " \t");
        if (url == NULL)
            reply(client, "err usage: open-url URL");
        else
            open_url(client, url);
        return 0;
    }
    if (strcmp(command, "launch") == 0)
    {
        const char *const title_id = strtok(NULL, " \t");
        if (title_id == NULL)
            reply(client, "err usage: launch TITLE_ID");
        else
            launch_app(client, title_id);
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
    if (strcmp(command, "module") == 0)
    {
        const char *const pid = strtok(NULL, " \t");
        const char *const name = strtok(NULL, " \t");
        if (pid == NULL || name == NULL)
            reply(client, "err usage: module PID BASENAME");
        else
            module_info(client, (pid_t)atoi(pid), name);
        return 0;
    }
    if (strcmp(command, "modules") == 0)
    {
        const char *const pid = strtok(NULL, " \t");
        if (pid == NULL)
            reply(client, "err usage: modules PID");
        else
            module_list(client, (pid_t)atoi(pid));
        return 0;
    }
    if (strcmp(command, "dlsym") == 0)
    {
        const char *const pid = strtok(NULL, " \t");
        const char *const name = strtok(NULL, " \t");
        const char *const symbol = strtok(NULL, " \t");
        if (pid == NULL || name == NULL || symbol == NULL)
            reply(client, "err usage: dlsym PID BASENAME SYMBOL");
        else
            dlsym_lookup(client, (pid_t)atoi(pid), name, symbol);
        return 0;
    }
    if (strcmp(command, "call") == 0)
    {
        const char *const pid = strtok(NULL, " \t");
        const char *const addr = strtok(NULL, " \t");
        if (pid == NULL || addr == NULL)
            reply(client, "err usage: call PID FUNC_ADDR [ARG0..ARG5]");
        else
        {
            uint64_t args[6] = {0};
            int nargs = 0;
            for (const char *tok = strtok(NULL, " \t"); tok != NULL && nargs < 6;
                 tok = strtok(NULL, " \t"))
                args[nargs++] = strtoull(tok, NULL, 0);
            call_in_process(client, (pid_t)atoi(pid), (intptr_t)strtoull(addr, NULL, 0), args, nargs);
        }
        return 0;
    }
    if (strcmp(command, "power") == 0)
    {
        const char *const what = strtok(NULL, " \t");
        if (what == NULL ||
            (strcmp(what, "off") != 0 && strcmp(what, "reboot") != 0 && strcmp(what, "rest") != 0))
            reply(client, "err usage: power off|reboot|rest");
        else
            power(client, what);
        return 0;
    }
    if (strcmp(command, "uninstall") == 0)
    {
        const char *const title_id = strtok(NULL, " \t");
        if (title_id == NULL)
            reply(client, "err usage: uninstall TITLE_ID");
        else
            uninstall_app(client, title_id);
        return 0;
    }
    if (strcmp(command, "install") == 0)
    {
        const char *const path = strtok(NULL, " \t");
        if (path == NULL || path[0] != '/')
            reply(client, "err usage: install /PATH/TO.pkg");
        else
            install_pkg(client, path);
        return 0;
    }
    if (strcmp(command, "exec") == 0)
    {
        char *const rest = strtok(NULL, "");
        if (rest == NULL)
            reply(client, "err usage: exec /PATH [ARGS]");
        else
            exec_command(client, rest);
        return 0;
    }
    if (strcmp(command, "notify") == 0)
    {
        const char *const type = strtok(NULL, " \t");
        const char *message = strtok(NULL, "");
        while (message != NULL && (*message == ' ' || *message == '\t'))
            message++;
        if (message == NULL || *message == '\0')
            reply(client, "err usage: notify toast|flyout MESSAGE");
        else
            notify(client, type, message);
        return 0;
    }
    if (strcmp(command, "hwinfo") == 0)
    {
        hwinfo(client);
        return 0;
    }
    if (strcmp(command, "setting-set") == 0)
    {
        const char *const reg = strtok(NULL, " \t");
        const char *const value = strtok(NULL, " \t");
        if (reg == NULL || value == NULL)
            reply(client, "err usage: setting-set REGID VALUE (REGID hex, from a settings dump)");
        else
            setting_set(client, (uint32_t)strtoul(reg, NULL, 0), atoi(value));
        return 0;
    }
    if (strcmp(command, "settings") == 0)
    {
        settings_dump(client);
        return 0;
    }
    if (strcmp(command, "df") == 0)
    {
        df(client);
        return 0;
    }
    if (strcmp(command, "lightbar") == 0)
    {
        const char *const r = strtok(NULL, " \t");
        const char *const g = strtok(NULL, " \t");
        const char *const b = strtok(NULL, " \t");
        if (r == NULL || g == NULL || b == NULL)
            reply(client, "err usage: lightbar R G B (attempts the physical pad)");
        else
            lightbar_try(client, atoi(r), atoi(g), atoi(b));
        return 0;
    }
    if (strcmp(command, "led") == 0)
    {
        const char *const a0 = strtok(NULL, " \t");
        if (a0 != NULL && strcmp(a0, "auto") == 0)
            led_set_effect(client, 0, 0, 0, 0, 0, "auto");
        else if (a0 != NULL && strcmp(a0, "off") == 0)
            led_set_effect(client, 1, 0, 0, 0, 0, "off");
        else if (a0 != NULL && (strcmp(a0, "breathe") == 0 || strcmp(a0, "chase") == 0))
        {
            const int chase = strcmp(a0, "chase") == 0;
            const char *const b = chase ? NULL : strtok(NULL, " \t");
            const char *const w = chase ? NULL : strtok(NULL, " \t");
            const char *const m = chase ? NULL : strtok(NULL, " \t");
            const char *const period = strtok(NULL, " \t");
            if (!chase && (b == NULL || w == NULL || m == NULL))
                reply(client, "err usage: led breathe BLUE WHITE AMBER [MS]");
            else
                led_set_effect(client, chase ? 3 : 2, chase ? 255 : atoi(b), chase ? 255 : atoi(w),
                               chase ? 255 : atoi(m), period != NULL ? atoi(period) : 0, a0);
        }
        else
        {
            const char *const b = strtok(NULL, " \t");
            const char *const c = strtok(NULL, " \t");
            if (a0 == NULL || b == NULL || c == NULL)
                reply(client,
                      "err usage: led BLUE WHITE AMBER | off | auto | breathe B W A [MS] | chase [MS]");
            else
                led_set_effect(client, 1, atoi(a0), atoi(b), atoi(c), 0, "static");
        }
        return 0;
    }
    if (strcmp(command, "fan") == 0)
    {
        const char *const sub = strtok(NULL, " \t");
        if (sub != NULL && strcmp(sub, "set") == 0)
            fan_command(client, strtok(NULL, " \t"));
        else if (sub != NULL && strcmp(sub, "hold") == 0)
        {
            const char *const temp = strtok(NULL, " \t");
            fan_hold_command(client, temp, temp != NULL ? strtok(NULL, " \t") : NULL);
        }
        else if (sub == NULL)
            fan_command(client, NULL);
        else
            reply(client,
                  "err usage: fan (read duty) | fan set TEMP (30..90) | "
                  "fan hold TEMP [SECS] | fan hold off");
        return 0;
    }
    if (strcmp(command, "beeper") == 0)
    {
        const char *const sub = strtok(NULL, " \t");
        if (sub != NULL && (strcmp(sub, "mute") == 0 || strcmp(sub, "vol") == 0))
        {
            const char *const value = strtok(NULL, " \t");
            if (value == NULL)
                reply(client, "err usage: beeper mute 0|1 | beeper vol 0..2 | beeper [PATTERN]");
            else
                beeper_command(client, sub, atoi(value));
        }
        else
            beeper_command(client, NULL, sub != NULL ? atoi(sub) : 1);
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
    if (strcmp(command, "peek") == 0)
    {
        const char *const pid = strtok(NULL, " \t");
        const char *const addr = strtok(NULL, " \t");
        const char *const len = strtok(NULL, " \t");
        if (pid == NULL || addr == NULL || len == NULL)
            reply(client, "err usage: peek PID ADDR LEN");
        else
            peek(client, (pid_t)atoi(pid), (intptr_t)strtoull(addr, NULL, 0),
                 (size_t)strtoull(len, NULL, 0));
        return 0;
    }
    if (strcmp(command, "poke") == 0)
    {
        const char *const pid = strtok(NULL, " \t");
        const char *const addr = strtok(NULL, " \t");
        const char *const hex = strtok(NULL, " \t");
        if (pid == NULL || addr == NULL || hex == NULL)
            reply(client, "err usage: poke PID ADDR HEX");
        else
            poke(client, (pid_t)atoi(pid), (intptr_t)strtoull(addr, NULL, 0), hex);
        return 0;
    }
    if (strcmp(command, "scan") == 0)
    {
        const char *const pid = strtok(NULL, " \t");
        const char *const addr = strtok(NULL, " \t");
        const char *const len = strtok(NULL, " \t");
        const char *const width = strtok(NULL, " \t");
        const char *const value = strtok(NULL, " \t");
        if (pid == NULL || addr == NULL || len == NULL || width == NULL || value == NULL)
            reply(client, "err usage: scan PID ADDR LEN WIDTH VALUE (width 1|2|4|8)");
        else
            mem_scan(client, (pid_t)atoi(pid), (intptr_t)strtoull(addr, NULL, 0),
                     (size_t)strtoull(len, NULL, 0), atoi(width), strtoull(value, NULL, 0));
        return 0;
    }
    if (strcmp(command, "scan-aob") == 0)
    {
        const char *const pid = strtok(NULL, " \t");
        const char *const addr = strtok(NULL, " \t");
        const char *const len = strtok(NULL, " \t");
        const char *const pattern = strtok(NULL, " \t");
        if (pid == NULL || addr == NULL || len == NULL || pattern == NULL)
            reply(client, "err usage: scan-aob PID ADDR LEN PATTERN (hex, ?? = wildcard)");
        else
            mem_scan_aob(client, (pid_t)atoi(pid), (intptr_t)strtoull(addr, NULL, 0),
                         (size_t)strtoull(len, NULL, 0), pattern);
        return 0;
    }
    if (strcmp(command, "kern-read") == 0)
    {
        const char *const addr = strtok(NULL, " \t");
        const char *const len = strtok(NULL, " \t");
        if (addr == NULL || len == NULL)
            reply(client, "err usage: kern-read ADDR LEN");
        else
            kern_read(client, (intptr_t)strtoull(addr, NULL, 0), (size_t)strtoull(len, NULL, 0));
        return 0;
    }
    if (strcmp(command, "kern-write") == 0)
    {
        const char *const addr = strtok(NULL, " \t");
        const char *const hex = strtok(NULL, " \t");
        if (addr == NULL || hex == NULL)
            reply(client, "err usage: kern-write ADDR HEX");
        else
            kern_write(client, (intptr_t)strtoull(addr, NULL, 0), hex);
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
    if (strcmp(command, "http") == 0)
    {
        const char *const url = strtok(NULL, " \t");
        if (url == NULL)
            reply(client, "err usage: http http://URL");
        else
            http_get(client, url);
        return 0;
    }
    if (strcmp(command, "pad") != 0)
    {
        reply(client, "err unknown %s", command);
        return 0;
    }

    const char *const action = strtok(NULL, " \t");
    const char *arguments[7] = {0};
    int count = 0;
    while (count < 7 && (arguments[count] = strtok(NULL, " \t")) != NULL)
        count++;
    if (action == NULL)
    {
        reply(client, "err usage: pad open|close|status|set|tap|handle|bind");
        return 0;
    }
    if (strcmp(action, "open") == 0)
    {
        // "pad open [USER] [keep]": keep leaves real controllers connected.
        const bool keep = (count > 0 && strcmp(arguments[0], "keep") == 0) ||
                          (count > 1 && strcmp(arguments[1], "keep") == 0);
        const int32_t user =
            (count > 0 && strcmp(arguments[0], "keep") != 0) ? (int32_t)strtoul(arguments[0], NULL, 0) : -1;
        const int status = pad_open(user, !keep, report, sizeof(report));
        reply(client, "%s pad %s%s", status == 0 ? "ok" : "err", status == 0 ? "open " : "", report);
        return 0;
    }
    if (strcmp(action, "close") == 0)
    {
        pad_close(report, sizeof(report));
        reply(client, "ok pad closed %s", report);
        return 0;
    }
    if (strcmp(action, "status") == 0)
    {
        reply(client, "ok pad %s handle=0x%x device=0x%llx user=0x%08x inserts=%u last=0x%08x",
              g_pad.open ? "open" : "closed", (uint32_t)g_pad.handle,
              (unsigned long long)g_pad.device_id, (uint32_t)g_pad.user_id, g_pad.inserts,
              (uint32_t)g_pad.last_insert);
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
        if (pad_libraries(report, sizeof(report)) != 0 || mbus_disconnect == NULL)
            reply(client, "err pad evict %s", report);
        else if (device_id == 0 || device_id == g_pad.device_id)
            reply(client, "err pad evict: name another device's id");
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
        pad_neutral(&g_pad.state);
        g_pad.handle = (int32_t)strtoul(arguments[0], NULL, 0);
        g_pad.open = true;
        const int32_t result = pad_insert(g_pad.handle, &g_pad.state);
        pthread_mutex_unlock(&g_pad.lock);
        reply(client, "ok pad handle=0x%x insert=0x%08x", (uint32_t)g_pad.handle, (uint32_t)result);
        return 0;
    }
    if (!g_pad.open)
    {
        reply(client, "err pad %s: the pad is not open", action);
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
        pad_hold((uint32_t)strtoul(arguments[0], NULL, 16), axes[0], axes[1], axes[2], axes[3],
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
        pad_tap((uint32_t)strtoul(arguments[0], NULL, 16), hold_ms);
        reply(client, "ok pad tap last=0x%08x", (uint32_t)g_pad.last_insert);
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
    pthread_t led_animator;
    if (pthread_create(&led_animator, NULL, led_fx_thread, NULL) == 0)
        pthread_detach(led_animator);
    else
        log_line("ps5loop: no led thread: %s", strerror(errno));
    log_line("ps5loop: listening");
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
