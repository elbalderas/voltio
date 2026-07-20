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
static void json_ok_begin(void)              { printf("{\n  \"status\": \"ok\""); }
static void json_end(void)                   { printf("\n}\n"); }
static void json_kv_f(const char *k, float v)       { printf(",\n  \"%s\": %.3f", k, v); }
static void json_kv_s(const char *k, const char *v) { printf(",\n  \"%s\": \"%s\"", k, v); }
static void json_kv_i(const char *k, int v)         { printf(",\n  \"%s\": %d", k, v); }

static void json_error(const char *msg)
{
    fprintf(stderr, "{\n  \"status\": \"error\",\n  \"message\": \"%s\"\n}\n", msg);
}

/* -------------------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------------- */

static int cmd_status(Voltio_Context *ctx)
{
    Voltio_Reading r;
    if (voltio_get_status(ctx, &r) != VOLTIO_OK) { json_error(voltio_last_error(ctx)); return 1; }
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
    float voltage = -1.0f, current = -1.0f;
    for (int i = 0; i < argc - 1; i++) {
        if      (strcmp(argv[i], "--voltage") == 0 || strcmp(argv[i], "-v") == 0) voltage = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--current") == 0 || strcmp(argv[i], "-i") == 0) current = (float)atof(argv[++i]);
    }
    if (voltage < 0.0f && current < 0.0f) { json_error("set requires --voltage and/or --current"); return 1; }
    if (voltage >= 0.0f && voltio_set_voltage(ctx, voltage) != VOLTIO_OK) { json_error(voltio_last_error(ctx)); return 1; }
    if (current >= 0.0f && voltio_set_current(ctx, current) != VOLTIO_OK) { json_error(voltio_last_error(ctx)); return 1; }
    float v = 0, i = 0;
    voltio_get_settings(ctx, &v, &i);
    json_ok_begin(); json_kv_f("voltage_set_V", v); json_kv_f("current_set_A", i); json_end();
    return 0;
}

static int cmd_output(Voltio_Context *ctx, const char *state)
{
    int is_on = (strcmp(state, "on") == 0 || strcmp(state, "1") == 0);
    int is_off = (strcmp(state, "off") == 0 || strcmp(state, "0") == 0);
    if (!is_on && !is_off) { json_error("output state must be 'on' or 'off'"); return 1; }
    Voltio_Status s = is_on ? voltio_output_on(ctx) : voltio_output_off(ctx);
    if (s != VOLTIO_OK) { json_error(voltio_last_error(ctx)); return 1; }
    json_ok_begin(); json_kv_s("output", is_on ? "on" : "off"); json_end();
    return 0;
}

static int cmd_preset(Voltio_Context *ctx, int argc, char **argv)
{
    if (argc < 1) { json_error("preset requires: list | recall <slot> | write <slot> <V> <A>"); return 1; }
    const char *action = argv[0];

    if (strcmp(action, "list") == 0) {
        Voltio_Preset p[3];
        if (voltio_get_presets(ctx, p) != VOLTIO_OK) { json_error(voltio_last_error(ctx)); return 1; }
        printf("{\n  \"status\": \"ok\",\n  \"presets\": [\n");
        for (int k = 0; k < 3; k++)
            printf("    { \"slot\": %d, \"voltage_V\": %.1f, \"current_A\": %.1f }%s\n",
                   p[k].slot, p[k].voltage, p[k].current, k < 2 ? "," : "");
        printf("  ]\n}\n");
        return 0;
    }

    if (strcmp(action, "recall") == 0) {
        if (argc < 2) { json_error("preset recall requires slot number"); return 1; }
        int slot = atoi(argv[1]);
        if (voltio_recall_preset(ctx, slot) != VOLTIO_OK) { json_error(voltio_last_error(ctx)); return 1; }
        float v = 0, i = 0; voltio_get_settings(ctx, &v, &i);
        json_ok_begin(); json_kv_i("recalled_slot", slot); json_kv_f("voltage_set_V", v); json_kv_f("current_set_A", i); json_end();
        return 0;
    }

    if (strcmp(action, "write") == 0) {
        if (argc < 4) { json_error("preset write requires: <slot> <voltage> <current>"); return 1; }
        int   slot  = atoi(argv[1]);
        float volts = (float)atof(argv[2]);
        float amps  = (float)atof(argv[3]);
        Voltio_Preset p[3];
        if (voltio_get_presets(ctx, p) != VOLTIO_OK) { json_error(voltio_last_error(ctx)); return 1; }
        if (slot < 1 || slot > 3) { json_error("slot must be 1, 2, or 3"); return 1; }
        p[slot-1].voltage = volts; p[slot-1].current = amps;
        if (voltio_set_presets(ctx, p) != VOLTIO_OK) { json_error(voltio_last_error(ctx)); return 1; }
        json_ok_begin(); json_kv_i("slot", slot); json_kv_f("voltage_V", volts); json_kv_f("current_A", amps); json_end();
        return 0;
    }

    json_error("unknown preset action — use: list | recall | write");
    return 1;
}

static int cmd_limits(Voltio_Context *ctx, int argc, char **argv)
{
    for (int i = 0; i < argc - 1; i++) {
        if      (strcmp(argv[i], "--ovp") == 0) voltio_set_ovp(ctx, (float)atof(argv[++i]));
        else if (strcmp(argv[i], "--ocp") == 0) voltio_set_ocp(ctx, (float)atof(argv[++i]));
    }
    float ovp = 0, ocp = 0;
    voltio_get_ovp(ctx, &ovp); voltio_get_ocp(ctx, &ocp);
    json_ok_begin(); json_kv_f("ovp_V", ovp); json_kv_f("ocp_A", ocp); json_end();
    return 0;
}

static int cmd_max(Voltio_Context *ctx)
{
    float v = 0, i = 0;
    if (voltio_get_max(ctx, &v, &i) != VOLTIO_OK) { json_error(voltio_last_error(ctx)); return 1; }
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
        "Usage: voltio --port <port> <command> [options]\n"
        "\n"
        "  macOS port example:   --port /dev/cu.usbserial-2130\n"
        "  Windows port example: --port COM4\n"
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
    int cmd_idx = 1;

    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--port") == 0) {
            port    = argv[i + 1];
            cmd_idx = i + 2;
            break;
        }
    }

    if (!port || cmd_idx >= argc) { print_usage(); return 1; }

    Voltio_Context *ctx = voltio_open(port, 1);
    if (!ctx || voltio_last_error(ctx)[0] != '\0') {
        char msg[300];
        snprintf(msg, sizeof(msg), "Cannot open %s: %s",
                 port, ctx ? voltio_last_error(ctx) : "out of memory");
        json_error(msg);
        voltio_close(ctx);
        return 1;
    }

    const char *cmd = argv[cmd_idx];
    int ret = 0;

    if      (strcmp(cmd, "status") == 0)                      ret = cmd_status(ctx);
    else if (strcmp(cmd, "set")    == 0)                      ret = cmd_set(ctx, argc - cmd_idx - 1, argv + cmd_idx + 1);
    else if (strcmp(cmd, "output") == 0 && cmd_idx+1 < argc)  ret = cmd_output(ctx, argv[cmd_idx + 1]);
    else if (strcmp(cmd, "preset") == 0)                      ret = cmd_preset(ctx, argc - cmd_idx - 1, argv + cmd_idx + 1);
    else if (strcmp(cmd, "limits") == 0)                      ret = cmd_limits(ctx, argc - cmd_idx - 1, argv + cmd_idx + 1);
    else if (strcmp(cmd, "max")    == 0)                      ret = cmd_max(ctx);
    else { json_error("Unknown command"); print_usage(); ret = 1; }

    voltio_close(ctx);
    return ret;
}
