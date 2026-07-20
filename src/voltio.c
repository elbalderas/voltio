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

/* -------------------------------------------------------------------------
 * Internal context
 * ---------------------------------------------------------------------- */
struct Voltio_Context {
    Serial_Port *port;
    int          dp;
    int          scale;
    char         last_error[256];
};

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */
static Voltio_Status send_command  (Voltio_Context *ctx, const char *cmd, char *resp_buf, size_t resp_len);
static void          encode_value  (const Voltio_Context *ctx, float val, char out[4]);
static float         decode_3      (const Voltio_Context *ctx, const char *s);
static float         decode_4      (const char *s);
static void          set_error     (Voltio_Context *ctx, const char *fmt, ...);

/* =========================================================================
 * Lifecycle
 * ====================================================================== */

Voltio_Context *voltio_open(const char *port, int dp)
{
    if (!port) return NULL;
    Voltio_Context *ctx = (Voltio_Context *)calloc(1, sizeof(Voltio_Context));
    if (!ctx) return NULL;
    ctx->dp    = (dp == 2) ? 2 : 1;
    ctx->scale = (dp == 2) ? 100 : 10;
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
    char enc[4], cmd[16];
    encode_value(ctx, volts, enc);
    snprintf(cmd, sizeof(cmd), "VOLT%s", enc);
    return send_command(ctx, cmd, NULL, 0);
}

Voltio_Status voltio_set_current(Voltio_Context *ctx, float amps)
{
    if (!ctx) return VOLTIO_ERR_NULL;
    char enc[4], cmd[16];
    encode_value(ctx, amps, enc);
    snprintf(cmd, sizeof(cmd), "CURR%s", enc);
    return send_command(ctx, cmd, NULL, 0);
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
    if (strlen(resp) < 6) { set_error(ctx, "GETS: short response '%s'", resp); return VOLTIO_ERR_PROTOCOL; }
    char sv[4] = {resp[0],resp[1],resp[2],'\0'}, si[4] = {resp[3],resp[4],resp[5],'\0'};
    *v = decode_3(ctx, sv); *i = decode_3(ctx, si);
    return VOLTIO_OK;
}

Voltio_Status voltio_get_display(Voltio_Context *ctx, float *v, float *i, int *cc)
{
    if (!ctx || !v || !i || !cc) return VOLTIO_ERR_NULL;
    char resp[32] = {0};
    Voltio_Status s = send_command(ctx, "GETD", resp, sizeof(resp));
    if (s != VOLTIO_OK) return s;
    if (strlen(resp) < 9) { set_error(ctx, "GETD: short response '%s'", resp); return VOLTIO_ERR_PROTOCOL; }
    char sv[5] = {resp[0],resp[1],resp[2],resp[3],'\0'}, si[5] = {resp[4],resp[5],resp[6],resp[7],'\0'};
    *v = decode_4(sv); *i = decode_4(si); *cc = (resp[8] == '1') ? 1 : 0;
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
        if (strlen(p) < 6) { set_error(ctx, "GETM: incomplete at slot %d", slot+1); return VOLTIO_ERR_PROTOCOL; }
        char v[4] = {p[0],p[1],p[2],'\0'}, i[4] = {p[3],p[4],p[5],'\0'};
        presets[slot].slot    = slot + 1;
        presets[slot].voltage = decode_3(ctx, v);
        presets[slot].current = decode_3(ctx, i);
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
        encode_value(ctx, presets[k].voltage, ev[k]);
        encode_value(ctx, presets[k].current, ei[k]);
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
    char e[4], c[16];
    encode_value(ctx, v, e);
    snprintf(c, sizeof(c), "SOVP%s", e);
    return send_command(ctx, c, NULL, 0);
}
Voltio_Status voltio_set_ocp(Voltio_Context *ctx, float a) {
    if (!ctx) return VOLTIO_ERR_NULL;
    char e[4], c[16];
    encode_value(ctx, a, e);
    snprintf(c, sizeof(c), "SOCP%s", e);
    return send_command(ctx, c, NULL, 0);
}

Voltio_Status voltio_get_ovp(Voltio_Context *ctx, float *v) {
    if (!ctx || !v) return VOLTIO_ERR_NULL;
    char r[16] = {0};
    Voltio_Status s = send_command(ctx, "GOVP", r, sizeof(r));
    if (s != VOLTIO_OK) return s;
    if (strlen(r) < 3) return VOLTIO_ERR_PROTOCOL;
    *v = decode_3(ctx, r); return VOLTIO_OK;
}
Voltio_Status voltio_get_ocp(Voltio_Context *ctx, float *a) {
    if (!ctx || !a) return VOLTIO_ERR_NULL;
    char r[16] = {0};
    Voltio_Status s = send_command(ctx, "GOCP", r, sizeof(r));
    if (s != VOLTIO_OK) return s;
    if (strlen(r) < 3) return VOLTIO_ERR_PROTOCOL;
    *a = decode_3(ctx, r); return VOLTIO_OK;
}

Voltio_Status voltio_get_max(Voltio_Context *ctx, float *v, float *i) {
    if (!ctx || !v || !i) return VOLTIO_ERR_NULL;
    char r[16] = {0};
    Voltio_Status s = send_command(ctx, "GMAX", r, sizeof(r));
    if (s != VOLTIO_OK) return s;
    if (strlen(r) < 6) return VOLTIO_ERR_PROTOCOL;
    char sv[4] = {r[0],r[1],r[2],'\0'}, si[4] = {r[3],r[4],r[5],'\0'};
    *v = decode_3(ctx, sv); *i = decode_3(ctx, si); return VOLTIO_OK;
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
    int    found_ok = 0;

    for (;;) {
        char ch;
        Serial_Status ss = serial_read_byte(ctx->port, &ch);
        if (ss == SERIAL_ERR_TIMEOUT) { set_error(ctx, "timeout for '%s'", cmd);     return VOLTIO_ERR_TIMEOUT; }
        if (ss != SERIAL_OK)          { set_error(ctx, "read error for '%s'", cmd);  return VOLTIO_ERR_TIMEOUT; }

        if (ch == '\r' || ch == '\n') {
            if (line_pos == 0) continue;
            line[line_pos] = '\0'; line_pos = 0;

            if (strcmp(line, "OK") == 0) { found_ok = 1; break; }

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
    return found_ok ? VOLTIO_OK : VOLTIO_ERR_TIMEOUT;
}

/* =========================================================================
 * Internal: encode / decode / error
 * ====================================================================== */
static void encode_value(const Voltio_Context *ctx, float val, char out[4]) {
    int raw = (int)(val * ctx->scale + 0.5f);
    if (raw < 0)   raw = 0;
    if (raw > 999) raw = 999;
    snprintf(out, 4, "%03d", raw);
}
static float decode_3(const Voltio_Context *ctx, const char *s) { return (float)atoi(s) / (float)ctx->scale; }
static float decode_4(const char *s)                             { return (float)atoi(s) / 100.0f; }
static void  set_error(Voltio_Context *ctx, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(ctx->last_error, sizeof(ctx->last_error), fmt, ap); va_end(ap);
}
