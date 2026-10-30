/**
 * voltio_cli.c  —  Command-line interface for Voltio
 *
 * Usage:
 *   voltio --port COM4 status
 *   voltio --port COM4 set --voltage 12.0 --current 2.5
 *   voltio --port COM4 output on
 *   voltio --port COM4 output off
 *   voltio --port COM4 preset list
 *   voltio --port COM4 preset recall 2
 *   voltio --port COM4 preset write 1 12.0 2.5
 *   voltio --port COM4 limits [--ovp 15.0] [--ocp 5.0]
 *   voltio --port COM4 max
 *   voltio --port COM4 --dp 2 status      (1685B: current has 2 d.p.)
 *
 * macOS port example:  --port /dev/cu.usbserial-2130
 * All output is JSON.
 */

#include "voltio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Minimal JSON helpers
 * ---------------------------------------------------------------------- */
static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if      (c == '"')  fputs("\\\"", f);
        else if (c == '\\') fputs("\\\\", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20)  fprintf(f, "\\u%04x", c);
        else                fputc(c, f);
    }
    fputc('"', f);
}

static void json_ok_begin(void)              { printf("{\n  \"status\": \"ok\""); }
static void json_end(void)                   { printf("\n}\n"); }
static void json_kv_f(const char *k, float v)       { printf(",\n  \"%s\": %.3f", k, v); }
static void json_kv_s(const char *k, const char *v) { printf(",\n  \"%s\": ", k); json_str(stdout, v); }
static void json_kv_i(const char *k, int v)         { printf(",\n  \"%s\": %d", k, v); }

static void json_error(const char *msg)
{
    fputs("{\n  \"status\": \"error\",\n  \"message\": ", stderr);
    json_str(stderr, msg);
    fputs("\n}\n", stderr);
}

/* Report the driver's last error and return the exit code. */
static int fail(Voltio_Context *ctx) { json_error(voltio_last_error(ctx)); return 1; }

/* Parse a float argument; rejects empty strings and trailing garbage. */
static int parse_float(const char *s, float *out)
{
    char *end;
    *out = strtof(s, &end);
    return end != s && *end == '\0';
}

static int parse_int(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return 0;
    *out = (int)v;
    return 1;
}

static int bad_number(const char *what, const char *s)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "invalid %s '%s'", what, s);
    json_error(msg);
    return 1;
}

/* -------------------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------------- */

static int cmd_status(Voltio_Context *ctx)
{
    Voltio_Reading r;
    if (voltio_get_status(ctx, &r) != VOLTIO_OK) return fail(ctx);
    json_ok_begin();
    json_kv_f("voltage_set_V",    r.voltage_set);
    json_kv_f("current_set_A",    r.current_set);
    json_kv_f("voltage_actual_V", r.voltage_actual);
    json_kv_f("current_actual_A", r.current_actual);
    json_kv_f("power_actual_W",   r.power_actual);
    json_kv_s("mode",             r.mode_cc ? "CC" : "CV");
    json_end();
    return 0;
}

static int cmd_set(Voltio_Context *ctx, int argc, char **argv)
{
    float voltage = 0.0f, current = 0.0f;
    int   has_v = 0, has_i = 0;
    for (int i = 0; i < argc; i++) {
        int is_v = strcmp(argv[i], "--voltage") == 0 || strcmp(argv[i], "-v") == 0;
        int is_i = strcmp(argv[i], "--current") == 0 || strcmp(argv[i], "-i") == 0;
        if (!is_v && !is_i) continue;
        if (i + 1 >= argc) { json_error("missing value after --voltage/--current"); return 1; }
        const char *val = argv[++i];
        if (is_v) { if (!parse_float(val, &voltage)) return bad_number("voltage", val); has_v = 1; }
        else      { if (!parse_float(val, &current)) return bad_number("current", val); has_i = 1; }
    }
    if (!has_v && !has_i) { json_error("set requires --voltage and/or --current"); return 1; }
    if (has_v && voltio_set_voltage(ctx, voltage) != VOLTIO_OK) return fail(ctx);
    if (has_i && voltio_set_current(ctx, current) != VOLTIO_OK) return fail(ctx);
    float v = 0, i = 0;
    if (voltio_get_settings(ctx, &v, &i) != VOLTIO_OK) return fail(ctx);
    json_ok_begin(); json_kv_f("voltage_set_V", v); json_kv_f("current_set_A", i); json_end();
    return 0;
}

static int cmd_output(Voltio_Context *ctx, int argc, char **argv)
{
    if (argc < 1) { json_error("output requires 'on' or 'off'"); return 1; }
    const char *state = argv[0];
    int is_on  = (strcmp(state, "on")  == 0 || strcmp(state, "1") == 0);
    int is_off = (strcmp(state, "off") == 0 || strcmp(state, "0") == 0);
    if (!is_on && !is_off) { json_error("output state must be 'on' or 'off'"); return 1; }
    Voltio_Status s = is_on ? voltio_output_on(ctx) : voltio_output_off(ctx);
    if (s != VOLTIO_OK) return fail(ctx);
    json_ok_begin(); json_kv_s("output", is_on ? "on" : "off"); json_end();
    return 0;
}

static int cmd_preset(Voltio_Context *ctx, int argc, char **argv)
{
    if (argc < 1) { json_error("preset requires: list | recall <slot> | write <slot> <V> <A>"); return 1; }
    const char *action = argv[0];

    if (strcmp(action, "list") == 0) {
        Voltio_Preset p[3];
        if (voltio_get_presets(ctx, p) != VOLTIO_OK) return fail(ctx);
        printf("{\n  \"status\": \"ok\",\n  \"presets\": [\n");
        for (int k = 0; k < 3; k++)
            printf("    { \"slot\": %d, \"voltage_V\": %.3f, \"current_A\": %.3f }%s\n",
                   p[k].slot, p[k].voltage, p[k].current, k < 2 ? "," : "");
        printf("  ]\n}\n");
        return 0;
    }

    if (strcmp(action, "recall") == 0) {
        if (argc < 2) { json_error("preset recall requires slot number"); return 1; }
        int slot;
        if (!parse_int(argv[1], &slot)) return bad_number("slot", argv[1]);
        if (voltio_recall_preset(ctx, slot) != VOLTIO_OK) return fail(ctx);
        float v = 0, i = 0;
        if (voltio_get_settings(ctx, &v, &i) != VOLTIO_OK) return fail(ctx);
        json_ok_begin(); json_kv_i("recalled_slot", slot); json_kv_f("voltage_set_V", v); json_kv_f("current_set_A", i); json_end();
        return 0;
    }

    if (strcmp(action, "write") == 0) {
        if (argc < 4) { json_error("preset write requires: <slot> <voltage> <current>"); return 1; }
        int   slot;
        float volts, amps;
        if (!parse_int(argv[1], &slot))    return bad_number("slot", argv[1]);
        if (!parse_float(argv[2], &volts)) return bad_number("voltage", argv[2]);
        if (!parse_float(argv[3], &amps))  return bad_number("current", argv[3]);
        if (slot < 1 || slot > 3) { json_error("slot must be 1, 2, or 3"); return 1; }
        Voltio_Preset p[3];
        if (voltio_get_presets(ctx, p) != VOLTIO_OK) return fail(ctx);
        p[slot-1].voltage = volts; p[slot-1].current = amps;
        if (voltio_set_presets(ctx, p) != VOLTIO_OK) return fail(ctx);
        json_ok_begin(); json_kv_i("slot", slot); json_kv_f("voltage_V", volts); json_kv_f("current_A", amps); json_end();
        return 0;
    }

    json_error("unknown preset action — use: list | recall | write");
    return 1;
}

static int cmd_limits(Voltio_Context *ctx, int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        int is_ovp = strcmp(argv[i], "--ovp") == 0;
        int is_ocp = strcmp(argv[i], "--ocp") == 0;
        if (!is_ovp && !is_ocp) continue;
        if (i + 1 >= argc) { json_error("missing value after --ovp/--ocp"); return 1; }
        const char *val = argv[++i];
        float x;
        if (!parse_float(val, &x)) return bad_number(is_ovp ? "ovp" : "ocp", val);
        Voltio_Status s = is_ovp ? voltio_set_ovp(ctx, x) : voltio_set_ocp(ctx, x);
        if (s != VOLTIO_OK) return fail(ctx);
    }
    float ovp = 0, ocp = 0;
    if (voltio_get_ovp(ctx, &ovp) != VOLTIO_OK) return fail(ctx);
    if (voltio_get_ocp(ctx, &ocp) != VOLTIO_OK) return fail(ctx);
    json_ok_begin(); json_kv_f("ovp_V", ovp); json_kv_f("ocp_A", ocp); json_end();
    return 0;
}

static int cmd_max(Voltio_Context *ctx)
{
    float v = 0, i = 0;
    if (voltio_get_max(ctx, &v, &i) != VOLTIO_OK) return fail(ctx);
    json_ok_begin(); json_kv_f("max_voltage_V", v); json_kv_f("max_current_A", i); json_end();
    return 0;
}

/* -------------------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------------- */
static void print_usage(void)
{
    fprintf(stderr,
        "Voltio — BK Precision 1900B-Series power supply control\n"
        "\n"
        "Usage: voltio --port <port> [--dp 1|2] <command> [options]\n"
        "\n"
        "  macOS port example:   --port /dev/cu.usbserial-2130\n"
        "  Windows port example: --port COM4\n"
        "  --dp                  Current decimal places: 2 for 1685B, 1 otherwise (default 1)\n"
        "\n"
        "Commands:\n"
        "  status                           Read live voltage, current, power, mode\n"
        "  set [--voltage V] [--current A]  Set output voltage and/or current\n"
        "  output <on|off>                  Enable or disable output\n"
        "  preset list                      List all 3 preset memory slots\n"
        "  preset recall <slot>             Apply preset (slot = 1, 2, 3)\n"
        "  preset write <slot> <V> <A>      Write a preset slot\n"
        "  limits [--ovp V] [--ocp A]       Get or set protection limits\n"
        "  max                              Read hardware maximum V and A\n"
        "\n"
        "All output is JSON.\n"
    );
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    const char *port = NULL;
    int dp = 1;
    int cmd_idx = 1;

    /* Global options come before the command. */
    while (cmd_idx < argc && strncmp(argv[cmd_idx], "--", 2) == 0) {
        if (cmd_idx + 1 >= argc) { print_usage(); return 1; }
        if      (strcmp(argv[cmd_idx], "--port") == 0) port = argv[cmd_idx + 1];
        else if (strcmp(argv[cmd_idx], "--dp")   == 0) {
            if (!parse_int(argv[cmd_idx + 1], &dp) || (dp != 1 && dp != 2)) {
                json_error("--dp must be 1 or 2");
                return 1;
            }
        }
        else { print_usage(); return 1; }
        cmd_idx += 2;
    }

    if (!port || cmd_idx >= argc) { print_usage(); return 1; }

    Voltio_Context *ctx = voltio_open(port, dp);
    if (!ctx || voltio_last_error(ctx)[0] != '\0') {
        char msg[300];
        snprintf(msg, sizeof(msg), "Cannot open %s: %s",
                 port, ctx ? voltio_last_error(ctx) : "out of memory");
        json_error(msg);
        voltio_close(ctx);
        return 1;
    }

    const char *cmd  = argv[cmd_idx];
    int         sargc = argc - cmd_idx - 1;
    char      **sargv = argv + cmd_idx + 1;
    int ret = 0;

    if      (strcmp(cmd, "status") == 0) ret = cmd_status(ctx);
    else if (strcmp(cmd, "set")    == 0) ret = cmd_set(ctx, sargc, sargv);
    else if (strcmp(cmd, "output") == 0) ret = cmd_output(ctx, sargc, sargv);
    else if (strcmp(cmd, "preset") == 0) ret = cmd_preset(ctx, sargc, sargv);
    else if (strcmp(cmd, "limits") == 0) ret = cmd_limits(ctx, sargc, sargv);
    else if (strcmp(cmd, "max")    == 0) ret = cmd_max(ctx);
    else { json_error("Unknown command"); print_usage(); ret = 1; }

    voltio_close(ctx);
    return ret;
}
