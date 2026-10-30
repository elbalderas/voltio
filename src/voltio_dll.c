/**
 * voltio_dll.c  —  DLL export layer for CANoe loadDll()
 *
 * All exports use __declspec(dllexport) __cdecl — required by CANoe.
 * Numeric arguments are passed as long or double by callDll().
 *
 * Example CAPL usage:
 *   variables { long hVoltio; }
 *   on start {
 *     hVoltio = loadDll("voltio.dll");
 *     callDll(hVoltio, "Voltio_Open", "COM4");      // or Voltio_OpenDp("COM4", 2) for 1685B
 *     callDll(hVoltio, "Voltio_SetVoltage", 12.5);
 *     callDll(hVoltio, "Voltio_OutputOn");
 *   }
 *   on stopMeasurement {
 *     callDll(hVoltio, "Voltio_Close");
 *     unloadDll(hVoltio);
 *   }
 */

#include "voltio.h"
#include <windows.h>

#define EXPORT __declspec(dllexport) __cdecl

static Voltio_Context *g_ctx = NULL;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/** Open COM port. dp = current decimal places: 2 for 1685B, 1 otherwise.
 *  Returns 1 on success, 0 on failure. */
EXPORT long Voltio_OpenDp(const char *port, long dp)
{
    if (g_ctx) { voltio_close(g_ctx); g_ctx = NULL; }
    g_ctx = voltio_open(port, (int)dp);
    return (g_ctx && voltio_last_error(g_ctx)[0] == '\0') ? 1 : 0;
}

/** Open COM port with 1 current decimal place (all models except 1685B). */
EXPORT long Voltio_Open(const char *port)
{
    return Voltio_OpenDp(port, 1);
}

EXPORT void Voltio_Close(void)
{
    voltio_close(g_ctx);
    g_ctx = NULL;
}

/* -------------------------------------------------------------------------
 * Output control — returns 1 on OK, 0 on error
 * ---------------------------------------------------------------------- */

EXPORT long Voltio_SetVoltage(double volts)
{
    if (!g_ctx) return 0;
    return voltio_set_voltage(g_ctx, (float)volts) == VOLTIO_OK ? 1 : 0;
}

EXPORT long Voltio_SetCurrent(double amps)
{
    if (!g_ctx) return 0;
    return voltio_set_current(g_ctx, (float)amps) == VOLTIO_OK ? 1 : 0;
}

EXPORT long Voltio_SetVoltageCurrent(double volts, double amps)
{
    if (!g_ctx) return 0;
    if (voltio_set_voltage(g_ctx, (float)volts) != VOLTIO_OK) return 0;
    if (voltio_set_current(g_ctx, (float)amps)  != VOLTIO_OK) return 0;
    return 1;
}

EXPORT long Voltio_OutputOn(void)
{
    if (!g_ctx) return 0;
    return voltio_output_on(g_ctx) == VOLTIO_OK ? 1 : 0;
}

EXPORT long Voltio_OutputOff(void)
{
    if (!g_ctx) return 0;
    return voltio_output_off(g_ctx) == VOLTIO_OK ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * Scalar getters — returns value on success, -1.0 on error
 * ---------------------------------------------------------------------- */

EXPORT double Voltio_GetVoltageSet(void)
{
    if (!g_ctx) return -1.0;
    float v, i;
    return voltio_get_settings(g_ctx, &v, &i) == VOLTIO_OK ? (double)v : -1.0;
}

EXPORT double Voltio_GetCurrentSet(void)
{
    if (!g_ctx) return -1.0;
    float v, i;
    return voltio_get_settings(g_ctx, &v, &i) == VOLTIO_OK ? (double)i : -1.0;
}

EXPORT double Voltio_GetVoltageActual(void)
{
    if (!g_ctx) return -1.0;
    float v, i; int cc;
    return voltio_get_display(g_ctx, &v, &i, &cc) == VOLTIO_OK ? (double)v : -1.0;
}

EXPORT double Voltio_GetCurrentActual(void)
{
    if (!g_ctx) return -1.0;
    float v, i; int cc;
    return voltio_get_display(g_ctx, &v, &i, &cc) == VOLTIO_OK ? (double)i : -1.0;
}

EXPORT double Voltio_GetPowerActual(void)
{
    if (!g_ctx) return -1.0;
    float v, i; int cc;
    if (voltio_get_display(g_ctx, &v, &i, &cc) != VOLTIO_OK) return -1.0;
    return (double)(v * i);
}

/** Returns 0=CV, 1=CC, -1=error */
EXPORT long Voltio_GetMode(void)
{
    if (!g_ctx) return -1;
    float v, i; int cc;
    return voltio_get_display(g_ctx, &v, &i, &cc) == VOLTIO_OK ? cc : -1;
}

/* -------------------------------------------------------------------------
 * Display snapshot
 *
 * Each Voltio_Get*Actual() call above sends its own query, so separate
 * calls can return values from different moments. Voltio_ReadDisplay()
 * queries once; the Voltio_Display*() getters then return values from
 * that one sample without touching the port.
 * ---------------------------------------------------------------------- */

static float g_disp_v  = -1.0f;
static float g_disp_i  = -1.0f;
static int   g_disp_cc = -1;

/** Take one reading. Returns 1 on success, 0 on error (snapshot cleared). */
EXPORT long Voltio_ReadDisplay(void)
{
    g_disp_v = -1.0f; g_disp_i = -1.0f; g_disp_cc = -1;
    if (!g_ctx) return 0;
    float v, i; int cc;
    if (voltio_get_display(g_ctx, &v, &i, &cc) != VOLTIO_OK) return 0;
    g_disp_v = v; g_disp_i = i; g_disp_cc = cc;
    return 1;
}

EXPORT double Voltio_DisplayVoltage(void) { return (double)g_disp_v; }
EXPORT double Voltio_DisplayCurrent(void) { return (double)g_disp_i; }
EXPORT double Voltio_DisplayPower(void)
{
    return g_disp_cc < 0 ? -1.0 : (double)(g_disp_v * g_disp_i);
}
/** Returns 0=CV, 1=CC, -1=no valid snapshot */
EXPORT long   Voltio_DisplayMode(void)    { return g_disp_cc; }

/* -------------------------------------------------------------------------
 * Preset memory
 * ---------------------------------------------------------------------- */

EXPORT long Voltio_RecallPreset(long slot)
{
    if (!g_ctx) return 0;
    return voltio_recall_preset(g_ctx, (int)slot) == VOLTIO_OK ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * Protection limits
 * ---------------------------------------------------------------------- */

EXPORT long   Voltio_SetOVP(double volts) { if (!g_ctx) return 0; return voltio_set_ovp(g_ctx, (float)volts) == VOLTIO_OK ? 1 : 0; }
EXPORT long   Voltio_SetOCP(double amps)  { if (!g_ctx) return 0; return voltio_set_ocp(g_ctx, (float)amps)  == VOLTIO_OK ? 1 : 0; }

EXPORT double Voltio_GetOVP(void)
{
    if (!g_ctx) return -1.0;
    float v;
    return voltio_get_ovp(g_ctx, &v) == VOLTIO_OK ? (double)v : -1.0;
}

EXPORT double Voltio_GetOCP(void)
{
    if (!g_ctx) return -1.0;
    float a;
    return voltio_get_ocp(g_ctx, &a) == VOLTIO_OK ? (double)a : -1.0;
}

/* -------------------------------------------------------------------------
 * DllMain
 * ---------------------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL; (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_DETACH) Voltio_Close();
    return TRUE;
}
