/**
 * voltio.h  —  BK Precision 1900B-Series public API
 *
 * Supports: 1685B, 1687B, 1688B, 1900B, 1901B, 1902B
 *
 * Value encoding (3 ASCII digits on the wire):
 *   Voltage is always 1 decimal place   ->  12.5 V = "125"
 *   Current is 1 decimal place on most models (2.5 A = "025") and
 *   2 decimal places on the 1685B (2.50 A = "250") — see `dp` below.
 *   All encode/decode is handled internally; callers use plain floats.
 *   Values that don't fit in 3 digits return VOLTIO_ERR_RANGE.
 */

#ifndef VOLTIO_H
#define VOLTIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Opaque handle
 * ---------------------------------------------------------------------- */
typedef struct Voltio_Context Voltio_Context;

/* -------------------------------------------------------------------------
 * Return codes
 * ---------------------------------------------------------------------- */
typedef enum {
    VOLTIO_OK             =  0,
    VOLTIO_ERR_PORT       = -1,
    VOLTIO_ERR_TIMEOUT    = -2,
    VOLTIO_ERR_PROTOCOL   = -3,
    VOLTIO_ERR_RANGE      = -4,
    VOLTIO_ERR_NULL       = -5,
} Voltio_Status;

/* -------------------------------------------------------------------------
 * Status snapshot
 * ---------------------------------------------------------------------- */
typedef struct {
    float voltage_set;
    float current_set;
    float voltage_actual;
    float current_actual;
    float power_actual;
    int   mode_cc;        /* 0 = CV,  1 = CC */
} Voltio_Reading;

/* -------------------------------------------------------------------------
 * Preset memory slot (1, 2, or 3)
 * ---------------------------------------------------------------------- */
typedef struct {
    int   slot;
    float voltage;
    float current;
} Voltio_Preset;

/* -------------------------------------------------------------------------
 * Lifecycle
 *
 * voltio_open() always returns a context unless out of memory; check
 * voltio_last_error(ctx)[0] != '\0' to detect a failed open.
 *
 * @param dp  Decimal places for current values: 2 for the 1685B,
 *            1 for every other model. Voltage is always 1 decimal place.
 * ---------------------------------------------------------------------- */
Voltio_Context *voltio_open (const char *port, int dp);
void            voltio_close(Voltio_Context *ctx);
const char     *voltio_last_error(const Voltio_Context *ctx);

/* -------------------------------------------------------------------------
 * Output control
 * ---------------------------------------------------------------------- */
Voltio_Status voltio_set_voltage(Voltio_Context *ctx, float volts);
Voltio_Status voltio_set_current(Voltio_Context *ctx, float amps);
Voltio_Status voltio_output_on  (Voltio_Context *ctx);
Voltio_Status voltio_output_off (Voltio_Context *ctx);

/* -------------------------------------------------------------------------
 * Readings
 * ---------------------------------------------------------------------- */
Voltio_Status voltio_get_settings(Voltio_Context *ctx, float *volts_out, float *amps_out);
Voltio_Status voltio_get_display (Voltio_Context *ctx, float *volts_out, float *amps_out, int *cc_out);
Voltio_Status voltio_get_status  (Voltio_Context *ctx, Voltio_Reading *out);

/* -------------------------------------------------------------------------
 * Preset memory
 * ---------------------------------------------------------------------- */
Voltio_Status voltio_get_presets  (Voltio_Context *ctx, Voltio_Preset presets[3]);
Voltio_Status voltio_set_presets  (Voltio_Context *ctx, const Voltio_Preset presets[3]);
Voltio_Status voltio_recall_preset(Voltio_Context *ctx, int slot);

/* -------------------------------------------------------------------------
 * Protection limits
 * ---------------------------------------------------------------------- */
Voltio_Status voltio_set_ovp(Voltio_Context *ctx, float volts);
Voltio_Status voltio_set_ocp(Voltio_Context *ctx, float amps);
Voltio_Status voltio_get_ovp(Voltio_Context *ctx, float *volts_out);
Voltio_Status voltio_get_ocp(Voltio_Context *ctx, float *amps_out);
Voltio_Status voltio_get_max(Voltio_Context *ctx, float *volts_out, float *amps_out);

/* -------------------------------------------------------------------------
 * Utility
 * ---------------------------------------------------------------------- */
const char *voltio_status_str(Voltio_Status s);

#ifdef __cplusplus
}
#endif
#endif /* VOLTIO_H */
