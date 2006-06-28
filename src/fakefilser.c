/*
 * loggertools
 * Copyright (C) 2004-2005 Max Kellermann <max@duempel.org>
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
 *
 * $Id$
 */

#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED

#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <sys/select.h>

#include "filser.h"

struct filser_wtf37 {
    char space[36];
    char null;
};

struct filser {
    int fd;
    struct filser_flight_info flight_info;
    struct filser_contest_class contest_class;
    struct filser_setup setup;
    struct filser_tp_tsk tp_tsk;
    unsigned start_address, end_address;
};


static void alarm_handler(int dummy) {
    (void)dummy;
}

static void usage(void) {
    printf("usage: fakefilser\n");
}

static void arg_error(const char *msg) __attribute__ ((noreturn));
static void arg_error(const char *msg) {
    fprintf(stderr, "fakefilser: %s\n", msg);
    fprintf(stderr, "Try 'fakefilser --help' for more information.\n");
    _exit(1);
}

static void default_filser(struct filser *filser) {
    /*unsigned i;*/

    memset(filser, 0, sizeof(*filser));
    strcpy(filser->flight_info.pilot, "Mustermann");
    strcpy(filser->flight_info.model, "G103");
    strcpy(filser->flight_info.registration, "D-3322");
    strcpy(filser->flight_info.number, "2H");

    strcpy(filser->contest_class.contest_class, "FOOBAR");

    filser->setup.sampling_rate_normal = 5;
    filser->setup.sampling_rate_near_tp = 1;
    filser->setup.sampling_rate_button = 1;
    filser->setup.sampling_rate_k = 20;
    filser->setup.button_fixes = 15;
    filser->setup.tp_radius = htons(1000);

    /*
    for (i = 0; i < 100; i++)
        memset(filser->tp_tsk.wtf37[i].space,
               ' ',
               sizeof(filser->tp_tsk.wtf37[i].space));
    */
}

static void write_crc(int fd, const void *buffer, size_t length) {
    int ret;

    ret = filser_write_crc(fd, buffer, length);
    if (ret < 0) {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
        _exit(1);
    }

    if (ret == 0) {
        fprintf(stderr, "short write\n");
        _exit(1);
    }
}

static ssize_t read_full_crc(int fd, void *buffer, size_t len) {
    int ret;

    ret = filser_read_crc(fd, buffer, len, 40);
    if (ret == -2) {
        fprintf(stderr, "CRC error\n");
        _exit(1);
    }

    if (ret <= 0)
        return -1;

    return (ssize_t)len;
}

static void dump_line(size_t offset,
                      const unsigned char *line,
                      size_t length) {
    size_t i;

    assert(length > 0);
    assert(length <= 0x10);

    if (length == 0)
        return;

    printf("%04x ", (unsigned)offset);

    for (i = 0; i < 0x10; i++) {
        if (i % 8 == 0)
            printf(" ");

        if (i >= length)
            printf("   ");
        else
            printf(" %02x", line[i]);
    }

    printf(" | ");
    for (i = 0; i < length; i++)
        putchar(line[i] >= 0x20 && line[i] < 0x80 ? line[i] : '.');

    printf("\n");
}

static void dump_buffer(const void *q,
                        size_t length) {
    const unsigned char *p = q;
    size_t offset = 0;

    while (length >= 0x10) {
        dump_line(offset, p, 0x10);

        p += 0x10;
        length -= 0x10;
        offset += 0x10;
    }

    if (length > 0)
        dump_line(offset, p, length);
}

static void dump_buffer_crc(const void *q,
                            size_t length) {
    dump_buffer(q, length);
    printf("crc = %02x\n",
           filser_calc_crc((const unsigned char*)q, length));
}

static void dump_timeout(struct filser *filser) {
    unsigned char data;
    ssize_t nbytes;
    time_t timeout = time(NULL) + 2;
    unsigned offset = 0, i;
    unsigned char row[0x10];

    while (1) {
        alarm(1);
        nbytes = read(filser->fd, &data, sizeof(data));
        alarm(0);
        if (nbytes < 0)
            break;
        if ((size_t)nbytes >= sizeof(data)) {
            if (offset % 16 == 0)
                printf("%04x ", offset);
            else if (offset % 8 == 0)
                printf(" ");

            printf(" %02x", data);
            row[offset%0x10] = data;
            ++offset;

            if (offset % 16 == 0) {
                printf(" | ");
                for (i = 0; i < 0x10; i++)
                    putchar(row[i] >= 0x20 && row[i] < 0x80 ? row[i] : '.');
                printf("\n");
            }

            timeout = time(NULL) + 2;
        } else if (time(NULL) >= timeout)
            break;
        fflush(stdout);
    }

    if (offset % 16 > 0) {
        printf(" | ");
        for (i = 0; i < offset % 16; i++)
            putchar(row[i] >= 0x20 && row[i] < 0x80 ? row[i] : '.');
        printf("\n");
    }

    printf("\n");
}

static void send_ack(struct filser *filser) {
    static const unsigned char ack = 0x06;
    ssize_t nbytes;

    nbytes = write(filser->fd, &ack, sizeof(ack));
    if (nbytes < 0) {
        fprintf(stderr, "write() failed: %s\n", strerror(errno));
        _exit(1);
    }

    if ((size_t)nbytes < sizeof(ack)) {
        fprintf(stderr, "short write()\n");
        _exit(1);
    }
}

static void handle_syn(struct filser *filser) {
    send_ack(filser);
}

static void handle_check_mem_settings(struct filser *filser) {
    static const unsigned char buffer[6] = {
        0x00, 0x00, 0x06, 0x80, 0x00, 0x0f,
    };

    write_crc(filser->fd, buffer, sizeof(buffer));
}

static void handle_open_flight_list(struct filser *filser) {
    struct filser_flight_index flight;

    memset(&flight, 0, sizeof(flight));

    flight.valid = 1;
    flight.start_address2 = 0x06;
    flight.end_address1 = 0x0f;
    flight.end_address0 = 0xd1;
    flight.end_address2 = 0x06;
    strcpy(flight.date, "16.07.05");
    strcpy(flight.start_time, "20:14:06");
    strcpy(flight.stop_time, "21:07:47");
    strcpy(flight.pilot, "Max Kellermann");
    flight.logger_id = htons(15149);
    flight.flight_no = 1;
    dump_buffer_crc(&flight, sizeof(flight));
    write_crc(filser->fd, (const unsigned char*)&flight, sizeof(flight));

    flight.start_address0 = 0x01;
    flight.start_address1 = 0x23;
    flight.start_address2 = 0x45;
    flight.start_address3 = 0x67;
    flight.end_address0 = 0x01;
    flight.end_address1 = 0x23;
    flight.end_address2 = 0x45;
    flight.end_address3 = 0x80;
    strcpy(flight.date, "23.08.05");
    strcpy(flight.start_time, "08:55:11");
    strcpy(flight.stop_time, "16:22:33");
    strcpy(flight.pilot, "Max Kellermann");
    flight.flight_no++;
    write_crc(filser->fd, (const unsigned char*)&flight, sizeof(flight));

    strcpy(flight.date, "24.08.05");
    strcpy(flight.start_time, "09:55:11");
    strcpy(flight.stop_time, "15:22:33");
    strcpy(flight.pilot, "Max Kellermann");
    dump_buffer_crc(&flight, sizeof(flight));
    write_crc(filser->fd, (const unsigned char*)&flight, sizeof(flight));

    memset(&flight, 0, sizeof(flight));
    write_crc(filser->fd, (const unsigned char*)&flight, sizeof(flight));
}

static void handle_get_basic_data(struct filser *filser) {
    static const unsigned char buffer[0x148] = "\x0d\x0aVersion COLIBRI   V3.01\x0d\x0aSN13123,HW2.0\x0d\x0aID:313-[313]\x0d\x0aChecksum:64\x0d\x0aAPT:APTempty\x0d\x0a 10.6.3\x0d\x0aKey uploaded by\x0d\x0aMihelin Peter\x0d\x0aLX Navigation\x0d\x0a";

    write(filser->fd, buffer, sizeof(buffer));
}

static void handle_get_flight_info(struct filser *filser) {
    write_crc(filser->fd, (const unsigned char*)&filser->flight_info, sizeof(filser->flight_info));
}

static void handle_get_mem_section(struct filser *filser) {
    struct filser_packet_mem_section packet;

    memset(&packet, 0, sizeof(packet));
    packet.section_lengths[0] = htons(0x4000);

    write_crc(filser->fd, &packet, sizeof(packet));
}

static void handle_def_mem(struct filser *filser) {
    struct filser_packet_def_mem packet;

    read_full_crc(filser->fd, &packet, sizeof(packet));

    filser->start_address
        = packet.start_address[2]
        + (packet.start_address[1] << 8)
        + (packet.start_address[0] << 16);
    filser->end_address
        = packet.end_address[2]
        + (packet.end_address[1] << 8)
        + (packet.end_address[0] << 16);

    printf("def_mem: address = %02x %02x %02x %02x %02x %02x\n",
           packet.start_address[0],
           packet.start_address[1],
           packet.start_address[2],
           packet.end_address[0],
           packet.end_address[1],
           packet.end_address[2]);

    printf("def_mem: address = %06x %06x\n",
           filser->start_address, filser->end_address);

    send_ack(filser);
}

static void handle_get_logger_data(struct filser *filser, unsigned block) {
    unsigned size;
    unsigned char *foo;

    switch (block) {
    case 0:
        size = 0x740;
        break;
    case 1:
        size = 0x9ef;
        break;
    default:
        size = 0;
    }

    printf("reading block %u size %u\n", block, size);

    foo = malloc(size);
    if (foo == NULL) {
        fprintf(stderr, "malloc(%u) failed\n", size);
        _exit(1);
    }

    memset(foo, 0, sizeof(foo));

    write_crc(filser->fd, foo, size);
}

static void handle_write_flight_info(struct filser *filser) {
    struct filser_flight_info flight_info;
    ssize_t nbytes;

    nbytes = read_full_crc(filser->fd, &flight_info,
                           sizeof(flight_info));
    if (nbytes < 0) {
        fprintf(stderr, "read failed: %s\n", strerror(errno));
        _exit(1);
    }
    if ((size_t)nbytes < sizeof(flight_info)) {
        fprintf(stderr, "short read\n");
        _exit(1);
    }

    printf("FI: Pilot = %s\n", flight_info.pilot);
    printf("FI: Model = %s\n", flight_info.model);
    printf("FI: Registration = %s\n", flight_info.registration);

    filser->flight_info = flight_info;

    send_ack(filser);
}

static void handle_read_tp_tsk(struct filser *filser) {
    char buffer[0x5528];
    /*fprintf(stderr, "write_tp_tsk: %zx\n", sizeof(filser->tp_tsk));
    write_crc(filser->fd, (const unsigned char*)&filser->tp_tsk,
              sizeof(filser->tp_tsk));
    */
    buffer[0x48a4]=0x47;
    buffer[0x48a5]=0x48;
    fprintf(stderr, "write_tp_tsk: 0x%zx\n", sizeof(buffer));
    memset(&buffer, 0, sizeof(buffer));
    write_crc(filser->fd, buffer, sizeof(buffer));
}

static void handle_write_tp_tsk(struct filser *filser) {
    struct filser_tp_tsk tp_tsk;
    ssize_t nbytes;

    nbytes = read_full_crc(filser->fd, &tp_tsk, sizeof(tp_tsk));
    if (nbytes < 0) {
        fprintf(stderr, "read failed: %s\n", strerror(errno));
        _exit(1);
    }
    if ((size_t)nbytes < sizeof(tp_tsk)) {
        fprintf(stderr, "short read\n");
        _exit(1);
    }

    filser->tp_tsk = tp_tsk;

    send_ack(filser);
}

static void handle_read_setup(struct filser *filser) {
    write_crc(filser->fd, (const unsigned char*)&filser->setup,
              sizeof(filser->setup));
}

static void handle_write_setup(struct filser *filser) {
    struct filser_setup setup;
    ssize_t nbytes;

    nbytes = read_full_crc(filser->fd, &setup, sizeof(setup));
    if (nbytes < 0) {
        fprintf(stderr, "read failed: %s\n", strerror(errno));
        _exit(1);
    }
    if ((size_t)nbytes < sizeof(setup)) {
        fprintf(stderr, "short read\n");
        _exit(1);
    }

    filser->setup = setup;

    send_ack(filser);
}

static void handle_read_contest_class(struct filser *filser) {
    write_crc(filser->fd, (const unsigned char*)&filser->contest_class,
              sizeof(filser->contest_class));
    dump_buffer_crc(&filser->contest_class, sizeof(filser->contest_class));
}

static void handle_get_extra_data(struct filser *filser) {
    static const unsigned char extra_data[0x100];

    write_crc(filser->fd, extra_data, sizeof(extra_data));
}

static void handle_write_contest_class(struct filser *filser) {
    struct filser_contest_class contest_class;
    ssize_t nbytes;

    nbytes = read_full_crc(filser->fd, &contest_class, sizeof(contest_class));
    if (nbytes < 0) {
        fprintf(stderr, "read failed: %s\n", strerror(errno));
        _exit(1);
    }
    if ((size_t)nbytes < sizeof(contest_class)) {
        fprintf(stderr, "short read\n");
        _exit(1);
    }

    filser->contest_class = contest_class;

    send_ack(filser);
}

static void handle_30(struct filser *filser) {
    char foo[0x8000];
    memset(&foo, 0, sizeof(foo));
    write_crc(filser->fd, foo, sizeof(foo));
}

static void handle_61_down(struct filser *filser) {
    char foo[16000];
    write_crc(filser->fd, foo, 8164);
}

static void handle_61_up(struct filser *filser) {
    char foo[0x8000];
    int ret;

    ret = filser_read_crc(filser->fd, foo, sizeof(foo), 15);
    if (ret <= 0) {
        fprintf(stderr, "61UP error\n");
        _exit(1);
    }

    send_ack(filser);
}

static void handle_68(struct filser *filser) {
    char foo[0xa07];
    int ret;

    ret = filser_read_crc(filser->fd, foo, sizeof(foo), 15);
    if (ret <= 0) {
        fprintf(stderr, "0x68 error\n");
        _exit(1);
    }

    send_ack(filser);
}

static int open_virtual(const char *symlink_path) {
    int fd, ret;

    fd = open("/dev/ptmx", O_RDWR|O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "failed to open /dev/ptmx: %s\n",
                strerror(errno));
        _exit(1);
    }

    ret = unlockpt(fd);
    if (ret < 0) {
        fprintf(stderr, "failed to unlockpt(): %s\n",
                strerror(errno));
        _exit(1);
    }

    printf("Slave terminal is %s\n", ptsname(fd));

    unlink(symlink_path);
    ret = symlink(ptsname(fd), symlink_path);
    if (ret < 0) {
        fprintf(stderr, "symlink() failed: %s\n",
                strerror(errno));
        _exit(1);
    }

    printf("Symlinked to %s\n", symlink_path);

    return fd;
}

int main(int argc, char **argv) {
    struct filser filser;
    int was_70 = 0;

    (void)argv;

    signal(SIGALRM, alarm_handler);

    if (argc > 1) {
        if (strcmp(argv[0], "--help") == 0) {
            usage();
            _exit(0);
        } else {
            arg_error("Too many arguments");
        }
    }

    default_filser(&filser);

    filser.fd = open_virtual("/tmp/fakefilser");

    while (1) {
        unsigned char cmd;
        int ret;

        ret = filser_read(filser.fd, &cmd, sizeof(cmd), 5);
        if (ret < 0) {
            if (errno == EIO) {
                close(filser.fd);
                filser.fd = open_virtual("/tmp/fakefilser");
                continue;
            }

            fprintf(stderr, "read failed: %s\n", strerror(errno));
            _exit(1);
        }

        if (ret == 0)
            continue;

        switch (cmd) {
        case FILSER_SYN: /* SYN (expects ACK) */
            printf("received SYN\n");
            /* send ACK */
            handle_syn(&filser);
            break;

        case FILSER_PREFIX:
            ret = filser_read(filser.fd, &cmd, sizeof(cmd), 5);
            if (ret <= 0) {
                fprintf(stderr, "read failed: %s\n", strerror(errno));
                _exit(1);
            }

            printf("cmd=0x%02x\n", cmd);
            fflush(stdout);

            if (cmd >= FILSER_READ_LOGGER_DATA) {
                printf("received get_logger_data\n");
                handle_get_logger_data(&filser, cmd - FILSER_READ_LOGGER_DATA);
                break;
            }

            switch (cmd) {
            case FILSER_READ_TP_TSK:
                printf("received read_tp_tsk\n");
                handle_read_tp_tsk(&filser);
                break;

            case FILSER_WRITE_TP_TSK:
                printf("received write_tp_tsk\n");
                handle_write_tp_tsk(&filser);
                break;

            case FILSER_READ_FLIGHT_LIST:
                printf("received open_flight_list\n");
                handle_open_flight_list(&filser);
                break;

            case FILSER_READ_BASIC_DATA:
                printf("received get_basic_data\n");
                handle_get_basic_data(&filser);
                break;

            case FILSER_READ_SETUP:
                printf("received read_setup\n");
                handle_read_setup(&filser);
                break;

            case FILSER_WRITE_SETUP:
                printf("received write_setup\n");
                handle_write_setup(&filser);
                break;

            case FILSER_READ_FLIGHT_INFO:
                printf("received get_flight_info\n");
                handle_get_flight_info(&filser);
                break;

            case FILSER_WRITE_FLIGHT_INFO:
                printf("received write_flight_info\n");
                handle_write_flight_info(&filser);
                break;

            case FILSER_READ_EXTRA_DATA:
                printf("received get_extra_data\n");
                handle_get_extra_data(&filser);
                break;

            case FILSER_GET_MEM_SECTION:
                printf("received get_mem_section\n");
                handle_get_mem_section(&filser);
                break;

            case FILSER_DEF_MEM:
                printf("received def_mem\n");
                handle_def_mem(&filser);
                break;

            case FILSER_READ_CONTEST_CLASS:
                printf("received read_contest_class\n");
                handle_read_contest_class(&filser);
                break;

            case FILSER_WRITE_CONTEST_CLASS:
                printf("received write_contest_class\n");
                handle_write_contest_class(&filser);
                break;

            case FILSER_CHECK_MEM_SETTINGS:
                printf("received check_mem_settings\n");
                handle_check_mem_settings(&filser);
                break;

            case 0x30: /* read LO4 logger */
            case 0x31:
            case 0x32:
                handle_30(&filser);
                break;

            case 0x4c:
                /* clear LO4 logger? */
                send_ack(&filser);
                break;

            case 0x61:
            case 0x62:
            case 0x63:
            case 0x64:
                if (was_70)
                    handle_61_up(&filser);
                else
                    handle_61_down(&filser);
                break;

            case 0x68: /* APT state block */
                handle_68(&filser);
                break;

            case 0x70:
                /* darauf folgt 0x61 mit daten */
                was_70 = 1;
                break;

            default:
                fprintf(stderr, "unknown command 0x02 0x%02x received, dumping input\n", cmd);
                dump_timeout(&filser);
            }
            break;

        default:
            fprintf(stderr, "unknown command 0x%02x received\n", cmd);
            _exit(1);
        }
    }

    return 0;
}
