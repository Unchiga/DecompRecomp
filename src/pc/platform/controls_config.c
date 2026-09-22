#define _POSIX_C_SOURCE 200809L
#include "controls_config.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pc/compat/posix.h"

static int path(char *out, size_t size)
{
    const char *p = getenv("MEMORIES_CONTROLS"), *settings = getenv("MEMORIES_SETTINGS");
    int n;
    if (p && *p)
        n = snprintf(out, size, "%s", p);
    else if (settings && *settings) {
        const char *slash = strrchr(settings, '/');
        n = slash ? snprintf(out, size, "%.*s/controls.txt", (int)(slash - settings), settings)
                  : snprintf(out, size, "controls.txt");
    } else
        n = snprintf(out, size, "saves/controls.txt");
    return n >= 0 && (size_t)n < size;
}
void ControlsConfig_Token(ControlSource s, char *out, unsigned size)
{
    static const char *kind[] = {"unbound", "key", "button", "axis", "trigger", "hat"};
    if (s.kind == CTRL_SRC_UNBOUND) {
        snprintf(out, size, "unbound");
        return;
    }
    snprintf(out, size, "%s.%s", kind[s.kind],
             s.kind == CTRL_SRC_KEY ? Controls_KeyName((CtrlKeyCode)s.code) : Controls_SourceName(&s));
    for (char *p = out; *p; p++) {
        if (*p == ' ')
            *p = '_';
        else
            *p = (char)tolower((unsigned char)*p);
    }
}
static int source(const char *token, ControlSource *out)
{
    char name[80];
    for (int kind = 0; kind <= CTRL_SRC_HAT; kind++)
        for (int code = 0; code < CTRL_KEY_COUNT; code++)
            for (int sign = -1; sign <= 1; sign++) {
                ControlSource s = {(CtrlSourceKind)kind, (uint16_t)code, (int8_t)sign};
                if (!Controls_SourceValid(kind != CTRL_SRC_KEY, &s))
                    continue;
                ControlsConfig_Token(s, name, sizeof(name));
                if (!strcmp(token, name)) {
                    *out = s;
                    return 1;
                }
            }
    return 0;
}
static void hex(const char *in, char *out)
{
    static const char digits[] = "0123456789abcdef";
    if (!*in) {
        strcpy(out, "-");
        return;
    }
    for (; *in; in++) {
        unsigned c = (unsigned char)*in;
        *out++ = digits[c >> 4];
        *out++ = digits[c & 15];
    }
    *out = 0;
}
static int unhex(const char *in, char *out)
{
    size_t n = strlen(in);
    if (!strcmp(in, "-")) {
        *out = 0;
        return 1;
    }
    if (n % 2 || n >= CTRL_IDENTITY_MAX * 2)
        return 0;
    for (size_t i = 0; i < n; i += 2) {
        unsigned v;
        if (!isxdigit((unsigned char)in[i]) || !isxdigit((unsigned char)in[i + 1]) ||
            sscanf(in + i, "%2x", &v) != 1 || v == 0)
            return 0;
        *out++ = (char)v;
    }
    *out = 0;
    return 1;
}
int ControlsConfig_Load(ControlsConfig *cfg, char *error, unsigned capacity)
{
    char file[4096], line[1024], id[CTRL_IDENTITY_MAX * 2], token[80], extra;
    ControlsConfig next;
    unsigned char seen[3 + CTRL_PROFILE_MAX][32] = {{0}}, ports[2] = {0}, devices[CTRL_PROFILE_MAX] = {0};
    int version, count, bad = 0, lines = 0;
    FILE *f;
    Controls_InitDefaults(cfg);
    if (!path(file, sizeof(file))) {
        snprintf(error, capacity, "Controls path too long");
        return -1;
    }
    f = fopen(file, "r");
    if (!f) {
        if (errno == ENOENT)
            return 0;
        snprintf(error, capacity, "Cannot read controls: %s", strerror(errno));
        return -1;
    }
    if (!fgets(line, sizeof(line), f) || sscanf(line, "controls %d %d %c", &version, &count, &extra) != 2) {
        fclose(f);
        goto malformed;
    }
    if (version != CTRL_CONFIG_VERSION) {
        fclose(f);
        snprintf(error, capacity, "Unsupported controls version %d; file preserved", version);
        return -2;
    }
    if (count < 0 || count > CTRL_PROFILE_MAX) {
        fclose(f);
        goto malformed;
    }
    Controls_InitDefaults(&next);
    next.profile_count = count;
    while (fgets(line, sizeof(line), f)) {
        int a, b, mode, icon;
        if (++lines > 1024 || !strchr(line, '\n')) {
            bad = 1;
            break;
        }
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "port %d %d %d %511s %c", &a, &mode, &icon, id, &extra) == 4) {
            if (a < 0 || a >= 2 || ports[a]++ || !unhex(id, next.port[a].identity)) {
                bad = 1;
                break;
            }
            next.port[a].mode = mode;
            next.port[a].icon = (CtrlIconStyle)icon;
        } else if (sscanf(line, "device %d %d %511s %c", &a, &icon, id, &extra) == 3) {
            if (a < 0 || a >= count || devices[a]++ || !unhex(id, next.profiles[a].identity)) {
                bad = 1;
                break;
            }
            next.profiles[a].icon = (CtrlIconStyle)icon;
        } else if (sscanf(line, "bind %d %d %79s %c", &a, &b, token, &extra) == 3) {
            ControlsProfile *profile;
            if (a < 0 || a >= 3 + count || b < 0 || b >= 32 || seen[a][b]++) {
                bad = 1;
                break;
            }
            profile = a == 0 ? &next.kb : a < 3 ? &next.ctrl[a - 1] : &next.profiles[a - 3].bindings;
            if (!source(token, &profile->src[b / 2][b % 2])) {
                bad = 1;
                break;
            }
        } else {
            bad = 1;
            break;
        }
    }
    if (ferror(f))
        bad = 1;
    fclose(f);
    for (int i = 0; i < 2; i++)
        if (!ports[i])
            bad = 1;
    for (int i = 0; i < count; i++)
        if (!devices[i])
            bad = 1;
    for (int a = 0; a < 3 + count; a++)
        for (int b = 0; b < 32; b++)
            if (!seen[a][b])
                bad = 1;
    if (bad || !Controls_ConfigValid(&next))
        goto malformed;
    *cfg = next;
    return 1;
malformed:
    snprintf(error, capacity, "Invalid controls file; using defaults");
    return -1;
}
int ControlsConfig_Save(const ControlsConfig *cfg, char *error, unsigned capacity)
{
    char file[4096], temp[4120], id[CTRL_IDENTITY_MAX * 2], token[80];
    int fd, ok = 1, version;
    FILE *f;
    if (!Controls_ConfigValid(cfg)) {
        snprintf(error, capacity, "Invalid bindings or duplicate controller selection");
        return 0;
    }
    if (!path(file, sizeof(file))) {
        snprintf(error, capacity, "Controls path too long");
        return 0;
    }
    f = fopen(file, "r");
    if (f) {
        if (fscanf(f, "controls %d", &version) == 1 && version != CTRL_CONFIG_VERSION) {
            fclose(f);
            snprintf(error, capacity, "Unsupported controls version; file preserved");
            return 0;
        }
        fclose(f);
    }
    if (!strncmp(file, "saves/", 6))
        (void)mkdir("saves", 0755);
    snprintf(temp, sizeof(temp), "%s.tmp.XXXXXX", file);
    fd = mkstemp(temp);
    if (fd < 0)
        goto failure;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(temp);
        goto failure;
    }
    if (fprintf(f, "controls %d %d\n", CTRL_CONFIG_VERSION, cfg->profile_count) < 0)
        ok = 0;
    for (int p = 0; p < 2; p++) {
        hex(cfg->port[p].identity, id);
        if (fprintf(f, "port %d %d %d %s\n", p, cfg->port[p].mode, cfg->port[p].icon, id) < 0)
            ok = 0;
    }
    for (int i = 0; i < cfg->profile_count; i++) {
        hex(cfg->profiles[i].identity, id);
        if (fprintf(f, "device %d %d %s\n", i, cfg->profiles[i].icon, id) < 0)
            ok = 0;
    }
    for (int a = 0; a < 3 + cfg->profile_count; a++) {
        const ControlsProfile *profile = a == 0  ? &cfg->kb
                                         : a < 3 ? &cfg->ctrl[a - 1]
                                                 : &cfg->profiles[a - 3].bindings;
        for (int b = 0; b < 32; b++) {
            ControlsConfig_Token(profile->src[b / 2][b % 2], token, sizeof(token));
            if (fprintf(f, "bind %d %d %s\n", a, b, token) < 0)
                ok = 0;
        }
    }
    if (fflush(f) || fsync(fd))
        ok = 0;
    if (fclose(f))
        ok = 0;
    if (ok && rename(temp, file) == 0)
        return 1;
    unlink(temp);
failure:
    snprintf(error, capacity, "Could not save controls: %s", strerror(errno));
    return 0;
}
