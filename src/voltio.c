/**
 * voltio.c  —  BK Precision 1900B-Series protocol driver
 *
 * Platform-agnostic. All serial I/O goes through serial.h.
 */

#include "voltio.h"
#include "serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Voltage set-values always have one decimal place on the wire. */
#define VOLT_SCALE 10

/* GETD readings are 4 digits with two decimal places. */
#define DISPLAY_SCALE 100

/* Give up if this many bytes arrive without an "OK" line — guards against
 * a device that keeps talking but never completes a response.          */
#define MAX_RESPONSE_BYTES 256

/* -------------------------------------------------------------------------
 * Internal context
 * ---------------------------------------------------------------------- */
struct Voltio_Context {
    Serial_Port *port;
    int          curr_scale;   /* 10 (1 d.p.) or 100 (2 d.p.) */
    char         last_error[256];
};

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */
static Voltio_Status send_command(Voltio_Context *ctx, const char *cmd, char *resp_buf, size_t resp_len);
static Voltio_Status encode_value(Voltio_Context *ctx, const char *what, float val, int scale, char out[4]);
static Voltio_Status set_value   (Voltio_Context *ctx, const char *prefix, const char *what, float val, int scale);
static int           parse_field (const char *s, int digits, int scale, float *out);
static void          set_error   (Voltio_Context *ctx, const char *fmt, ...);

/* =========================================================================
 * Lifecycle
 * ====================================================================== */

Voltio_Context *voltio_open(const char *port, int dp)
{
    if (!port) return NULL;
    Voltio_Context *ctx = (Voltio_Context *)calloc(1, sizeof(Voltio_Context));
    if (!ctx) return NULL;
    if (dp != 1 && dp != 2) {
        set_error(ctx, "dp must be 1 or 2, got %d", dp);
        return ctx;
    }
    ctx->curr_scale = (dp == 2) ? 100 : 10;
    char serial_err[256] = {0};
    ctx->port = serial_open(port, serial_err, sizeof(serial_err));
    if (!ctx->port) set_error(ctx, "%s", serial_err);
    return ctx;
}

void voltio_close(Voltio_Context *ctx)
{
    if (!ctx) return;
    serial_close(ctx->port);
    free(ctx);
}

const char *voltio_last_error(const Voltio_Context *ctx)
{
    if (!ctx) return "NULL context";
    return ctx->last_error;
}

/* =========================================================================
 * Output control
 * ====================================================================== */

Voltio_Status voltio_set_voltage(Voltio_Context *ctx, float volts)
{
    if (!ctx) return VOLTIO_ERR_NULL;
    return set_value(ctx, "VOLT", "voltage", volts, VOLT_SCALE);
}

Voltio_Status voltio_set_current(Voltio_Context *ctx, float amps)
{
    if (!ctx) return VOLTIO_ERR_NULL;
    return set_value(ctx, "CURR", "current", amps, ctx->curr_scale);
}

Voltio_Status voltio_output_on (Voltio_Context *ctx) { if (!ctx) return VOLTIO_ERR_NULL; return send_command(ctx, "SOUT0", NULL, 0); }
Voltio_Status voltio_output_off(Voltio_Context *ctx) { if (!ctx) return VOLTIO_ERR_NULL; return send_command(ctx, "SOUT1", NULL, 0); }

/* =========================================================================
 * Readings
 * ====================================================================== */

Voltio_Status voltio_get_settings(Voltio_Context *ctx, float *v, float *i)
{
    if (!ctx || !v || !i) return VOLTIO_ERR_NULL;
    char resp[32] = {0};
    Voltio_Status s = send_command(ctx, "GETS", resp, sizeof(resp));
    if (s != VOLTIO_OK) return s;
    if (strlen(resp) < 6 ||
        !parse_field(resp,     3, VOLT_SCALE,      v) ||
        !parse_field(resp + 3, 3, ctx->curr_scale, i)) {
        set_error(ctx, "GETS: bad response '%s'", resp);
        return VOLTIO_ERR_PROTOCOL;
    }
    return VOLTIO_OK;
}

Voltio_Status voltio_get_display(Voltio_Context *ctx, float *v, float *i, int *cc)
{
    if (!ctx || !v || !i || !cc) return VOLTIO_ERR_NULL;
    char resp[32] = {0};
    Voltio_Status s = send_command(ctx, "GETD", resp, sizeof(resp));
    if (s != VOLTIO_OK) return s;
    if (strlen(resp) < 9 ||
        !parse_field(resp,     4, DISPLAY_SCALE, v) ||
        !parse_field(resp + 4, 4, DISPLAY_SCALE, i) ||
        (resp[8] != '0' && resp[8] != '1')) {
        set_error(ctx, "GETD: bad response '%s'", resp);
        return VOLTIO_ERR_PROTOCOL;
    }
    *cc = (resp[8] == '1') ? 1 : 0;
    return VOLTIO_OK;
}

Voltio_Status voltio_get_status(Voltio_Context *ctx, Voltio_Reading *out)
{
    if (!ctx || !out) return VOLTIO_ERR_NULL;
    Voltio_Status s = voltio_get_settings(ctx, &out->voltage_set, &out->current_set);
    if (s != VOLTIO_OK) return s;
    s = voltio_get_display(ctx, &out->voltage_actual, &out->current_actual, &out->mode_cc);
    if (s != VOLTIO_OK) return s;
    out->power_actual = out->voltage_actual * out->current_actual;
    return VOLTIO_OK;
}

/* =========================================================================
 * Preset memory
 * ====================================================================== */

Voltio_Status voltio_get_presets(Voltio_Context *ctx, Voltio_Preset presets[3])
{
    if (!ctx || !presets) return VOLTIO_ERR_NULL;
    char resp[64] = {0};
    Voltio_Status s = send_command(ctx, "GETM", resp, sizeof(resp));
    if (s != VOLTIO_OK) return s;
    /* Slots arrive as 6-char VVVIII groups — some models put all three on
     * one line, others one per line — so walk 6 chars at a time and skip
     * any newline separators. */
    const char *p = resp;
    for (int slot = 0; slot < 3; slot++) {
        if (strlen(p) < 6 ||
            !parse_field(p,     3, VOLT_SCALE,      &presets[slot].voltage) ||
            !parse_field(p + 3, 3, ctx->curr_scale, &presets[slot].current)) {
            set_error(ctx, "GETM: bad data at slot %d in '%s'", slot + 1, resp);
            return VOLTIO_ERR_PROTOCOL;
        }
        presets[slot].slot = slot + 1;
        p += 6;
        while (*p == '\n') p++;
    }
    return VOLTIO_OK;
}

Voltio_Status voltio_set_presets(Voltio_Context *ctx, const Voltio_Preset presets[3])
{
    if (!ctx || !presets) return VOLTIO_ERR_NULL;
    char ev[3][4], ei[3][4];
    for (int k = 0; k < 3; k++) {
        Voltio_Status s = encode_value(ctx, "preset voltage", presets[k].voltage, VOLT_SCALE, ev[k]);
        if (s != VOLTIO_OK) return s;
        s = encode_value(ctx, "preset current", presets[k].current, ctx->curr_scale, ei[k]);
        if (s != VOLTIO_OK) return s;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "PROM%s%s%s%s%s%s", ev[0],ei[0],ev[1],ei[1],ev[2],ei[2]);
    return send_command(ctx, cmd, NULL, 0);
}

Voltio_Status voltio_recall_preset(Voltio_Context *ctx, int slot)
{
    if (!ctx) return VOLTIO_ERR_NULL;
    if (slot < 1 || slot > 3) { set_error(ctx, "slot must be 1-3, got %d", slot); return VOLTIO_ERR_RANGE; }
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "RUNM%d", slot - 1);
    return send_command(ctx, cmd, NULL, 0);
}

/* =========================================================================
 * Protection limits
 * ====================================================================== */

Voltio_Status voltio_set_ovp(Voltio_Context *ctx, float v) {
    if (!ctx) return VOLTIO_ERR_NULL;
    return set_value(ctx, "SOVP", "OVP", v, VOLT_SCALE);
}
Voltio_Status voltio_set_ocp(Voltio_Context *ctx, float a) {
    if (!ctx) return VOLTIO_ERR_NULL;
    return set_value(ctx, "SOCP", "OCP", a, ctx->curr_scale);
}

Voltio_Status voltio_get_ovp(Voltio_Context *ctx, float *v) {
    if (!ctx || !v) return VOLTIO_ERR_NULL;
    char r[16] = {0};
    Voltio_Status s = send_command(ctx, "GOVP", r, sizeof(r));
    if (s != VOLTIO_OK) return s;
    if (strlen(r) < 3 || !parse_field(r, 3, VOLT_SCALE, v)) {
        set_error(ctx, "GOVP: bad response '%s'", r);
        return VOLTIO_ERR_PROTOCOL;
    }
    return VOLTIO_OK;
}
Voltio_Status voltio_get_ocp(Voltio_Context *ctx, float *a) {
    if (!ctx || !a) return VOLTIO_ERR_NULL;
    char r[16] = {0};
    Voltio_Status s = send_command(ctx, "GOCP", r, sizeof(r));
    if (s != VOLTIO_OK) return s;
    if (strlen(r) < 3 || !parse_field(r, 3, ctx->curr_scale, a)) {
        set_error(ctx, "GOCP: bad response '%s'", r);
        return VOLTIO_ERR_PROTOCOL;
    }
    return VOLTIO_OK;
}

Voltio_Status voltio_get_max(Voltio_Context *ctx, float *v, float *i) {
    if (!ctx || !v || !i) return VOLTIO_ERR_NULL;
    char r[16] = {0};
    Voltio_Status s = send_command(ctx, "GMAX", r, sizeof(r));
    if (s != VOLTIO_OK) return s;
    if (strlen(r) < 6 ||
        !parse_field(r,     3, VOLT_SCALE,      v) ||
        !parse_field(r + 3, 3, ctx->curr_scale, i)) {
        set_error(ctx, "GMAX: bad response '%s'", r);
        return VOLTIO_ERR_PROTOCOL;
    }
    return VOLTIO_OK;
}

/* =========================================================================
 * Utility
 * ====================================================================== */

const char *voltio_status_str(Voltio_Status s) {
    switch (s) {
        case VOLTIO_OK:           return "VOLTIO_OK";
        case VOLTIO_ERR_PORT:     return "VOLTIO_ERR_PORT";
        case VOLTIO_ERR_TIMEOUT:  return "VOLTIO_ERR_TIMEOUT";
        case VOLTIO_ERR_PROTOCOL: return "VOLTIO_ERR_PROTOCOL";
        case VOLTIO_ERR_RANGE:    return "VOLTIO_ERR_RANGE";
        case VOLTIO_ERR_NULL:     return "VOLTIO_ERR_NULL";
        default:                  return "VOLTIO_ERR_UNKNOWN";
    }
}

/* =========================================================================
 * Internal: send_command
 * ====================================================================== */
static Voltio_Status send_command(Voltio_Context *ctx, const char *cmd, char *resp_buf, size_t resp_len)
{
    if (!ctx || !ctx->port) return VOLTIO_ERR_PORT;
    serial_flush_rx(ctx->port);

    char tx[64];
    int  tx_len = snprintf(tx, sizeof(tx), "%s\r", cmd);
    if (serial_write(ctx->port, tx, (size_t)tx_len) != SERIAL_OK) {
        set_error(ctx, "write failed for command '%s'", cmd);
        return VOLTIO_ERR_PORT;
    }

    if (resp_buf && resp_len > 0) resp_buf[0] = '\0';

    char   line[64];
    int    line_pos = 0;
    size_t resp_pos = 0;

    for (int total = 0; total < MAX_RESPONSE_BYTES; total++) {
        char ch;
        Serial_Status ss = serial_read_byte(ctx->port, &ch);
        if (ss == SERIAL_ERR_TIMEOUT) { set_error(ctx, "timeout for '%s'", cmd);     return VOLTIO_ERR_TIMEOUT; }
        if (ss != SERIAL_OK)          { set_error(ctx, "read error for '%s'", cmd);  return VOLTIO_ERR_TIMEOUT; }

        if (ch == '\r' || ch == '\n') {
            if (line_pos == 0) continue;
            line[line_pos] = '\0'; line_pos = 0;

            if (strcmp(line, "OK") == 0) return VOLTIO_OK;

            if (resp_buf && resp_len > 1) {
                if (resp_pos > 0 && resp_pos < resp_len - 1) resp_buf[resp_pos++] = '\n';
                size_t copy = strlen(line);
                if (resp_pos + copy >= resp_len - 1) copy = resp_len - 1 - resp_pos;
                memcpy(resp_buf + resp_pos, line, copy);
                resp_pos += copy;
                resp_buf[resp_pos] = '\0';
            }
        } else {
            if (line_pos < (int)(sizeof(line) - 1)) line[line_pos++] = ch;
        }
    }
    set_error(ctx, "no OK within %d bytes for '%s'", MAX_RESPONSE_BYTES, cmd);
    return VOLTIO_ERR_PROTOCOL;
}

/* =========================================================================
 * Internal: encode / decode / error
 * ====================================================================== */

/* Encode val as 3 ASCII digits at the given scale. Out-of-range values
 * (negative, NaN, or above 999/scale) are rejected rather than clamped. */
static Voltio_Status encode_value(Voltio_Context *ctx, const char *what, float val, int scale, char out[4]) {
    float max = 999.0f / (float)scale;
    if (!(val >= 0.0f) || val * (float)scale + 0.5f >= 1000.0f) {
        set_error(ctx, "%s %g out of range (0 to %g)", what, (double)val, (double)max);
        return VOLTIO_ERR_RANGE;
    }
    snprintf(out, 4, "%03d", (int)(val * (float)scale + 0.5f));
    return VOLTIO_OK;
}

static Voltio_Status set_value(Voltio_Context *ctx, const char *prefix, const char *what, float val, int scale) {
    char enc[4], cmd[16];
    Voltio_Status s = encode_value(ctx, what, val, scale, enc);
    if (s != VOLTIO_OK) return s;
    snprintf(cmd, sizeof(cmd), "%s%s", prefix, enc);
    return send_command(ctx, cmd, NULL, 0);
}

/* Parse exactly `digits` ASCII digits from s. Returns 1 on success. */
static int parse_field(const char *s, int digits, int scale, float *out) {
    int raw = 0;
    for (int k = 0; k < digits; k++) {
        if (s[k] < '0' || s[k] > '9') return 0;
        raw = raw * 10 + (s[k] - '0');
    }
    *out = (float)raw / (float)scale;
    return 1;
}

static void set_error(Voltio_Context *ctx, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(ctx->last_error, sizeof(ctx->last_error), fmt, ap); va_end(ap);
}
