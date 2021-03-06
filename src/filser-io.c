/*
 * loggertools
 * Copyright (C) 2004-2007 Max Kellermann <max@duempel.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the License
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <assert.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include "filser.h"

int filser_write_cmd(filser_t device, unsigned char cmd) {
    static const unsigned char prefix = FILSER_PREFIX;
    ssize_t nbytes;

    nbytes = write(device->fd, &prefix, sizeof(prefix));
    if (nbytes <= 0)
        return (int)nbytes;

    nbytes = write(device->fd, &cmd, sizeof(cmd));
    if (nbytes <= 0)
        return (int)nbytes;

    return 1;
}

int filser_write_crc(filser_t device, const void *p0, size_t length) {
    const unsigned char *const p = p0;
    ssize_t nbytes;
    unsigned char crc;
    size_t pos = 0, sublen;

    while (pos < length) {
        if (pos > 0) {
            struct timeval tv;

            /* we have to throttle here ... */
            tv.tv_sec = 0;
            tv.tv_usec = 10000;
            select(0, NULL, NULL, NULL, &tv);
        }

        sublen = length - pos;
        if (sublen > 256)
            sublen = 256;

        nbytes = write(device->fd, p + pos, sublen);
        if (nbytes < 0)
            return -1;

        if ((size_t)nbytes != sublen)
            return 0;

        pos += sublen;
    }

    crc = filser_calc_crc(p, length);

    nbytes = write(device->fd, &crc, sizeof(crc));
    if (nbytes < 0)
        return -1;

    if ((size_t)nbytes != sizeof(crc))
        return 0;

    return 1;
}

int filser_write_packet(filser_t device, unsigned char cmd,
                        const void *packet, size_t length) {
    int ret;

    assert(device->fd >= 0);
    assert(packet != NULL && length > 0);

    ret = filser_write_cmd(device, cmd);
    if (ret <= 0)
        return ret;

    ret = filser_write_crc(device, packet, length);
    if (ret <= 0)
        return ret;

    return 1;
}

static int filser_select(filser_t device, time_t seconds) {
    fd_set rfds;
    struct timeval tv;

    tv.tv_sec = seconds;
    tv.tv_usec = 0;

    FD_ZERO(&rfds);
    FD_SET(device->fd, &rfds);

    return select(device->fd + 1, &rfds, NULL, NULL, &tv);
}

int filser_read(filser_t device, void *p0, size_t length,
                time_t timeout) {
    unsigned char *p = p0;
    ssize_t nbytes;
    size_t pos = 0;
    time_t end_time = 0;

    if (timeout > 0)
        end_time = time(NULL) + timeout;

    for (;;) {
        nbytes = read(device->fd, p + pos, length - pos);
        if (nbytes < 0)
            return -1;

        if (nbytes > 0)
            end_time = time(NULL) + timeout;

        pos += (size_t)nbytes;
        if (pos >= length)
            break;

        if (timeout > 0 && time(NULL) > end_time)
            return 0;
    }

    return 1;
}

int filser_read_crc(filser_t device, void *p0, size_t length,
                    time_t timeout) {
    int ret;
    unsigned char crc;

    ret = filser_read(device, p0, length, timeout);
    if (ret <= 0)
        return ret;

    ret = filser_read(device, &crc, sizeof(crc), timeout);
    if (ret <= 0)
        return ret;

    if (crc != filser_calc_crc(p0, length))
        return -2;

    return 1;
}

ssize_t filser_read_most(filser_t device, void *p0, size_t length,
                         time_t timeout) {
    unsigned char *buffer = p0;
    int ret;
    ssize_t nbytes;
    size_t pos = 0;
    time_t end_time = time(NULL) + timeout, timeout2 = 0;

    assert(length > 0);
    assert(timeout > 0);

    for (;;) {
        ret = filser_select(device, 1);
        if (ret < 0)
            return -1;

        if (ret == 0)
            return (ssize_t)pos;

        nbytes = read(device->fd, buffer + pos, length - pos);
        if (nbytes < 0)
            return -1;

        if (timeout2 == 0) {
            if (nbytes == 0)
                timeout2 = time(NULL) + 1;
        } else {
            if (nbytes > 0)
                timeout2 = 0;
        }

        pos += (size_t)nbytes;
        if (pos >= length)
            return (ssize_t)pos;

        if (timeout2 > 0 && time(NULL) >= timeout2)
            return pos;

        if (time(NULL) >= end_time) {
            errno = EINTR;
            return -1;
        }
    }
}

ssize_t filser_read_most_crc(filser_t device, void *p0, size_t length,
                             time_t timeout) {
    unsigned char *buffer = p0;
    ssize_t nbytes;
    unsigned char crc;

    nbytes = filser_read_most(device, buffer, length, timeout);
    if (nbytes <= 0)
        return nbytes;

    crc = filser_calc_crc(buffer, nbytes - 1);
    if (crc != buffer[nbytes - 1])
        return -2;

    return nbytes;
}
