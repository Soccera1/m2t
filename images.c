#define _XOPEN_SOURCE 700
#include "images.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include <webp/decode.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 32768
#include <stb_image.h>

#define MAX_IMAGE_BYTES (64u * 1024u * 1024u)
#define MAX_IMAGE_PIXELS (16u * 1024u * 1024u)

typedef struct ImageRequest {
    struct ImageRequest *next;
    char *source;
    ImageSlot slot;
    Image image;
    unsigned width, height;
    uint32_t background;
    int64_t deadline;
    bool done, success, processed, processing;
} ImageRequest;

struct Images {
    const char *source;
    ImageMode mode;
    bool checked, supported, tmux, curl_ready;
    unsigned cell_width, cell_height, columns, rows;
    unsigned timeout_seconds;
    uint32_t background, next_id;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t workers[4];
    size_t worker_count, pending;
    bool workers_started;
    atomic_bool stopping;
    ImageRequest *requests, *last, *next_work;
};

typedef struct {
    unsigned char *bytes;
    size_t len, cap;
} Bytes;

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t encode64(const unsigned char *s, size_t n, char *out)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t value = (uint32_t)s[i] << 16;
        if (i + 1 < n) value |= (uint32_t)s[i + 1] << 8;
        if (i + 2 < n) value |= s[i + 2];
        out[k++] = alphabet[value >> 18];
        out[k++] = alphabet[(value >> 12) & 63];
        out[k++] = i + 1 < n ? alphabet[(value >> 6) & 63] : '=';
        out[k++] = i + 2 < n ? alphabet[value & 63] : '=';
    }
    return k;
}

static int base64_digit(unsigned char c)
{
    const char *p = c ? strchr(alphabet, c) : NULL;
    return p ? (int)(p - alphabet) : -1;
}

static bool decode64(const char *s, size_t n, Bytes *out)
{
    if (n % 4 || n / 4 * 3 > MAX_IMAGE_BYTES) return false;
    out->bytes = malloc(n / 4 * 3 + 1);
    if (!out->bytes) return false;
    for (size_t i = 0; i < n; i += 4) {
        int a = base64_digit((unsigned char)s[i]);
        int b = base64_digit((unsigned char)s[i + 1]);
        int c = s[i + 2] == '=' ? 0 : base64_digit((unsigned char)s[i + 2]);
        int d = s[i + 3] == '=' ? 0 : base64_digit((unsigned char)s[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (s[i + 2] == '=' && s[i + 3] != '=') ||
            ((s[i + 2] == '=' || s[i + 3] == '=') && i + 4 != n)) return false;
        uint32_t value = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6 | (uint32_t)d;
        out->bytes[out->len++] = (unsigned char)(value >> 16);
        if (s[i + 2] != '=') out->bytes[out->len++] = (unsigned char)(value >> 8);
        if (s[i + 3] != '=') out->bytes[out->len++] = (unsigned char)value;
    }
    return true;
}

static void send_command(FILE *out, bool tmux, const char *command, size_t len)
{
    if (tmux) fputs("\033Ptmux;", out);
    for (size_t i = 0; i < len; i++) {
        if (tmux && command[i] == '\033') fputc('\033', out);
        fputc((unsigned char)command[i], out);
    }
    if (tmux) fputs("\033\\", out);
}

static volatile sig_atomic_t probe_signal;

static void interrupted(int signum)
{
    probe_signal = signum;
}

static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int hex_digit(unsigned char c);

static void read_background(Images *images, const char *response)
{
    const char *p = strstr(response, "\033]11;rgb:");
    if (!p) return;
    p += 9;
    uint32_t color = 0xff000000u;
    for (unsigned channel = 0; channel < 3; channel++) {
        unsigned value = 0, digits = 0;
        int digit;
        while ((digit = hex_digit((unsigned char)*p)) >= 0 && digits < 4) {
            value = value * 16 + (unsigned)digit;
            digits++;
            p++;
        }
        if (!digits || (channel < 2 && *p++ != '/')) return;
        unsigned maximum = (1u << (digits * 4)) - 1;
        color |= ((value * 255 + maximum / 2) / maximum) << (16 - channel * 8);
    }
    if (*p == '\007' || (*p == '\033' && p[1] == '\\')) images->background = color;
}

static bool graphics_reply(const char *response, unsigned id, const char *message)
{
    for (const char *p = response; (p = strstr(p, "\033_G")); p += 3) {
        const char *end = strstr(p, "\033\\");
        const char *payload = strchr(p, ';');
        if (!end || !payload || payload > end) continue;
        for (const char *key = p + 3; key < payload;) {
            if (key + 2 < payload && key[0] == 'i' && key[1] == '=') {
                char *next;
                unsigned long value = strtoul(key + 2, &next, 10);
                if (next <= payload && value == id && next > key + 2 &&
                    (next == payload || *next == ',') &&
                    (size_t)(end - payload - 1) >= strlen(message) &&
                    !strncmp(payload + 1, message, strlen(message))) return true;
            }
            const char *comma = memchr(key, ',', (size_t)(payload - key));
            if (!comma) break;
            key = comma + 1;
        }
    }
    return false;
}

static void read_cell_size(Images *images, const char *response)
{
    const char *p = strstr(response, "\033[6;");
    unsigned height, width;
    int consumed = 0;
    if (p && sscanf(p, "\033[6;%u;%ut%n", &height, &width, &consumed) == 2 &&
        consumed && width && height && width <= 4096 && height <= 4096) {
        images->cell_width = width;
        images->cell_height = height;
    }
}

static bool probe_terminal(Images *images, bool protocol)
{
    if (!isatty(STDOUT_FILENO) || tcgetsid(STDOUT_FILENO) != getsid(0) ||
        tcgetpgrp(STDOUT_FILENO) != getpgrp()) return false;
    int fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct termios saved, raw;
    struct pollfd input = {.fd = fd, .events = POLLIN};
    /* Do not consume input which was already waiting before our query. */
    if (poll(&input, 1, 0) != 0 || tcgetattr(fd, &saved)) { close(fd); return false; }
    raw = saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_iflag &= (tcflag_t)~(ICRNL | INLCR | IGNCR | IXON | IXOFF);
    raw.c_cc[VMIN] = raw.c_cc[VTIME] = 0;
    static const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP};
    struct sigaction previous[sizeof(signals) / sizeof(signals[0])];
    struct sigaction action = {.sa_handler = interrupted};
    sigemptyset(&action.sa_mask);
    probe_signal = 0;
    size_t installed = 0;
    for (; installed < sizeof(signals) / sizeof(signals[0]); installed++)
        if (sigaction(signals[installed], &action, &previous[installed])) break;
    bool supported = false;
    if (installed != sizeof(signals) / sizeof(signals[0]) || tcsetattr(fd, TCSANOW, &raw))
        goto done;
    unsigned id = 0x40000000u | ((unsigned)getpid() & 0x3fffffffu);
    char query[512];
    int len = 0;
    if (protocol) {
        /* A terminal with relative placements must reject the missing parent
         * placement with ENOPARENT. Older implementations which ignore P/Q
         * merely place an invisible pixel; delete it immediately either way. */
        len = snprintf(query, sizeof(query),
            "\033_Gi=%u,s=1,v=1,a=q,t=d,f=24;AAAA\033\\"
            "\033_Ga=t,i=%u,s=1,v=1,f=32,q=2;AAAAAA==\033\\"
            "\033_Ga=p,i=%u,p=1,P=%u,Q=2,c=1,r=1,C=1;\033\\"
            "\033_Ga=d,d=I,i=%u,q=2;\033\\", id, id + 1, id + 1, id + 1, id + 1);
    }
    len += snprintf(query + len, sizeof(query) - (size_t)len, "\033]11;?\033\\\033[16t\033[c");
    send_command(stdout, images->tmux, query, (size_t)len);
    fflush(stdout);
    int64_t deadline = milliseconds() + 250;
    char response[1024] = {0};
    size_t used = 0;
    bool device_attributes = false;
    while (!probe_signal && !device_attributes) {
        int64_t remaining = deadline - milliseconds();
        if (remaining <= 0) break;
        int ready = poll(&input, 1, (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || !(input.revents & POLLIN)) break;
        char c;
        ssize_t count = read(fd, &c, 1);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count != 1) break;
        if (used + 1 >= sizeof(response)) break;
        response[used++] = c;
        response[used] = '\0';
        if (c == 't') read_cell_size(images, response);
        if ((used >= 2 && response[used - 2] == '\033' && c == '\\') || c == '\007') {
            read_background(images, response);
            supported = graphics_reply(response, id, "OK") &&
                        graphics_reply(response, id + 1, "ENOPARENT");
        }
        if (c == 'c') {
            char *start = strrchr(response, '\033');
            if (start && start[1] == '[' && (start[2] == '?' || start[2] == '>'))
                device_attributes = true;
        }
    }
done:
    /* TCSANOW preserves any unread data; never flush the user's input. */
    while (tcsetattr(fd, TCSANOW, &saved) && errno == EINTR) {}
    close(fd);
    for (size_t i = 0; i < installed; i++) sigaction(signals[i], &previous[i], NULL);
    if (probe_signal) raise(probe_signal);
    return supported;
}

Images *images_create(const char *source, ImageMode mode, unsigned timeout_seconds)
{
    Images *images = calloc(1, sizeof(*images));
    if (!images) return NULL;
    images->source = source && strcmp(source, "-") ? source : "./stdin";
    images->mode = mode;
    images->timeout_seconds = timeout_seconds;
    images->background = 0xff000000u;
    images->next_id = (uint32_t)((uint64_t)milliseconds() * 2654435761u) ^ (uint32_t)getpid();
    atomic_init(&images->stopping, false);
    if (pthread_mutex_init(&images->mutex, NULL)) { free(images); return NULL; }
    if (pthread_cond_init(&images->changed, NULL)) {
        pthread_mutex_destroy(&images->mutex);
        free(images);
        return NULL;
    }
    const char *tmux = getenv("TMUX");
    images->tmux = tmux && *tmux;
    images->cell_width = 8;
    images->cell_height = 16;
    images->columns = 80;
    images->rows = 24;
    struct winsize size;
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &size)) {
        if (size.ws_col) images->columns = size.ws_col;
        if (size.ws_row) images->rows = size.ws_row;
        if (size.ws_col && size.ws_xpixel >= size.ws_col)
            images->cell_width = size.ws_xpixel / size.ws_col;
        if (size.ws_row && size.ws_ypixel >= size.ws_row)
            images->cell_height = size.ws_ypixel / size.ws_row;
    }
    return images;
}

void images_destroy(Images *images)
{
    if (!images) return;
    atomic_store(&images->stopping, true);
    pthread_mutex_lock(&images->mutex);
    pthread_cond_broadcast(&images->changed);
    pthread_mutex_unlock(&images->mutex);
    for (size_t i = 0; i < images->worker_count; i++) pthread_join(images->workers[i], NULL);
    for (ImageRequest *request = images->requests; request;) {
        ImageRequest *next = request->next;
        image_destroy(&request->image);
        free(request->source);
        free(request);
        request = next;
    }
    if (images->curl_ready) curl_global_cleanup();
    pthread_cond_destroy(&images->changed);
    pthread_mutex_destroy(&images->mutex);
    free(images);
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *unescape(const char *s, size_t n, size_t *length)
{
    if (n > MAX_IMAGE_BYTES) return NULL;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    *length = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%' && i + 2 < n) {
            int high = hex_digit((unsigned char)s[i + 1]);
            int low = hex_digit((unsigned char)s[i + 2]);
            if (high >= 0 && low >= 0) { c = (unsigned char)(high * 16 + low); i += 2; }
        }
        out[(*length)++] = (char)c;
    }
    out[*length] = '\0';
    return out;
}

static bool append_bytes(Bytes *out, const unsigned char *s, size_t n)
{
    if (!n) return true;
    if (n > MAX_IMAGE_BYTES - out->len) return false;
    if (out->len + n > out->cap) {
        size_t cap = out->cap ? out->cap : 4096;
        while (cap < out->len + n) cap *= 2;
        unsigned char *p = realloc(out->bytes, cap);
        if (!p) return false;
        out->bytes = p;
        out->cap = cap;
    }
    memcpy(out->bytes + out->len, s, n);
    out->len += n;
    return true;
}

static size_t receive_image(char *data, size_t size, size_t count, void *userdata)
{
    if (size && count > SIZE_MAX / size) return 0;
    size_t n = size * count;
    return append_bytes(userdata, (const unsigned char *)data, n) ? n : 0;
}

static int cancel_transfer(void *userdata, curl_off_t total, curl_off_t received,
                            curl_off_t upload_total, curl_off_t uploaded)
{
    (void)total; (void)received; (void)upload_total; (void)uploaded;
    return atomic_load(&((Images *)userdata)->stopping) ? 1 : 0;
}

static bool http_image(Images *images, const char *url, Bytes *out, int64_t deadline)
{
    int64_t remaining = deadline - milliseconds();
    if (!images->curl_ready || remaining <= 0 || atomic_load(&images->stopping)) return false;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, remaining < 3000 ? (long)remaining : 3000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)remaining);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)MAX_IMAGE_BYTES);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "m2t");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_image);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancel_transfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, images);
    CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
}

static bool read_image(Images *images, const char *src, Bytes *out, int64_t deadline)
{
    if (milliseconds() >= deadline || atomic_load(&images->stopping)) return false;
    if (!strncasecmp(src, "data:image/", 11)) {
        const char *comma = strchr(src, ',');
        if (!comma) return false;
        size_t len;
        char *data = unescape(comma + 1, strlen(comma + 1), &len);
        if (!data) return false;
        bool encoded = comma - src >= 7 && !strncasecmp(comma - 7, ";base64", 7);
        bool ok = encoded ? decode64(data, len, out) :
                            append_bytes(out, (const unsigned char *)data, len);
        free(data);
        return ok;
    }
    if (!strncasecmp(src, "http://", 7) || !strncasecmp(src, "https://", 8))
        return http_image(images, src, out, deadline);
    if (!strncmp(src, "//", 2)) {
        size_t n = strlen(src);
        char *url = malloc(n + 7);
        if (!url) return false;
        snprintf(url, n + 7, "https:%s", src);
        bool ok = http_image(images, url, out, deadline);
        free(url);
        return ok;
    }
    if (!strncasecmp(src, "file://", 7)) {
        src += 7;
        if (!strncasecmp(src, "localhost/", 10)) src += 9;
        if (*src != '/') return false;
    } else if (strchr(src, ':')) return false;
    size_t len;
    char *relative = unescape(src, strcspn(src, "?#"), &len);
    if (!relative || memchr(relative, '\0', len)) { free(relative); return false; }
    const char *slash = strrchr(images->source, '/');
    size_t prefix = *relative == '/' || !slash ? 0 : (size_t)(slash - images->source) + 1;
    if (prefix > SIZE_MAX - len - 1) { free(relative); return false; }
    char *path = malloc(prefix + len + 1);
    if (!path) { free(relative); return false; }
    memcpy(path, images->source, prefix);
    memcpy(path + prefix, relative, len + 1);
    free(relative);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    free(path);
    if (fd < 0) return false;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size > MAX_IMAGE_BYTES) {
        close(fd);
        return false;
    }
    unsigned char chunk[16384];
    bool ok = true;
    for (;;) {
        if (milliseconds() >= deadline || atomic_load(&images->stopping)) { ok = false; break; }
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (!n) break;
        if (!append_bytes(out, chunk, (size_t)n)) { ok = false; break; }
    }
    close(fd);
    return ok;
}

static bool valid_dimensions(int width, int height)
{
    return width > 0 && height > 0 && width <= STBI_MAX_DIMENSIONS &&
           height <= STBI_MAX_DIMENSIONS && (size_t)width * (size_t)height <= MAX_IMAGE_PIXELS;
}

typedef struct {
    int first, last;
    double begin, end, fraction;
    bool downsample;
} Axis;

static Axis sample_axis(unsigned source, unsigned target, unsigned pixel)
{
    double scale = (double)source / target;
    if (source > target) {
        double begin = pixel * scale, end = (pixel + 1) * scale;
        return (Axis){.first = (int)floor(begin), .last = (int)ceil(end) - 1,
                      .begin = begin, .end = end, .downsample = true};
    }
    double center = (pixel + .5) * scale - .5;
    int first = (int)floor(center);
    return (Axis){.first = first, .last = first + 1, .fraction = center - first};
}

static double sample_weight(const Axis *axis, int pixel)
{
    if (!axis->downsample) return pixel == axis->first ? 1 - axis->fraction : axis->fraction;
    double begin = pixel > axis->begin ? pixel : axis->begin;
    double end = pixel + 1 < axis->end ? pixel + 1 : axis->end;
    return end > begin ? end - begin : 0;
}

static unsigned char srgb_byte(double linear)
{
    if (linear <= 0) return 0;
    if (linear >= 1) return 255;
    double value = linear <= .0031308 ? linear * 12.92 :
                   1.055 * pow(linear, 1.0 / 2.4) - .055;
    return (unsigned char)(value * 255 + .5);
}

/* Area filtering when shrinking, bilinear filtering when enlarging. Blend in
 * linear light with premultiplied alpha so transparent edges do not gain halos. */
static unsigned char *resize_rgba(const unsigned char *source, unsigned width,
                                  unsigned height, unsigned target_width, unsigned target_height)
{
    unsigned char *target = malloc((size_t)target_width * target_height * 4);
    if (!target) return NULL;
    double linear[256];
    for (unsigned i = 0; i < 256; i++) {
        double value = i / 255.0;
        linear[i] = value <= .04045 ? value / 12.92 : pow((value + .055) / 1.055, 2.4);
    }
    for (unsigned y = 0; y < target_height; y++) {
        Axis vertical = sample_axis(height, target_height, y);
        for (unsigned x = 0; x < target_width; x++) {
            Axis horizontal = sample_axis(width, target_width, x);
            double total = 0, alpha = 0, channels[3] = {0};
            for (int sy = vertical.first; sy <= vertical.last; sy++) {
                unsigned py = sy < 0 ? 0 : (unsigned)sy >= height ? height - 1 : (unsigned)sy;
                double wy = sample_weight(&vertical, sy);
                for (int sx = horizontal.first; sx <= horizontal.last; sx++) {
                    unsigned px = sx < 0 ? 0 : (unsigned)sx >= width ? width - 1 : (unsigned)sx;
                    const unsigned char *pixel = source + ((size_t)py * width + px) * 4;
                    double weight = wy * sample_weight(&horizontal, sx);
                    double a = pixel[3] / 255.0 * weight;
                    total += weight;
                    alpha += a;
                    for (unsigned c = 0; c < 3; c++) channels[c] += linear[pixel[c]] * a;
                }
            }
            unsigned char *pixel = target + ((size_t)y * target_width + x) * 4;
            for (unsigned c = 0; c < 3; c++) pixel[c] = alpha > 0 ? srgb_byte(channels[c] / alpha) : 0;
            double a = total > 0 ? alpha / total : 0;
            pixel[3] = a >= 1 ? 255 : a <= 0 ? 0 : (unsigned char)(a * 255 + .5);
        }
    }
    return target;
}

static bool load_image(Images *images, const char *src, unsigned columns, unsigned row_limit,
                        unsigned requested_width, unsigned requested_height,
                        int64_t deadline, Image *image)
{
    Bytes data = {0};
    bool ok = false;
    unsigned char *pixels = NULL;
    if (!read_image(images, src, &data, deadline) || !data.len || data.len > INT_MAX) goto done;
    int width, height, channels;
    if (stbi_info_from_memory(data.bytes, (int)data.len, &width, &height, &channels)) {
        if (!valid_dimensions(width, height)) goto done;
        pixels = stbi_load_from_memory(data.bytes, (int)data.len, &width, &height, &channels, 4);
    } else if (WebPGetInfo(data.bytes, data.len, &width, &height)) {
        if (!valid_dimensions(width, height)) goto done;
        size_t bytes = (size_t)width * (size_t)height * 4;
        pixels = malloc(bytes);
        if (pixels && !WebPDecodeRGBAInto(data.bytes, data.len, pixels, bytes, width * 4)) {
            free(pixels);
            pixels = NULL;
        }
    }
    if (!pixels || !valid_dimensions(width, height)) goto done;
    if (!columns || columns > images->columns) columns = images->columns;
    unsigned max_width = (unsigned)columns * images->cell_width;
    unsigned max_height = row_limit * images->cell_height;
    double scale = 1;
    if (requested_width) scale = (double)requested_width / width;
    if (requested_height && (!requested_width || (double)requested_height / height < scale))
        scale = (double)requested_height / height;
    if (scale > (double)max_width / width) scale = (double)max_width / width;
    if (scale > (double)max_height / height) scale = (double)max_height / height;
    double safe_scale = sqrt((double)MAX_IMAGE_PIXELS / ((double)width * height));
    if (scale > safe_scale) scale = safe_scale;
    if (scale > (double)STBI_MAX_DIMENSIONS / width) scale = (double)STBI_MAX_DIMENSIONS / width;
    if (scale > (double)STBI_MAX_DIMENSIONS / height) scale = (double)STBI_MAX_DIMENSIONS / height;
    unsigned target_width = (unsigned)(width * scale);
    unsigned target_height = (unsigned)(height * scale);
    if (!target_width) target_width = 1;
    if (!target_height) target_height = 1;
    if (target_width != (unsigned)width || target_height != (unsigned)height) {
        unsigned char *resized = resize_rgba(pixels, (unsigned)width, (unsigned)height,
                                              target_width, target_height);
        free(pixels);
        pixels = resized;
        if (!pixels) goto done;
    }
    if (milliseconds() >= deadline || atomic_load(&images->stopping)) goto done;
    image->pixels = pixels;
    image->width = target_width;
    image->height = target_height;
    image->columns = (target_width + images->cell_width - 1) / images->cell_width;
    image->rows = (target_height + images->cell_height - 1) / images->cell_height;
    pixels = NULL;
    ok = true;
done:
    free(pixels);
    free(data.bytes);
    return ok;
}

typedef struct {
    uint32_t id, parent;
    unsigned columns, rows, x, y;
    int z;
} Placement;

static bool write_image(Images *images, FILE *out, const Image *image, const Placement *place)
{
    size_t total = (size_t)image->width * image->height * 4;
    for (size_t offset = 0; offset < total;) {
        size_t n = total - offset;
        if (n > 3072) n = 3072;
        bool more = offset + n < total;
        char command[4608];
        int head;
        if (!offset) {
            head = snprintf(command, sizeof(command),
                "\033_Ga=T,t=d,f=32,s=%u,v=%u,c=%u,r=%u,C=1,q=2,m=%d,i=%u,p=1,z=%d",
                image->width, image->height, place->columns, place->rows, more, place->id, place->z);
            if (place->parent) head += snprintf(command + head, sizeof(command) - (size_t)head,
                ",P=%u,Q=1,H=%u,V=%u", place->parent, place->x, place->y);
            command[head++] = ';';
        }
        else head = snprintf(command, sizeof(command), "\033_Gm=%d,q=2;", more);
        size_t len = (size_t)head;
        len += encode64(image->pixels + offset, n, command + len);
        command[len++] = '\033';
        command[len++] = '\\';
        send_command(out, images->tmux, command, len);
        offset += n;
    }
    return !ferror(out);
}

static void *image_worker(void *userdata)
{
    Images *images = userdata;
    for (;;) {
        pthread_mutex_lock(&images->mutex);
        while (!images->next_work && !atomic_load(&images->stopping))
            pthread_cond_wait(&images->changed, &images->mutex);
        if (atomic_load(&images->stopping)) { pthread_mutex_unlock(&images->mutex); break; }
        ImageRequest *request = images->next_work;
        images->next_work = request->next;
        pthread_mutex_unlock(&images->mutex);
        bool success = load_image(images, request->source, request->slot.columns,
            request->slot.rows, request->width, request->height, request->deadline, &request->image);
        pthread_mutex_lock(&images->mutex);
        request->success = success;
        request->done = true;
        pthread_cond_broadcast(&images->changed);
        /* Retain at most one decoded result per worker while text is rendering. */
        while (!request->processed && !atomic_load(&images->stopping))
            pthread_cond_wait(&images->changed, &images->mutex);
        pthread_mutex_unlock(&images->mutex);
    }
    return NULL;
}

static bool start_workers(Images *images)
{
    if (images->mode == IMAGES_NEVER) return false;
    if (!images->checked) {
        if (images->mode == IMAGES_ALWAYS) {
            probe_terminal(images, false); /* Only query the terminal background. */
            images->supported = true;
        } else images->supported = probe_terminal(images, true);
        images->checked = true;
    }
    if (!images->supported) return false;
    if (!images->workers_started) {
        images->workers_started = true;
        /* libcurl global setup must finish before any worker can use it. */
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return false;
        images->curl_ready = true;
        for (; images->worker_count < sizeof(images->workers) / sizeof(images->workers[0]);
             images->worker_count++)
            if (pthread_create(&images->workers[images->worker_count], NULL, image_worker, images)) break;
    }
    return images->worker_count != 0;
}

bool image_queue(Images *images, const char *src, size_t columns, size_t inset,
                  unsigned width, unsigned height, uint32_t background, ImageSlot *slot)
{
    if (!images || !*src) return false;
    if (!columns || columns > images->columns) columns = images->columns;
    if (inset >= columns || !start_workers(images)) return false;
    ImageRequest *request = calloc(1, sizeof(*request));
    if (!request) return false;
    request->source = strdup(src);
    if (!request->source) { free(request); return false; }
    unsigned max_rows = images->rows > 2 ? images->rows - 2 : 1;
    unsigned rows = max_rows < 8 ? max_rows : 8;
    if (height) {
        double scale = width && (double)width > (columns - inset) * images->cell_width ?
            (double)((columns - inset) * images->cell_width) / width : 1;
        double desired_rows = ceil(height * scale / images->cell_height);
        rows = desired_rows > max_rows ? max_rows : (unsigned)desired_rows;
        if (!rows) rows = 1;
    }
    if (!images->next_id || images->next_id > UINT32_MAX - 3) images->next_id = 1;
    request->slot = (ImageSlot){.columns = (unsigned)(columns - inset), .rows = rows,
                                .id = images->next_id};
    images->next_id += 3;
    request->width = width;
    request->height = height;
    request->background = background >> 24 ? background : images->background;
    request->deadline = milliseconds() + (int64_t)images->timeout_seconds * 1000;
    *slot = request->slot;
    pthread_mutex_lock(&images->mutex);
    if (images->last) images->last->next = request;
    else images->requests = request;
    images->last = request;
    if (!images->next_work) images->next_work = request;
    images->pending++;
    pthread_cond_broadcast(&images->changed);
    pthread_mutex_unlock(&images->mutex);
    return true;
}

bool image_anchor(Images *images, FILE *out, const ImageSlot *slot)
{
    unsigned char pixel[4] = {0};
    Image transparent = {.pixels = pixel, .width = 1, .height = 1};
    Placement place = {.id = slot->id, .columns = slot->columns, .rows = slot->rows, .z = -1};
    return write_image(images, out, &transparent, &place);
}

static bool finish_request(Images *images, FILE *out, const ImageRequest *request)
{
    if (!request->success) {
        char command[96];
        int n = snprintf(command, sizeof(command), "\033_Ga=d,d=I,i=%u,q=2;\033\\", request->slot.id);
        send_command(out, images->tmux, command, (size_t)n);
        return !ferror(out);
    }
    /* Cover the label with the surrounding background before displaying the
     * image, retaining transparent source pixels without text showing through. */
    unsigned char pixel[4] = {(unsigned char)(request->background >> 16),
        (unsigned char)(request->background >> 8), (unsigned char)request->background, 255};
    unsigned width = request->slot.columns * images->cell_width;
    unsigned height = request->slot.rows * images->cell_height;
    /* A solid backdrop needs only the same aspect ratio, not a full-size
     * bitmap. Reducing by the GCD usually makes this just a few pixels. */
    unsigned divisor = width, remainder = height;
    while (remainder) {
        unsigned next = divisor % remainder;
        divisor = remainder;
        remainder = next;
    }
    if (divisor) { width /= divisor; height /= divisor; }
    if (!width || !height || width > MAX_IMAGE_PIXELS / height) return false;
    Image backdrop = {.width = width, .height = height};
    backdrop.pixels = malloc((size_t)width * height * 4);
    if (!backdrop.pixels) return false;
    for (size_t i = 0; i < (size_t)width * height; i++) memcpy(backdrop.pixels + i * 4, pixel, 4);
    Placement place = {.id = request->slot.id + 1, .parent = request->slot.id,
        .columns = request->slot.columns, .rows = request->slot.rows, .z = 1};
    bool ok = write_image(images, out, &backdrop, &place);
    image_destroy(&backdrop);
    if (!ok) return false;
    place.id++;
    place.z = 2;
    place.columns = request->image.columns;
    place.rows = request->image.rows;
    place.x = (request->slot.columns - place.columns) / 2;
    place.y = (request->slot.rows - place.rows) / 2;
    return write_image(images, out, &request->image, &place);
}

bool images_finish(Images *images, FILE *out)
{
    if (!images || fflush(out)) return false;
    pthread_mutex_lock(&images->mutex);
    while (images->pending && !atomic_load(&images->stopping)) {
        ImageRequest *ready = NULL;
        for (ImageRequest *request = images->requests; request; request = request->next)
            if (request->done && !request->processed && !request->processing) { ready = request; break; }
        if (!ready) { pthread_cond_wait(&images->changed, &images->mutex); continue; }
        ready->processing = true;
        pthread_mutex_unlock(&images->mutex);
        bool ok = finish_request(images, out, ready) && !fflush(out);
        image_destroy(&ready->image);
        pthread_mutex_lock(&images->mutex);
        ready->processed = true;
        images->pending--;
        pthread_cond_broadcast(&images->changed);
        if (!ok) { pthread_mutex_unlock(&images->mutex); return false; }
    }
    pthread_mutex_unlock(&images->mutex);
    return !ferror(out);
}

void image_destroy(Image *image)
{
    free(image->pixels);
    memset(image, 0, sizeof(*image));
}
