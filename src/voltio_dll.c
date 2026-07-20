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
 *     callDll(hVoltio, "Voltio_Open", "COM4");
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

/** Open COM port. Returns 1 on success, 0 on failure. */
EXPORT long Voltio_Open(const char *port)
{
    if (g_ctx) { voltio_close(g_ctx); g_ctx = NULL; }
    g_ctx = voltio_open(port, 1);   /* 1 decimal place for 1901B */
    return (g_ctx && voltio_last_error(g_ctx)[0] == '\0') ? 1 : 0;
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
