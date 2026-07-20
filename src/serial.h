/**
 * serial.h  —  Platform-agnostic serial port HAL
 *
 * psu.c calls only these functions.  The implementation is selected at
 * compile time:
 *   macOS / Linux  ->  serial_posix.c   (termios)
 *   Windows        ->  serial_win32.c   (Win32 CreateFile / ReadFile)
 *
 * All functions are intentionally minimal — just what the PSU protocol
 * needs: open, close, write a buffer, read one byte at a time.
 */

#ifndef SERIAL_H
#define SERIAL_H

#include <stddef.h>   /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Opaque handle — never inspect the contents outside serial_*.c
 * ---------------------------------------------------------------------- */
typedef struct Serial_Port Serial_Port;

/* -------------------------------------------------------------------------
 * Return codes
 * ---------------------------------------------------------------------- */
typedef enum {
    SERIAL_OK           =  0,
    SERIAL_ERR_OPEN     = -1,   /* Could not open the port               */
    SERIAL_ERR_CONFIG   = -2,   /* Could not apply baud/data settings    */
    SERIAL_ERR_WRITE    = -3,   /* Write failed or was incomplete        */
    SERIAL_ERR_READ     = -4,   /* Read failed                           */
    SERIAL_ERR_TIMEOUT  = -5,   /* Read returned no data within timeout  */
    SERIAL_ERR_NULL     = -6,   /* NULL pointer argument                 */
} Serial_Status;

/* -------------------------------------------------------------------------
 * Lifecycle
 *
 * serial_open()  opens the port at 9600 8N1, no flow control,
 *                2-second read timeout.
 *
 * @param port    Platform port string:
 *                  macOS   ->  "/dev/tty.usbserial-XXXX"  or "/dev/cu.*"
 *                  Windows ->  "COM4"  (\\.\COM4 added internally)
 * @param error   Optional: caller-supplied buffer for a human-readable
 *                error message on failure (pass NULL to ignore).
 * @param error_len  Size of the error buffer.
 * @return        Allocated handle, or NULL on failure.
 * ---------------------------------------------------------------------- */
Serial_Port   *serial_open (const char *port, char *error, size_t error_len);
void           serial_close(Serial_Port *p);   /* Safe to call with NULL  */

/* -------------------------------------------------------------------------
 * I/O
 * ---------------------------------------------------------------------- */

/**
 * Write len bytes from buf.  Blocks until all bytes are written or an
 * error occurs.
 */
Serial_Status  serial_write     (Serial_Port *p, const char *buf, size_t len);

/**
 * Read exactly one byte into *out_byte.
 * Blocks up to the port's read timeout.
 * Returns SERIAL_ERR_TIMEOUT if no byte arrives in time.
 */
Serial_Status  serial_read_byte (Serial_Port *p, char *out_byte);

/**
 * Discard any bytes waiting in the OS receive buffer.
 * Call this before sending a new command.
 */
void           serial_flush_rx  (Serial_Port *p);

#ifdef __cplusplus
}
#endif
#endif /* SERIAL_H */
