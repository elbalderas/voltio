/**
 * serial_win32.c  —  Serial HAL implementation for Windows
 *
 * Uses Win32 CreateFile / ReadFile / WriteFile.
 * Compiled only on Windows (CMake guards this with a platform check).
 *
 * Settings: 9600 8N1, no flow control, 2-second read timeout.
 */

#include "serial.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal context
 * ---------------------------------------------------------------------- */
struct Serial_Port {
    HANDLE hPort;
};

/* -------------------------------------------------------------------------
 * Open
 * ---------------------------------------------------------------------- */
Serial_Port *serial_open(const char *port, char *error, size_t error_len)
{
    if (!port) {
        if (error) snprintf(error, error_len, "port is NULL");
        return NULL;
    }

    /* Prefix with \\.\  so COM10+ work correctly */
    char full_port[32];
    snprintf(full_port, sizeof(full_port), "\\\\.\\%s", port);

    HANDLE h = CreateFileA(
        full_port,
        GENERIC_READ | GENERIC_WRITE,
        0,              /* exclusive */
        NULL,
        OPEN_EXISTING,
        0,              /* synchronous I/O */
        NULL
    );

    if (h == INVALID_HANDLE_VALUE) {
        if (error) snprintf(error, error_len,
                            "CreateFile(%s) failed: error %lu",
                            port, GetLastError());
        return NULL;
    }

    /* --- Port parameters --- */
    DCB dcb = { .DCBlength = sizeof(DCB) };
    if (!GetCommState(h, &dcb)) {
        if (error) snprintf(error, error_len,
                            "GetCommState failed: %lu", GetLastError());
        CloseHandle(h);
        return NULL;
    }

    dcb.BaudRate        = CBR_9600;
    dcb.ByteSize        = 8;
    dcb.Parity          = NOPARITY;
    dcb.StopBits        = ONESTOPBIT;
    dcb.fBinary         = TRUE;
    dcb.fParity         = FALSE;
    dcb.fOutxCtsFlow    = FALSE;
    dcb.fOutxDsrFlow    = FALSE;
    dcb.fDtrControl     = DTR_CONTROL_DISABLE;
    dcb.fRtsControl     = RTS_CONTROL_DISABLE;
    dcb.fOutX           = FALSE;
    dcb.fInX            = FALSE;

    if (!SetCommState(h, &dcb)) {
        if (error) snprintf(error, error_len,
                            "SetCommState failed: %lu", GetLastError());
        CloseHandle(h);
        return NULL;
    }

    /* --- 2-second read timeout --- */
    COMMTIMEOUTS timeouts = {
        .ReadIntervalTimeout         = 100,
        .ReadTotalTimeoutMultiplier  = 10,
        .ReadTotalTimeoutConstant    = 2000,
        .WriteTotalTimeoutMultiplier = 10,
        .WriteTotalTimeoutConstant   = 2000,
    };
    SetCommTimeouts(h, &timeouts);

    Serial_Port *p = (Serial_Port *)malloc(sizeof(Serial_Port));
    if (!p) {
        if (error) snprintf(error, error_len, "malloc failed");
        CloseHandle(h);
        return NULL;
    }
    p->hPort = h;
    return p;
}

/* -------------------------------------------------------------------------
 * Close
 * ---------------------------------------------------------------------- */
void serial_close(Serial_Port *p)
{
    if (!p) return;
    if (p->hPort && p->hPort != INVALID_HANDLE_VALUE)
        CloseHandle(p->hPort);
    free(p);
}

/* -------------------------------------------------------------------------
 * Write
 * ---------------------------------------------------------------------- */
Serial_Status serial_write(Serial_Port *p, const char *buf, size_t len)
{
    if (!p || !buf) return SERIAL_ERR_NULL;

    DWORD written = 0;
    if (!WriteFile(p->hPort, buf, (DWORD)len, &written, NULL) ||
        written != (DWORD)len)
        return SERIAL_ERR_WRITE;

    return SERIAL_OK;
}

/* -------------------------------------------------------------------------
 * Read one byte
 * ---------------------------------------------------------------------- */
Serial_Status serial_read_byte(Serial_Port *p, char *out_byte)
{
    if (!p || !out_byte) return SERIAL_ERR_NULL;

    DWORD bytes_read = 0;
    if (!ReadFile(p->hPort, out_byte, 1, &bytes_read, NULL))
        return SERIAL_ERR_READ;
    if (bytes_read == 0)
        return SERIAL_ERR_TIMEOUT;

    return SERIAL_OK;
}

/* -------------------------------------------------------------------------
 * Flush RX buffer
 * ---------------------------------------------------------------------- */
void serial_flush_rx(Serial_Port *p)
{
    if (!p) return;
    PurgeComm(p->hPort, PURGE_RXCLEAR);
}
