/*
 * bot.c - Telegram bot to control tmux panes and Terminal.app tabs
 *
 * Allows reading the screen content and sending keystrokes to tmux panes
 * (via the tmux command) and macOS Terminal.app tabs (via AppleScript)
 * from Telegram messages.
 *
 * Commands:
 *   .list    - List tmux panes and Terminal.app tabs
 *   .1 .2 .. - Connect to a terminal by number
 *   .stream  - Keep the last screen message updated as the terminal changes
 *   .stop    - Stop streaming
 *   .esc .ctrl_c .enter ... - Send a key
 *   .help    - Show help
 *
 * The Telegram menu offers the same commands as /_list, /_stream and so
 * forth. Plain "/" messages are typed in the terminal instead, since many
 * programs (like coding agents) have their own slash commands.
 *
 * Once connected, any text is sent as keystrokes (newline auto-added).
 * End with 💜 to suppress the automatic newline.
 * Emoji modifiers: ❤️ (Ctrl), 💙 (Alt), 💛 (ESC), 🧡 (Enter)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <spawn.h>
#include <pthread.h>
#include <sys/wait.h>
#include <time.h>

#include "botlib.h"
#include "sha1.h"
#include "qrcodegen.h"

extern char **environ;

/* ============================================================================
 * State
 * ========================================================================= */

#define MOD_CTRL    (1<<0)
#define MOD_ALT     (1<<1)

/* Kinds of terminals we can control. */
#define TARGET_TMUX 0                 /* A tmux pane. */
#define TARGET_TERMINAL 1             /* A macOS Terminal.app tab. */

/* Terminal information. */
typedef struct {
    int kind;           /* TARGET_TMUX or TARGET_TERMINAL. */
    char id[64];        /* tmux pane ID like "%5", or the tty of the
                         * Terminal.app tab like "/dev/ttys003". Both are
                         * stable for the life of the terminal. */
    char label[256];    /* Human readable description for .list. */
} Target;

/* Global state. */
static pthread_mutex_t RequestLock = PTHREAD_MUTEX_INITIALIZER;
static Target *TargetList = NULL;     /* Cached list for .list display. */
static int TargetCount = 0;           /* Number of terminals in list. */

/* TOTP authentication state. */
static int WeakSecurity = 0;          /* If 1, skip all OTP logic. */
static int Authenticated = 0;         /* Whether OTP has been verified. */
static time_t LastActivity = 0;       /* Last time owner sent a valid command. */
static int OtpTimeout = 300;          /* Timeout in seconds (default 5 min). */

/* Connected terminal. */
static int Connected = 0;             /* 1 if connected, 0 otherwise. */
static int ConnectedKind = 0;         /* TARGET_* of the connected terminal. */
static char ConnectedId[64];          /* ID of the connected terminal. */
static char ConnectedLabel[256];      /* Label for display. */

/* Last screen message sent. While streaming, it is edited as the terminal
 * content changes. */
#define STREAM_INTERVAL 2             /* Min seconds between stream edits. */
static int Streaming = 0;             /* 1 if streaming is active. */
static int64_t ScreenChat = 0;        /* Chat of the last screen message. */
static int64_t ScreenMsgId = 0;       /* ID of the last screen message. */
static sds ScreenLast = NULL;         /* Screen text it currently shows. */
static time_t ScreenTime = 0;         /* Time it was sent or last edited. */

/* ============================================================================
 * TOTP Authentication
 * ========================================================================= */

/* Encode raw bytes to Base32 string (RFC 4648). Returns static buffer. */
static const char *base32_encode(const unsigned char *data, size_t len) {
    static char out[128];
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    int i = 0, j = 0;
    uint64_t buf = 0;
    int bits = 0;

    for (i = 0; i < (int)len; i++) {
        buf = (buf << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out[j++] = alphabet[(buf >> bits) & 0x1f];
        }
    }
    if (bits > 0) {
        out[j++] = alphabet[(buf << (5 - bits)) & 0x1f];
    }
    out[j] = '\0';
    return out;
}

/* Compute 6-digit TOTP code from raw secret and time step. */
static uint32_t totp_code(const unsigned char *secret, size_t secret_len,
                          uint64_t time_step)
{
    unsigned char msg[8];
    for (int i = 7; i >= 0; i--) {
        msg[i] = (unsigned char)(time_step & 0xff);
        time_step >>= 8;
    }

    unsigned char hash[SHA1_DIGEST_SIZE];
    hmac_sha1(secret, secret_len, msg, 8, hash);

    int offset = hash[19] & 0x0f;
    uint32_t code = ((uint32_t)(hash[offset] & 0x7f) << 24)
                  | ((uint32_t)hash[offset+1] << 16)
                  | ((uint32_t)hash[offset+2] << 8)
                  | (uint32_t)hash[offset+3];
    return code % 1000000;
}

/* Print QR code as compact ASCII art using half-block characters.
 * Each output line encodes two QR rows using ▀ ▄ █ and space. */
static void print_qr_ascii(const char *text) {
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempbuf[qrcodegen_BUFFER_LEN_MAX];

    if (!qrcodegen_encodeText(text, tempbuf, qrcode,
            qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
            qrcodegen_Mask_AUTO, true)) {
        printf("Failed to generate QR code.\n");
        return;
    }

    int size = qrcodegen_getSize(qrcode);
    int lo = -1, hi = size + 1; /* 1-module quiet zone. */

    for (int y = lo; y < hi; y += 2) {
        for (int x = lo; x < hi; x++) {
            int top = (x >= 0 && x < size && y >= 0 && y < size &&
                       qrcodegen_getModule(qrcode, x, y));
            int bot = (x >= 0 && x < size && y+1 >= 0 && y+1 < size &&
                       qrcodegen_getModule(qrcode, x, y+1));
            if (top && bot)       printf("\xe2\x96\x88"); /* █ */
            else if (top && !bot) printf("\xe2\x96\x80"); /* ▀ */
            else if (!top && bot) printf("\xe2\x96\x84"); /* ▄ */
            else                  printf(" ");
        }
        printf("\n");
    }
}

/* Convert hex string to raw bytes. Returns number of bytes written. */
static int hex_to_bytes(const char *hex, unsigned char *out, int max) {
    int len = 0;
    while (*hex && *(hex+1) && len < max) {
        unsigned int byte;
        if (sscanf(hex, "%2x", &byte) != 1) break;
        out[len++] = (unsigned char)byte;
        hex += 2;
    }
    return len;
}

/* Convert raw bytes to hex string. Returns static buffer. */
static const char *bytes_to_hex(const unsigned char *data, int len) {
    static char hex[128];
    for (int i = 0; i < len && i < 63; i++) {
        sprintf(hex + i*2, "%02x", data[i]);
    }
    hex[len*2] = '\0';
    return hex;
}

/* Setup TOTP: check for existing secret, generate if needed, display QR.
 * The db_path is the SQLite database file path.
 * Returns the secret length in bytes, or 0 on error/weak-security. */
static int totp_setup(const char *db_path) {
    if (WeakSecurity) return 0;

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database for TOTP setup.\n");
        return 0;
    }
    /* Ensure KV table exists. */
    sqlite3_exec(db, TB_CREATE_KV_STORE, 0, 0, NULL);

    /* Check for existing secret. */
    sds existing = kvGet(db, "totp_secret");
    if (existing) {
        sdsfree(existing);
        /* Load stored timeout if present. */
        sds timeout_str = kvGet(db, "otp_timeout");
        if (timeout_str) {
            int t = atoi(timeout_str);
            if (t >= 30 && t <= 28800) OtpTimeout = t;
            sdsfree(timeout_str);
        }
        sqlite3_close(db);
        return 1; /* Secret already exists. */
    }

    /* Generate 20 random bytes. */
    unsigned char secret[20];
    FILE *f = fopen("/dev/urandom", "r");
    if (!f || fread(secret, 1, 20, f) != 20) {
        fprintf(stderr, "Failed to read /dev/urandom, aborting: "
                        "can't proceed without TOTP secret generation.\n");
        exit(1);
    }
    fclose(f);

    /* Store as hex in KV. */
    kvSet(db, "totp_secret", bytes_to_hex(secret, 20), 0);
    sqlite3_close(db);

    /* Build otpauth URI and display QR code. */
    const char *b32 = base32_encode(secret, 20);
    char uri[256];
    snprintf(uri, sizeof(uri),
             "otpauth://totp/tgterm?secret=%s&issuer=tgterm", b32);

    printf("\n=== TOTP Setup ===\n");
    printf("Scan this QR code with Google Authenticator:\n\n");
    print_qr_ascii(uri);
    printf("\nOr enter this secret manually: %s\n", b32);
    printf("==================\n\n");
    fflush(stdout);

    return 1;
}

/* Check if the given code matches the current TOTP (with ±1 window). */
static int totp_verify(sqlite3 *db, const char *code_str) {
    sds hex = kvGet(db, "totp_secret");
    if (!hex) return 0;

    unsigned char secret[20];
    int slen = hex_to_bytes(hex, secret, 20);
    sdsfree(hex);
    if (slen != 20) return 0;

    uint64_t now = (uint64_t)time(NULL) / 30;
    uint32_t input_code = (uint32_t)atoi(code_str);

    for (int i = -1; i <= 1; i++) {
        if (totp_code(secret, 20, now + i) == input_code)
            return 1;
    }
    return 0;
}

/* ============================================================================
 * UTF-8 Emoji Parsing
 * ========================================================================= */

/* Match red heart ❤️ (E2 9D A4, optionally followed by EF B8 8F). */
int match_red_heart(const unsigned char *p, size_t remaining) {
    if (remaining >= 3 && p[0] == 0xE2 && p[1] == 0x9D && p[2] == 0xA4) {
        if (remaining >= 6 && p[3] == 0xEF && p[4] == 0xB8 && p[5] == 0x8F)
            return 6;
        return 3;
    }
    return 0;
}

/* Match colored hearts 💙💛 (F0 9F 92 99/9B). */
int match_colored_heart(const unsigned char *p, size_t remaining, char *heart) {
    if (remaining >= 4 && p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0x92) {
        if (p[3] == 0x99) { *heart = 'B'; return 4; }  /* 💙 Blue = Alt */
        if (p[3] == 0x9B) { *heart = 'Y'; return 4; }  /* 💛 Yellow = ESC */
    }
    return 0;
}

/* Match orange heart 🧡 (F0 9F A7 A1) - sends Enter. */
int match_orange_heart(const unsigned char *p, size_t remaining) {
    if (remaining >= 4 && p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0xA7 && p[3] == 0xA1)
        return 4;
    return 0;
}

/* Match purple heart 💜 (F0 9F 92 9C) - used to suppress newline. */
int match_purple_heart(const unsigned char *p, size_t remaining) {
    if (remaining >= 4 && p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0x92 && p[3] == 0x9C)
        return 4;
    return 0;
}

/* Check if string ends with purple heart. */
int ends_with_purple_heart(const char *text) {
    size_t len = strlen(text);
    if (len >= 4) {
        const unsigned char *p = (const unsigned char *)text + len - 4;
        if (match_purple_heart(p, 4)) return 1;
    }
    return 0;
}

/* ============================================================================
 * Terminal Functions
 * ========================================================================= */

/* AppleScript programs used to control Terminal.app. The tabs are always
 * addressed by index and never via loop variables, since "contents of"
 * a variable dereferences the variable instead of reading the tab
 * contents. Note that "tab" is a Terminal.app class, so the field
 * separator is built with "character id 9". */

/* List every tab as: tty, window index, tab index, command, title.
 * The processes of a tab are in creation order: login, the shell, then
 * the job the shell is running and its children, so the third one is
 * the command shown, like the pane command reported by tmux. */
#define TERMINAL_LIST_SCRIPT \
    "if application \"Terminal\" is not running then return \"\"\n" \
    "set sep to character id 9\n" \
    "set out to \"\"\n" \
    "tell application \"Terminal\"\n" \
    "  set wi to 0\n" \
    "  repeat with w in windows\n" \
    "    set wi to wi + 1\n" \
    "    repeat with ti from 1 to count of tabs of w\n" \
    "      set procs to processes of tab ti of w\n" \
    "      set cmd to \"\"\n" \
    "      if (count of procs) >= 3 then\n" \
    "        set cmd to item 3 of procs\n" \
    "      else if (count of procs) > 0 then\n" \
    "        set cmd to last item of procs\n" \
    "      end if\n" \
    "      set out to out & (tty of tab ti of w) & sep & wi & sep & ti & " \
                          "sep & cmd & sep & (custom title of tab ti of w) & linefeed\n" \
    "    end repeat\n" \
    "  end repeat\n" \
    "end tell\n" \
    "return out\n"

/* Return the visible contents of the tab with the tty in argv[1]. */
#define TERMINAL_CONTENTS_SCRIPT \
    "on run argv\n" \
    "  tell application \"Terminal\"\n" \
    "    repeat with w in windows\n" \
    "      repeat with i from 1 to count of tabs of w\n" \
    "        if tty of tab i of w is item 1 of argv then return contents of tab i of w\n" \
    "      end repeat\n" \
    "    end repeat\n" \
    "  end tell\n" \
    "  error number 1\n" \
    "end run\n"

/* Type argv[2] in the tab with the tty in argv[1]. Terminal.app appends
 * a newline to the text. */
#define TERMINAL_TYPE_SCRIPT \
    "on run argv\n" \
    "  tell application \"Terminal\"\n" \
    "    repeat with w in windows\n" \
    "      repeat with i from 1 to count of tabs of w\n" \
    "        if tty of tab i of w is item 1 of argv then\n" \
    "          do script (item 2 of argv) in tab i of w\n" \
    "          return\n" \
    "        end if\n" \
    "      end repeat\n" \
    "    end repeat\n" \
    "  end tell\n" \
    "  error number 1\n" \
    "end run\n"

/* Run the program argv[0] with the given NULL terminated argument vector.
 * If 'output' is not NULL, the standard output of the program is returned
 * there as a new sds string. Returns the program exit code, or -1 if the
 * program could not be executed. */
static int run_command(char *const argv[], sds *output) {
    int fds[2];
    if (pipe(fds) == -1) return -1;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    pid_t pid;
    int err = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (err != 0) {
        close(fds[0]);
        return -1;
    }

    sds out = sdsempty();
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        out = sdscatlen(out, buf, n);
    close(fds[0]);

    int status;
    if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status)) {
        sdsfree(out);
        return -1;
    }
    if (output) *output = out; else sdsfree(out);
    return WEXITSTATUS(status);
}

/* Free the cached terminal list. */
static void free_target_list(void) {
    xfree(TargetList);
    TargetList = NULL;
    TargetCount = 0;
}

/* Append a terminal to the cached list. */
static void add_target(int kind, const char *id, const char *label) {
    TargetList = xrealloc(TargetList, (TargetCount + 1) * sizeof(Target));
    Target *t = &TargetList[TargetCount++];
    t->kind = kind;
    snprintf(t->id, sizeof(t->id), "%s", id);
    snprintf(t->label, sizeof(t->label), "%s", label);
}

/* Run 'argv' and call add_target() for every line of its output, which
 * must have 'numfields' tab separated fields, formatted by 'format'. */
static void list_targets(char *const argv[], int numfields, int kind,
                         void (*format)(sds *fields, char *label, size_t size))
{
    sds out;
    if (run_command(argv, &out) != 0) return;

    int count;
    sds *lines = sdssplitlen(out, sdslen(out), "\n", 1, &count);
    sdsfree(out);
    for (int i = 0; i < count; i++) {
        int nf;
        sds *f = sdssplitlen(lines[i], sdslen(lines[i]), "\t", 1, &nf);
        if (nf == numfields) {
            char label[256];
            format(f, label, sizeof(label));
            add_target(kind, f[0], label);
        }
        sdsfreesplitres(f, nf);
    }
    sdsfreesplitres(lines, count);
}

/* Label for a tmux pane: "session:window.pane name (command)", showing the
 * window name only if it adds information. */
static void tmux_label(sds *f, char *label, size_t size) {
    if (strcmp(f[2], f[3]) == 0)
        snprintf(label, size, "%s (%s)", f[1], f[3]);
    else
        snprintf(label, size, "%s %s (%s)", f[1], f[2], f[3]);
}

/* Label for a Terminal.app tab: "Terminal window.tab title (command)". */
static void terminal_label(sds *f, char *label, size_t size) {
    if (sdslen(f[4]) == 0)
        snprintf(label, size, "Terminal %s.%s (%s)", f[1], f[2], f[3]);
    else
        snprintf(label, size, "Terminal %s.%s %s (%s)", f[1], f[2], f[4], f[3]);
}

/* Refresh the list with the panes of all the tmux sessions, followed by
 * the tabs of all the Terminal.app windows. Returns the number of
 * terminals found. */
static int refresh_target_list(void) {
    free_target_list();

    char *tmux_argv[] = {"tmux", "list-panes", "-a", "-F",
        "#{pane_id}\t#{session_name}:#{window_index}.#{pane_index}\t"
        "#{window_name}\t#{pane_current_command}", NULL};
    list_targets(tmux_argv, 4, TARGET_TMUX, tmux_label);

    char *terminal_argv[] = {"osascript", "-e", TERMINAL_LIST_SCRIPT, NULL};
    list_targets(terminal_argv, 5, TARGET_TERMINAL, terminal_label);
    return TargetCount;
}

/* Return a copy of the screen without trailing spaces in every line and
 * without trailing empty lines. */
static sds trim_screen(const char *screen) {
    int count;
    sds *lines = sdssplitlen(screen, strlen(screen), "\n", 1, &count);
    int last = count;   /* Number of lines to keep. */
    for (int i = 0; i < count; i++) {
        size_t len = sdslen(lines[i]);
        while (len > 0 && lines[i][len-1] == ' ') len--;
        lines[i][len] = '\0';
        sdssetlen(lines[i], len);
        if (len > 0) last = i + 1;
    }
    if (last == count && count > 0 && sdslen(lines[count-1]) == 0) last = 0;

    sds out = sdsempty();
    for (int i = 0; i < last; i++) {
        if (i > 0) out = sdscatlen(out, "\n", 1);
        out = sdscatsds(out, lines[i]);
    }
    sdsfreesplitres(lines, count);
    return out;
}

/* Capture the visible content of the connected terminal. Returns NULL if
 * the terminal no longer exists. */
static sds capture_screen(void) {
    sds out;
    int err;
    if (ConnectedKind == TARGET_TMUX) {
        char *argv[] = {"tmux", "capture-pane", "-p", "-t", ConnectedId, NULL};
        err = run_command(argv, &out);
    } else {
        char *argv[] = {"osascript", "-e", TERMINAL_CONTENTS_SCRIPT, ConnectedId, NULL};
        err = run_command(argv, &out);
    }
    if (err != 0) return NULL;

    sds screen = trim_screen(out);
    sdsfree(out);
    return screen;
}

/* Convert a message into the bytes to type: heart modifiers and escape
 * sequences become control characters (Alt is the ESC prefix). Enter is
 * appended unless the message ends with 💜, is a single modified key or
 * bare ESC, or already ends with Enter. */
static sds keys_to_bytes(const char *text) {
    int add_newline = !ends_with_purple_heart(text);
    const unsigned char *p = (const unsigned char *)text;
    size_t len = strlen(text);

    /* If ends with purple heart, reduce length to skip it. */
    if (!add_newline) len -= 4;

    sds bytes = sdsempty();
    int mods = 0;
    int consumed;
    char heart;
    int keycount = 0;       /* Number of keystrokes. */
    int had_special = 0;    /* True if any keystroke was modified or ESC. */
    int last_was_nl = 0;    /* True if last keystroke was Enter. */

    while (len > 0) {
        if ((consumed = match_red_heart(p, len)) > 0) {
            mods |= MOD_CTRL;
            p += consumed; len -= consumed;
            continue;
        }

        if ((consumed = match_colored_heart(p, len, &heart)) > 0 && heart == 'B') {
            mods |= MOD_ALT;
            p += consumed; len -= consumed;
            continue;
        }

        /* What follows is a key: get the byte it produces. */
        unsigned char c;
        if (consumed > 0) {
            c = 0x1b; had_special = 1;      /* 💛 is ESC. */
        } else if ((consumed = match_orange_heart(p, len)) > 0) {
            c = '\r';
        } else if (*p == '\\' && len > 1 && p[1] == 'n') {
            c = '\r'; consumed = 2;
        } else if (*p == '\\' && len > 1 && p[1] == 't') {
            c = '\t'; consumed = 2;
        } else if (*p == '\\' && len > 1 && p[1] == '\\') {
            c = '\\'; consumed = 2;
        } else {
            c = *p; consumed = 1;
        }
        p += consumed; len -= consumed;

        if (mods & MOD_ALT) bytes = sdscatlen(bytes, "\x1b", 1);
        if (mods & MOD_CTRL) c &= 0x1f;
        bytes = sdscatlen(bytes, &c, 1);
        if (mods) had_special = 1;
        last_was_nl = (c == '\r');
        keycount++;
        mods = 0;
    }

    /* Add newline unless:
     * - Suppressed by purple heart
     * - Single modified keystroke (like Ctrl+C) or bare ESC
     * - Last explicit keystroke was already a newline */
    if (add_newline && !(keycount == 1 && had_special) && !last_was_nl)
        bytes = sdscatlen(bytes, "\r", 1);
    return bytes;
}

/* Type the given bytes in the connected terminal. Terminal.app always
 * adds a newline after the typed text, so the trailing Enter, if any, is
 * removed from 'bytes' to avoid sending it twice. */
static void type_bytes(sds bytes) {
    if (ConnectedKind == TARGET_TMUX) {
        char *argv[] = {"tmux", "send-keys", "-t", ConnectedId, "-l", "--", bytes, NULL};
        run_command(argv, NULL);
    } else {
        size_t len = sdslen(bytes);
        if (len > 0 && bytes[len-1] == '\r') {
            bytes[len-1] = '\0';
            sdssetlen(bytes, len-1);
        }
        char *argv[] = {"osascript", "-e", TERMINAL_TYPE_SCRIPT, ConnectedId, bytes, NULL};
        run_command(argv, NULL);
    }
}

/* Send the message text as keystrokes to the connected terminal. */
static void send_keys(const char *text) {
    sds bytes = keys_to_bytes(text);
    type_bytes(bytes);
    sdsfree(bytes);
}

/* ============================================================================
 * Commands
 * ========================================================================= */

/* The bot commands, in the order shown by .help and by the Telegram menu,
 * where they appear in the "/_" form. Commands with 'keys' set just send
 * those bytes to the terminal. Connecting with .N is not here, since
 * Telegram command names can only contain letters, digits and underscores. */
typedef struct {
    char *name;
    char *description;
    char *keys;
} Command;

static Command Commands[] = {
    {"list", "Show tmux panes and Terminal.app tabs", NULL},
    {"stream", "Keep the screen updated", NULL},
    {"stop", "Stop streaming", NULL},
    {"esc", "Send ESC", "\x1b"},
    {"ctrl_c", "Send Ctrl+C", "\x03"},
    {"enter", "Send Enter", "\r"},
    {"tab", "Send Tab", "\t"},
    {"shift_tab", "Send Shift+Tab", "\x1b[Z"},
    {"up", "Send arrow up", "\x1b[A"},
    {"down", "Send arrow down", "\x1b[B"},
    {"ctrl_d", "Send Ctrl+D", "\x04"},
    {"help", "Show the commands", NULL},
    {"otptimeout", "Set the OTP timeout in seconds (30-28800)", NULL},
};

#define NUM_COMMANDS (sizeof(Commands) / sizeof(Commands[0]))

/* If 'req' is a bot command, that is it starts with "." or with "/_"
 * (the form used by the Telegram menu), return the text after the
 * prefix. Otherwise return NULL. */
static const char *command_body(const char *req) {
    if (req[0] == '.') return req + 1;
    if (req[0] == '/' && req[1] == '_') return req + 2;
    return NULL;
}

/* If 'req' is the command 'name', return what follows the name: the
 * arguments, or an empty string. Otherwise return NULL. */
static const char *command_arg(const char *req, const char *name) {
    const char *body = command_body(req);
    if (!body) return NULL;
    size_t len = strlen(name);
    if (strncasecmp(body, name, len) != 0) return NULL;
    if (body[len] != '\0' && body[len] != ' ') return NULL;
    return body + len;
}

/* Return true if 'req' is the command 'name' without arguments. */
static int is_command(const char *req, const char *name) {
    const char *arg = command_arg(req, name);
    return arg && *arg == '\0';
}

/* Register the commands in the Telegram menu, in their "/_" form. */
static void set_menu(void) {
    char names[NUM_COMMANDS][64];
    char *menu[NUM_COMMANDS * 2];
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        snprintf(names[i], sizeof(names[i]), "_%s", Commands[i].name);
        menu[i*2] = names[i];
        menu[i*2+1] = Commands[i].description;
    }
    botSetMyCommands(menu, NUM_COMMANDS);
}

/* ============================================================================
 * Messages
 * ========================================================================= */

#define SCREEN_MAX_BYTES 4000   /* Telegram messages are limited to 4096 chars. */
#define REFRESH_BTN "🔄 Refresh"
#define REFRESH_DATA "refresh"
#define STOP_BTN "⏹ Stop"
#define STOP_DATA "stop"

/* Append 'text' to 'html', escaping the characters special in HTML. */
static sds html_escape(sds html, const char *text) {
    for (const char *p = text; *p; p++) {
        if (*p == '<') html = sdscat(html, "&lt;");
        else if (*p == '>') html = sdscat(html, "&gt;");
        else if (*p == '&') html = sdscat(html, "&amp;");
        else html = sdscatlen(html, p, 1);
    }
    return html;
}

/* Build the .list response (HTML). */
static sds build_list_message(void) {
    refresh_target_list();

    sds msg = sdsempty();
    if (TargetCount == 0) return sdscat(msg, "No tmux panes or Terminal.app tabs found.");

    msg = sdscat(msg, "Terminals:\n");
    for (int i = 0; i < TargetCount; i++) {
        msg = sdscatprintf(msg, ".%d ", i + 1);
        msg = html_escape(msg, TargetList[i].label);
        msg = sdscat(msg, "\n");
    }
    return msg;
}

/* Build the .help response (HTML). */
static sds build_help_message(void) {
    sds msg = sdsnew("Commands (from the menu as /_list, /_esc, ...):\n");
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        msg = sdscatprintf(msg, ".%s - ", Commands[i].name);
        msg = html_escape(msg, Commands[i].description);
        msg = sdscat(msg, "\n");
        if (strcmp(Commands[i].name, "list") == 0)
            msg = sdscat(msg, ".1 .2 ... - Connect to a terminal\n");
    }
    return sdscat(msg,
        "Any other /text is typed in the terminal.\n\n"
        "Once connected, text is sent as keystrokes.\n"
        "Newline is auto-added; end with <code>💜</code> to suppress it.\n\n"
        "Modifiers (tap to copy, then paste + key):\n"
        "<code>❤️</code> Ctrl  <code>💙</code> Alt  "
        "<code>💛</code> ESC  <code>🧡</code> Enter\n\n"
        "Escape sequences: \\n=Enter \\t=Tab");
}

/* Build the HTML message showing the screen content: the text is escaped
 * and enclosed in a <pre> block. If it is too large, the first lines are
 * dropped so that the bottom of the screen is preserved. */
static sds build_screen_message(const char *screen) {
    sds html = html_escape(sdsempty(), screen);

    if (sdslen(html) > SCREEN_MAX_BYTES) {
        /* Cut at the first line boundary that makes it fit, or at a
         * character boundary if a single line is too long. */
        char *start = html + sdslen(html) - SCREEN_MAX_BYTES;
        char *nl = strchr(start, '\n');
        if (nl) {
            start = nl + 1;
        } else {
            while ((*start & 0xC0) == 0x80) start++;
        }
        sdsrange(html, start - html, -1);
    }

    sds msg;
    if (sdslen(html) == 0) msg = sdsnew("(empty screen)");
    else msg = sdscatprintf(sdsempty(), "<pre>%s</pre>", html);
    sdsfree(html);
    return msg;
}

/* Text and callback data of the button under screen messages: while
 * streaming the button stops the stream, otherwise it refreshes. */
static const char *screen_btn_text(void) {
    return Streaming ? STOP_BTN : REFRESH_BTN;
}

static const char *screen_btn_data(void) {
    return Streaming ? STOP_DATA : REFRESH_DATA;
}

/* Remember the last screen message and the content it shows.
 * Takes ownership of 'screen'. */
static void set_screen_message(int64_t chat_id, int64_t msg_id, sds screen) {
    ScreenChat = chat_id;
    ScreenMsgId = msg_id;
    sdsfree(ScreenLast);
    ScreenLast = screen;
    ScreenTime = time(NULL);
}

/* Send the current screen as a new message. Returns 0 on success, -1 if
 * the terminal no longer exists. */
static int send_screen(int64_t chat_id) {
    sds screen = capture_screen();
    if (!screen) return -1;

    sds msg = build_screen_message(screen);
    int64_t msg_id = 0;
    int ok = botSendMessageHTML(chat_id, msg, screen_btn_text(),
                                screen_btn_data(), &msg_id);
    sdsfree(msg);
    if (ok) set_screen_message(chat_id, msg_id, screen);
    else sdsfree(screen);
    return 0;
}

/* Update an existing screen message with the current screen content.
 * Unless 'force' is true, the last screen message is not edited when
 * the content did not change since it was sent. Returns 0 on success,
 * -1 if the terminal no longer exists. */
static int refresh_screen(int64_t chat_id, int64_t msg_id, int force) {
    sds screen = capture_screen();
    if (!screen) return -1;

    if (!force && msg_id == ScreenMsgId && strcmp(screen, ScreenLast) == 0) {
        sdsfree(screen);
        return 0;
    }

    sds msg = build_screen_message(screen);
    botEditMessageHTML(chat_id, msg_id, msg, screen_btn_text(), screen_btn_data());
    sdsfree(msg);
    if (msg_id == ScreenMsgId) set_screen_message(chat_id, msg_id, screen);
    else sdsfree(screen);
    return 0;
}

/* Disconnect from the current terminal. */
static void disconnect(void) {
    Connected = 0;
    Streaming = 0;
    ConnectedId[0] = '\0';
    ConnectedLabel[0] = '\0';
}

/* Handle the disappearance of the connected terminal: disconnect and
 * show the list of the remaining ones. */
static void terminal_closed(int64_t chat_id) {
    disconnect();
    sds msg = sdsnew("Terminal closed.\n\n");
    sds list = build_list_message();
    msg = sdscatsds(msg, list);
    sdsfree(list);
    botSendMessageHTML(chat_id, msg, NULL, NULL, NULL);
    sdsfree(msg);
}

/* ============================================================================
 * Telegram Bot Callbacks
 * ========================================================================= */

#define OWNER_KEY "owner_id"


void handle_request(sqlite3 *db, BotRequest *br) {
    pthread_mutex_lock(&RequestLock);

    /* Check owner. First user to message becomes owner. */
    sds owner_str = kvGet(db, OWNER_KEY);
    int64_t owner_id = 0;

    if (owner_str) {
        owner_id = strtoll(owner_str, NULL, 10);
        sdsfree(owner_str);
    }

    if (owner_id == 0) {
        /* Register first user as owner. */
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)br->from);
        kvSet(db, OWNER_KEY, buf, 0);
        owner_id = br->from;
        printf("Registered owner: %lld (%s)\n", (long long)owner_id, br->from_username);
    }

    if (br->from != owner_id) {
        printf("Ignoring message from non-owner %lld\n", (long long)br->from);
        goto done;
    }

    /* TOTP authentication check (applies to both messages and callbacks). */
    if (!WeakSecurity) {
        if (!Authenticated || time(NULL) - LastActivity > OtpTimeout) {
            Authenticated = 0;
            if (br->is_callback) {
                botAnswerCallbackQuery(br->callback_id);
                goto done;
            }
            char *req = br->request;
            /* Check if message is a 6-digit OTP code. */
            int is_otp = (strlen(req) == 6);
            for (int i = 0; is_otp && i < 6; i++) {
                if (!isdigit((unsigned char)req[i])) is_otp = 0;
            }
            if (is_otp && totp_verify(db, req)) {
                Authenticated = 1;
                LastActivity = time(NULL);
                botSendMessage(br->target, "Authenticated.", 0);
            } else {
                botSendMessage(br->target, "Enter OTP code.", 0);
            }
            goto done;
        }
        LastActivity = time(NULL);
    }

    /* Handle callback query (button press): refresh the screen message,
     * stopping the stream first if the button was Stop. */
    if (br->is_callback) {
        botAnswerCallbackQuery(br->callback_id);
        if (!Connected) goto done;
        int stop = strcmp(br->callback_data, STOP_DATA) == 0;
        if (stop) Streaming = 0;
        if (refresh_screen(br->target, br->msg_id, stop) == -1)
            terminal_closed(br->target);
        goto done;
    }

    char *req = br->request;

    /* Handle .list command. */
    if (is_command(req, "list")) {
        disconnect();
        sds msg = build_list_message();
        botSendMessageHTML(br->target, msg, NULL, NULL, NULL);
        sdsfree(msg);
        goto done;
    }

    /* Handle .help command. */
    if (is_command(req, "help")) {
        sds msg = build_help_message();
        botSendMessageHTML(br->target, msg, NULL, NULL, NULL);
        sdsfree(msg);
        goto done;
    }

    /* Handle .stream command: the screen message sent becomes the one
     * updated as the terminal content changes. */
    if (is_command(req, "stream")) {
        if (!Connected) {
            botSendMessage(br->target, "Not connected.", 0);
            goto done;
        }
        Streaming = 1;
        if (send_screen(br->target) == -1) terminal_closed(br->target);
        goto done;
    }

    /* Handle .stop command. */
    if (is_command(req, "stop")) {
        if (!Streaming) {
            botSendMessage(br->target, "Not streaming.", 0);
            goto done;
        }
        Streaming = 0;
        /* Replace the Stop button of the stream message with Refresh. */
        if (refresh_screen(ScreenChat, ScreenMsgId, 1) == -1) {
            terminal_closed(br->target);
        } else {
            botSendMessage(br->target, "Streaming stopped.", 0);
        }
        goto done;
    }

    /* Handle .otptimeout command. */
    const char *arg = command_arg(req, "otptimeout");
    if (arg) {
        while (*arg == ' ') arg++;
        int secs = atoi(arg);
        if (secs < 30) secs = 30;
        if (secs > 28800) secs = 28800;
        OtpTimeout = secs;
        char buf[64];
        snprintf(buf, sizeof(buf), "%d", secs);
        kvSet(db, "otp_timeout", buf, 0);
        sds msg = sdscatprintf(sdsempty(), "OTP timeout set to %d seconds.", secs);
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    /* Handle key commands like .esc or .ctrl_c: send the key. */
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (!Commands[i].keys || !is_command(req, Commands[i].name)) continue;
        if (!Connected) {
            botSendMessage(br->target, "Not connected.", 0);
            goto done;
        }
        sds bytes = sdsnew(Commands[i].keys);
        type_bytes(bytes);
        sdsfree(bytes);
        sleep(1);
        if (send_screen(br->target) == -1) terminal_closed(br->target);
        goto done;
    }

    /* Handle .N to connect to terminal N. */
    const char *body = command_body(req);
    if (body && isdigit((unsigned char)body[0])) {
        int n = atoi(body);
        refresh_target_list();

        if (n < 1 || n > TargetCount) {
            botSendMessage(br->target, "Invalid terminal number.", 0);
            goto done;
        }

        /* Store connection info directly. */
        Target *t = &TargetList[n - 1];
        Connected = 1;
        ConnectedKind = t->kind;
        snprintf(ConnectedId, sizeof(ConnectedId), "%s", t->id);
        snprintf(ConnectedLabel, sizeof(ConnectedLabel), "%s", t->label);

        sds msg = html_escape(sdsnew("Connected to "), ConnectedLabel);
        botSendMessageHTML(br->target, msg, NULL, NULL, NULL);
        sdsfree(msg);

        if (send_screen(br->target) == -1) terminal_closed(br->target);
        goto done;
    }

    /* Not a command - send as keystrokes if connected. */
    if (!Connected) {
        sds msg = build_list_message();
        botSendMessageHTML(br->target, msg, NULL, NULL, NULL);
        sdsfree(msg);
        goto done;
    }

    send_keys(req);

    /* Give the program some time to react before showing the screen. */
    sleep(1);
    if (send_screen(br->target) == -1) terminal_closed(br->target);

done:
    pthread_mutex_unlock(&RequestLock);
}

/* Called by botlib about once per second: while streaming, update the
 * last screen message when the terminal content changes. */
void cron_callback(sqlite3 *db) {
    UNUSED(db);

    /* Set the Telegram commands menu once. This is done here and not in
     * main() since the API key is only available after startBot(). */
    static int menu_set = 0;
    if (!menu_set) {
        set_menu();
        menu_set = 1;
    }

    /* Don't wait for requests in progress: we'll retry at the next call. */
    if (pthread_mutex_trylock(&RequestLock) != 0) return;

    if (Streaming && time(NULL) - ScreenTime >= STREAM_INTERVAL) {
        if (refresh_screen(ScreenChat, ScreenMsgId, 0) == -1)
            terminal_closed(ScreenChat);
    }
    pthread_mutex_unlock(&RequestLock);
}

/* ============================================================================
 * Main
 * ========================================================================= */

int main(int argc, char **argv) {
    /* Parse our custom flags. */
    const char *dbfile = "./mybot.sqlite";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--use-weak-security") == 0) {
            WeakSecurity = 1;
            printf("WARNING: OTP authentication disabled.\n");
        } else if (strcmp(argv[i], "--dbfile") == 0 && i+1 < argc) {
            dbfile = argv[i+1];
        }
    }

    /* TOTP setup: check/generate secret before starting the bot. */
    totp_setup(dbfile);

    /* Triggers: respond to all private messages. */
    static char *triggers[] = { "*", NULL };

    startBot(TB_CREATE_KV_STORE, argc, argv, TB_FLAGS_IGNORE_BAD_ARG,
             handle_request, cron_callback, triggers);
    return 0;
}
