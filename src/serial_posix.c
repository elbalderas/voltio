/**
 * serial_posix.c  —  Serial HAL implementation for macOS / Linux
 *
 * Uses POSIX termios.  Compiled when building on macOS or Linux.
 * The PSU appears as a virtual COM port — on macOS typically:
 *   /dev/tty.usbserial-XXXX   (blocks until device connects)
 *   /dev/cu.usbserial-XXXX    (returns immediately — prefer this one)
 *
 * Settings mirror the PSU manual §1.1:
 *   9600 baud, 8 data bits, no parity, 1 stop bit, no flow control
 *   2-second read timeout (VTIME = 20 tenths-of-a-second)
 */

#include "serial.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Internal context
 * ---------------------------------------------------------------------- */
struct Serial_Port {
    int fd;   /* file descriptor */
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

    /* O_NOCTTY  : don't let the port become the controlling terminal
     * O_NDELAY  : don't block waiting for DCD (carrier detect)        */
    int fd = open(port, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        if (error) snprintf(error, error_len,
                            "open(%s) failed: %s", port, strerror(errno));
        return NULL;
    }

    /* Restore blocking mode for reads */
    fcntl(fd, F_SETFL, 0);

    /* Save current settings, then configure */
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        if (error) snprintf(error, error_len,
                            "tcgetattr(%s) failed: %s", port, strerror(errno));
        close(fd);
        return NULL;
    }

    /* --- Baud rate --- */
    cfsetispeed(&tty, B9600);
    cfsetospeed(&tty, B9600);

    /* --- Control flags --- */
    tty.c_cflag &= ~PARENB;          /* No parity                       */
    tty.c_cflag &= ~CSTOPB;          /* 1 stop bit                      */
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |=  CS8;             /* 8 data bits                     */
    tty.c_cflag &= ~CRTSCTS;         /* No hardware flow control        */
    tty.c_cflag |=  CREAD | CLOCAL;  /* Enable receiver, ignore modem   */

    /* --- Input flags --- */
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);   /* No software flow ctrl */
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    /* --- Output flags --- */
    tty.c_oflag &= ~OPOST;   /* Raw output */
    tty.c_oflag &= ~ONLCR;

    /* --- Local flags --- */
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    /* --- Read timeout ---
     * VMIN=0, VTIME=20 : return after 2 seconds even if no bytes arrived */
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 20;   /* tenths of a second */

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        if (error) snprintf(error, error_len,
                            "tcsetattr(%s) failed: %s", port, strerror(errno));
        close(fd);
        return NULL;
    }

    Serial_Port *p = (Serial_Port *)malloc(sizeof(Serial_Port));
    if (!p) {
        if (error) snprintf(error, error_len, "malloc failed");
        close(fd);
        return NULL;
    }
    p->fd = fd;
    return p;
}

/* -------------------------------------------------------------------------
 * Close
 * ---------------------------------------------------------------------- */
void serial_close(Serial_Port *p)
{
    if (!p) return;
    if (p->fd >= 0) close(p->fd);
    free(p);
}

/* -------------------------------------------------------------------------
 * Write
 * ---------------------------------------------------------------------- */
Serial_Status serial_write(Serial_Port *p, const char *buf, size_t len)
{
    if (!p || !buf) return SERIAL_ERR_NULL;

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(p->fd, buf + written, len - written);
        if (n < 0) return SERIAL_ERR_WRITE;
        written += (size_t)n;
    }
    /* Drain the output buffer before returning so the PSU sees the full
     * command before we start reading the response.                    */
    tcdrain(p->fd);
    return SERIAL_OK;
}

/* -------------------------------------------------------------------------
 * Read one byte
 * ---------------------------------------------------------------------- */
Serial_Status serial_read_byte(Serial_Port *p, char *out_byte)
{
    if (!p || !out_byte) return SERIAL_ERR_NULL;

    ssize_t n = read(p->fd, out_byte, 1);
    if (n < 0)  return SERIAL_ERR_READ;
    if (n == 0) return SERIAL_ERR_TIMEOUT;   /* VTIME expired */
    return SERIAL_OK;
}

/* -------------------------------------------------------------------------
 * Flush RX buffer
 * ---------------------------------------------------------------------- */
void serial_flush_rx(Serial_Port *p)
{
    if (!p) return;
    tcflush(p->fd, TCIFLUSH);
}
