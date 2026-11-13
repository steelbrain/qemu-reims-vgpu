/*
 * Linux host USB redirector
 *
 * Copyright (c) 2005 Fabrice Bellard
 *
 * Copyright (c) 2008 Max Krasnyansky
 *      Support for host device auto connect & disconnect
 *      Major rewrite to support fully async operation
 *
 * Copyright 2008 TJ <linux@tjworld.net>
 *      Added flexible support for /dev/bus/usb /sys/bus/usb/devices in addition
 *      to the legacy /proc/bus/usb USB device discovery and handling
 *
 * (c) 2012 Gerd Hoffmann <kraxel@redhat.com>
 *      Completely rewritten to use libusb instead of usbfs ioctls.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qom/object.h"
#ifndef CONFIG_WIN32
#include <poll.h>
#endif
#include <libusb.h>

#ifdef CONFIG_LINUX
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#endif

#include "qapi/error.h"
#include "migration/vmstate.h"
#include "monitor/monitor.h"
#include "monitor/hmp.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "block/thread-pool.h"
#include "system/runstate.h"
#include "system/system.h"
#include "trace.h"

#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"

/* ------------------------------------------------------------------------ */

#define TYPE_USB_HOST_DEVICE "usb-host"
OBJECT_DECLARE_SIMPLE_TYPE(USBHostDevice, USB_HOST_DEVICE)

typedef struct USBHostRequest USBHostRequest;
typedef struct USBHostIsoXfer USBHostIsoXfer;
typedef struct USBHostIsoRing USBHostIsoRing;
typedef struct USBHostSetIntf USBHostSetIntf;
typedef struct USBHostDeferred USBHostDeferred;

typedef struct libusb_config_descriptor libusb_config_descriptor;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(libusb_config_descriptor,
                            libusb_free_config_descriptor)

struct USBAutoFilter {
    uint32_t bus_num;
    uint32_t addr;
    char     *port;
    uint32_t vendor_id;
    uint32_t product_id;
};

enum USBHostDeviceOptions {
    USB_HOST_OPT_PIPELINE,
};

struct USBHostDevice {
    USBDevice parent_obj;

    /* properties */
    struct USBAutoFilter             match;
    char                             *hostdevice;
    int32_t                          bootindex;
    uint32_t                         iso_urb_count;
    uint32_t                         iso_urb_frames;
    uint32_t                         options;
    uint32_t                         loglevel;
    bool                             allow_one_guest_reset;
    bool                             allow_all_guest_resets;
    bool                             suppress_remote_wake;

    /* state */
    QTAILQ_ENTRY(USBHostDevice)      next;
    int                              seen, errcount;
    int                              bus_num;
    int                              addr;
    char                             port[16];

    int                              hostfd;
    libusb_device                    *dev;
    libusb_device_handle             *dh;
    struct libusb_device_descriptor  ddesc;

    struct {
        bool                         detached;
        bool                         claimed;
    } ifs[USB_MAX_INTERFACES];

    /* bInterval of each endpoint of the current settings, [in][number] */
    uint8_t                          ep_interval[2][USB_MAX_ENDPOINTS + 1];

    /* callbacks & friends */
    QEMUBH                           *bh_nodev;
    QEMUBH                           *bh_postld;
    bool                             bh_postld_pending;
    Notifier                         exit;

    /* request queues */
    QTAILQ_HEAD(, USBHostRequest)    requests;
    QTAILQ_HEAD(, USBHostIsoRing)    isorings;

    /* the SET_INTERFACE request a worker thread carries out, if any */
    USBHostSetIntf                   *setintf;
    /*
     * Set from the start of that request until the submits and cancels that
     * waited for it have been made, in their order, from deferred.
     */
    bool                             deferring;
    QSIMPLEQ_HEAD(, USBHostDeferred) deferred;
};

struct USBHostRequest {
    USBHostDevice                    *host;
    USBPacket                        *p;
    bool                             in;
    struct libusb_transfer           *xfer;
    unsigned char                    *buffer;
    unsigned char                    *cbuf;
    unsigned int                     clen;
    bool                             usb3ep0quirk;
    QTAILQ_ENTRY(USBHostRequest)     next;
};

/*
 * libusb_set_interface_alt_setting() is synchronous: the kernel holds the
 * device while it kills the interface's transfers, sends SET_INTERFACE and
 * reconfigures the host controller's endpoints. A worker thread makes the call,
 * so QEMU's main loop and vCPUs keep running; the guest's control transfer
 * completes when the call returns.
 *
 * Linux's usbfs takes the same lock of the device for each of its ioctls,
 * so the main loop makes none on the device until the call returns: it
 * handles no libusb events, where libusb reaps the completed URBs of every
 * device it has any for (usb_host_events_pause), and the device's submits and
 * cancels wait in its deferred queue (usb_host_submit, usb_host_cancel),
 * which runs in order as the call returns.
 */
struct USBHostSetIntf {
    USBHostDevice                    *host;
    USBPacket                        *p;    /* NULL once the guest canceled it */
    libusb_device_handle             *dh;
    int                              iface;
    int                              alt;
    int                              rc;
    bool                             finished;
    QemuEvent                        done;  /* the worker's call returned */
};

/* A submit, or a cancel, of a transfer that waits for a SET_INTERFACE call */
struct USBHostDeferred {
    struct libusb_transfer           *xfer;
    bool                             cancel;
    QSIMPLEQ_ENTRY(USBHostDeferred)  next;
};

struct USBHostIsoXfer {
    USBHostIsoRing                   *ring;
    struct libusb_transfer           *xfer;
    bool                             copy_complete;
    unsigned int                     packet;
    QTAILQ_ENTRY(USBHostIsoXfer)     next;
};

struct USBHostIsoRing {
    USBHostDevice                    *host;
    USBEndpoint                      *ep;
    unsigned int                     nr_xfers;
    /* IN: transfers completed before the stream gave the guest data */
    unsigned int                     nr_primed;
    bool                             primed;
    /* OUT: a packet waits for a transfer to complete and free its buffer */
    bool                             out_waiting;
    QTAILQ_HEAD(, USBHostIsoXfer)    unused;
    QTAILQ_HEAD(, USBHostIsoXfer)    inflight;
    QTAILQ_HEAD(, USBHostIsoXfer)    copy;
    QTAILQ_ENTRY(USBHostIsoRing)     next;
};

static QTAILQ_HEAD(, USBHostDevice) hostdevs =
    QTAILQ_HEAD_INITIALIZER(hostdevs);

static void usb_host_auto_check(void *unused);
static void usb_host_release_interfaces(USBHostDevice *s);
static void usb_host_nodev(USBHostDevice *s);
static void usb_host_detach_kernel(USBHostDevice *s);
static void usb_host_attach_kernel(USBHostDevice *s);
static void usb_host_set_interface_wait(USBHostDevice *s);

/* ------------------------------------------------------------------------ */

#ifndef LIBUSB_LOG_LEVEL_WARNING /* older libusb didn't define these */
#define LIBUSB_LOG_LEVEL_WARNING 2
#endif

/* ------------------------------------------------------------------------ */

#define CONTROL_TIMEOUT  10000        /* 10 sec    */
#define BULK_TIMEOUT         0        /* unlimited */
#define INTR_TIMEOUT         0        /* unlimited */

#ifndef LIBUSB_API_VERSION
# define LIBUSB_API_VERSION LIBUSBX_API_VERSION
#endif
#if LIBUSB_API_VERSION >= 0x01000103
# define HAVE_STREAMS 1
#endif
#if LIBUSB_API_VERSION >= 0x01000106
# define HAVE_SUPER_PLUS 1
#endif

#ifdef CONFIG_HMP
static const char *speed_name[] = {
    [LIBUSB_SPEED_UNKNOWN] = "?",
    [LIBUSB_SPEED_LOW]     = "1.5",
    [LIBUSB_SPEED_FULL]    = "12",
    [LIBUSB_SPEED_HIGH]    = "480",
    [LIBUSB_SPEED_SUPER]   = "5000",
#ifdef HAVE_SUPER_PLUS
    [LIBUSB_SPEED_SUPER_PLUS] = "5000+",
#endif
};
#endif

static const unsigned int speed_map[] = {
    [LIBUSB_SPEED_LOW]     = USB_SPEED_LOW,
    [LIBUSB_SPEED_FULL]    = USB_SPEED_FULL,
    [LIBUSB_SPEED_HIGH]    = USB_SPEED_HIGH,
    [LIBUSB_SPEED_SUPER]   = USB_SPEED_SUPER,
#ifdef HAVE_SUPER_PLUS
    [LIBUSB_SPEED_SUPER_PLUS] = USB_SPEED_SUPER,
#endif
};

static const unsigned int status_map[] = {
    [LIBUSB_TRANSFER_COMPLETED] = USB_RET_SUCCESS,
    [LIBUSB_TRANSFER_ERROR]     = USB_RET_IOERROR,
    [LIBUSB_TRANSFER_TIMED_OUT] = USB_RET_IOERROR,
    [LIBUSB_TRANSFER_CANCELLED] = USB_RET_IOERROR,
    [LIBUSB_TRANSFER_STALL]     = USB_RET_STALL,
    [LIBUSB_TRANSFER_NO_DEVICE] = USB_RET_NODEV,
    [LIBUSB_TRANSFER_OVERFLOW]  = USB_RET_BABBLE,
};

static const char *err_names[] = {
    [-LIBUSB_ERROR_IO]               = "IO",
    [-LIBUSB_ERROR_INVALID_PARAM]    = "INVALID_PARAM",
    [-LIBUSB_ERROR_ACCESS]           = "ACCESS",
    [-LIBUSB_ERROR_NO_DEVICE]        = "NO_DEVICE",
    [-LIBUSB_ERROR_NOT_FOUND]        = "NOT_FOUND",
    [-LIBUSB_ERROR_BUSY]             = "BUSY",
    [-LIBUSB_ERROR_TIMEOUT]          = "TIMEOUT",
    [-LIBUSB_ERROR_OVERFLOW]         = "OVERFLOW",
    [-LIBUSB_ERROR_PIPE]             = "PIPE",
    [-LIBUSB_ERROR_INTERRUPTED]      = "INTERRUPTED",
    [-LIBUSB_ERROR_NO_MEM]           = "NO_MEM",
    [-LIBUSB_ERROR_NOT_SUPPORTED]    = "NOT_SUPPORTED",
    [-LIBUSB_ERROR_OTHER]            = "OTHER",
};

static libusb_context *ctx;
static uint32_t loglevel;

/*
 * The SET_INTERFACE calls in flight. While there is one, the main loop
 * handles no libusb events: the other devices' completions wait for the
 * call, as the whole main loop did while it made the call itself.
 */
static unsigned int events_paused;

#ifndef CONFIG_WIN32

static void usb_host_handle_fd(void *opaque)
{
    struct timeval tv = { 0, 0 };
    libusb_handle_events_timeout(ctx, &tv);
}

static void usb_host_add_fd(int fd, short events, void *user_data)
{
    if (events_paused) {
        return; /* usb_host_events_resume adds it */
    }
    qemu_set_fd_handler(fd,
                        (events & POLLIN)  ? usb_host_handle_fd : NULL,
                        (events & POLLOUT) ? usb_host_handle_fd : NULL,
                        ctx);
}

static void usb_host_del_fd(int fd, void *user_data)
{
    qemu_set_fd_handler(fd, NULL, NULL, NULL);
}

/* Take libusb's fds out of the main loop, or put them back */
static void usb_host_watch_fds(bool watch)
{
    const struct libusb_pollfd **poll = libusb_get_pollfds(ctx);
    int i;

    for (i = 0; poll && poll[i] != NULL; i++) {
        if (watch) {
            usb_host_add_fd(poll[i]->fd, poll[i]->events, ctx);
        } else {
            usb_host_del_fd(poll[i]->fd, ctx);
        }
    }
    free(poll);
}

#else

static QEMUTimer *poll_timer;
static uint32_t request_count;

static void usb_host_timer_kick(void)
{
    int64_t delay_ns;

    delay_ns = request_count
        ? (NANOSECONDS_PER_SECOND / 100)  /* 10 ms interval with active req */
        : (NANOSECONDS_PER_SECOND);       /* 1 sec interval otherwise */
    timer_mod(poll_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static void usb_host_timer(void *opaque)
{
    struct timeval tv = { 0, 0 };

    if (!events_paused) {
        libusb_handle_events_timeout(ctx, &tv);
    }
    usb_host_timer_kick();
}

#endif /* !CONFIG_WIN32 */

static void usb_host_events_pause(void)
{
    if (events_paused++ == 0) {
#ifndef CONFIG_WIN32
        usb_host_watch_fds(false);
#endif
    }
}

static void usb_host_events_resume(void)
{
    assert(events_paused > 0);
    if (--events_paused == 0) {
#ifndef CONFIG_WIN32
        usb_host_watch_fds(true);
#endif
    }
}

static int usb_host_init(void)
{
#ifndef CONFIG_WIN32
    const struct libusb_pollfd **poll;
#endif
    int rc;

    if (ctx) {
        return 0;
    }
    rc = libusb_init(&ctx);
    if (rc != 0) {
        return -1;
    }
#if LIBUSB_API_VERSION >= 0x01000106
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, loglevel);
#else
    libusb_set_debug(ctx, loglevel);
#endif
#ifdef CONFIG_WIN32
    poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usb_host_timer, NULL);
    usb_host_timer_kick();
#else
    libusb_set_pollfd_notifiers(ctx, usb_host_add_fd,
                                usb_host_del_fd,
                                ctx);
    poll = libusb_get_pollfds(ctx);
    if (poll) {
        int i;
        for (i = 0; poll[i] != NULL; i++) {
            usb_host_add_fd(poll[i]->fd, poll[i]->events, ctx);
        }
    }
    free(poll);
#endif
    return 0;
}

static int usb_host_get_port(libusb_device *dev, char *port, size_t len)
{
    uint8_t path[7];
    size_t off;
    int rc, i;

#if LIBUSB_API_VERSION >= 0x01000102
    rc = libusb_get_port_numbers(dev, path, 7);
#else
    rc = libusb_get_port_path(ctx, dev, path, 7);
#endif
    if (rc < 0) {
        return 0;
    }
    off = snprintf(port, len, "%d", path[0]);
    for (i = 1; i < rc; i++) {
        off += snprintf(port+off, len-off, ".%d", path[i]);
    }
    return off;
}

static void usb_host_libusb_error(const char *func, int rc)
{
    const char *errname;

    if (rc >= 0) {
        return;
    }

    if (-rc < ARRAY_SIZE(err_names) && err_names[-rc]) {
        errname = err_names[-rc];
    } else {
        errname = "?";
    }
    error_report("%s: %d [%s]", func, rc, errname);
}

/* ------------------------------------------------------------------------ */

static void usb_host_defer(USBHostDevice *s, struct libusb_transfer *xfer,
                           bool cancel)
{
    USBHostDeferred *d = g_new0(USBHostDeferred, 1);

    d->xfer = xfer;
    d->cancel = cancel;
    QSIMPLEQ_INSERT_TAIL(&s->deferred, d, next);
}

/* Drop what waits in the deferred queue for a transfer that is done */
static void usb_host_undefer(USBHostDevice *s, struct libusb_transfer *xfer)
{
    USBHostDeferred *d, *dn;

    QSIMPLEQ_FOREACH_SAFE(d, &s->deferred, next, dn) {
        if (d->xfer == xfer) {
            QSIMPLEQ_REMOVE(&s->deferred, d, USBHostDeferred, next);
            g_free(d);
        }
    }
}

/*
 * Complete a transfer that never reached the kernel through its callback, as
 * libusb completes one: with status, and nothing transferred.
 */
static void usb_host_xfer_unsent(struct libusb_transfer *xfer,
                                 enum libusb_transfer_status status)
{
    int i;

    xfer->status = status;
    xfer->actual_length = 0;
    for (i = 0; i < xfer->num_iso_packets; i++) {
        xfer->iso_packet_desc[i].status = status;
        xfer->iso_packet_desc[i].actual_length = 0;
    }
    xfer->callback(xfer);
}

/* libusb_submit_transfer, or its place in the deferred queue */
static int usb_host_submit(USBHostDevice *s, struct libusb_transfer *xfer)
{
    if (s->deferring) {
        usb_host_defer(s, xfer, false);
        return 0;
    }
    return libusb_submit_transfer(xfer);
}

/*
 * libusb_cancel_transfer, or its place in the deferred queue. A transfer
 * whose submit still waits there never reaches the kernel: it leaves the
 * queue and completes as canceled.
 */
static void usb_host_cancel(USBHostDevice *s, struct libusb_transfer *xfer)
{
    USBHostDeferred *d;

    if (!s->deferring) {
        libusb_cancel_transfer(xfer);
        return;
    }
    QSIMPLEQ_FOREACH(d, &s->deferred, next) {
        if (d->xfer == xfer && !d->cancel) {
            QSIMPLEQ_REMOVE(&s->deferred, d, USBHostDeferred, next);
            g_free(d);
            usb_host_xfer_unsent(xfer, LIBUSB_TRANSFER_CANCELLED);
            return;
        }
    }
    usb_host_defer(s, xfer, true);
}

/*
 * Make the submits and cancels that waited for a SET_INTERFACE call, in their
 * order, with whatever a completion here queues behind them. libusb's events
 * stay paused meanwhile, so no transfer completes, and none is freed, under a
 * cancel that waits. A submit that fails completes its transfer with the
 * error, as one that failed in the kernel would, and a cancel of it that
 * waits is dropped. A SET_INTERFACE started meanwhile (the guest canceled
 * this one) keeps what is left for its own call.
 */
static void usb_host_run_deferred(USBHostDevice *s)
{
    USBHostDeferred *d;
    struct libusb_transfer *xfer;
    bool cancel;
    int rc;

    while (!s->setintf && (d = QSIMPLEQ_FIRST(&s->deferred)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&s->deferred, next);
        xfer = d->xfer;
        cancel = d->cancel;
        g_free(d);
        if (cancel) {
            libusb_cancel_transfer(xfer);
            continue;
        }
        rc = libusb_submit_transfer(xfer);
        if (rc == 0) {
            continue;
        }
        usb_host_libusb_error("libusb_submit_transfer [deferred]", rc);
        usb_host_undefer(s, xfer);
        usb_host_xfer_unsent(xfer, rc == LIBUSB_ERROR_NO_DEVICE ?
                             LIBUSB_TRANSFER_NO_DEVICE :
                             LIBUSB_TRANSFER_ERROR);
        if (rc == LIBUSB_ERROR_NO_DEVICE) {
            usb_host_nodev(s);
        }
    }
    if (!s->setintf) {
        s->deferring = false;
    }
}

/* ------------------------------------------------------------------------ */

static bool usb_host_use_combining(USBEndpoint *ep)
{
    int type;

    if (!ep->pipeline) {
        return false;
    }
    if (ep->pid != USB_TOKEN_IN) {
        return false;
    }
    type = usb_ep_get_type(ep->dev, ep->pid, ep->nr);
    if (type != USB_ENDPOINT_XFER_BULK) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */

static USBHostRequest *usb_host_req_alloc(USBHostDevice *s, USBPacket *p,
                                          bool in, size_t bufsize)
{
    USBHostRequest *r = g_new0(USBHostRequest, 1);

    r->host = s;
    r->p = p;
    r->in = in;
    r->xfer = libusb_alloc_transfer(0);
    if (bufsize) {
        r->buffer = g_malloc(bufsize);
    }
    QTAILQ_INSERT_TAIL(&s->requests, r, next);
#ifdef CONFIG_WIN32
    request_count++;
    usb_host_timer_kick();
#endif
    return r;
}

static void usb_host_req_free(USBHostRequest *r)
{
#ifdef CONFIG_WIN32
    request_count--;
#endif
    QTAILQ_REMOVE(&r->host->requests, r, next);
    /*
     * A cancel can wait for a transfer that completed: one that libusb had
     * reaped in the same pass as a SET_INTERFACE request started.
     */
    usb_host_undefer(r->host, r->xfer);
    libusb_free_transfer(r->xfer);
    g_free(r->buffer);
    g_free(r);
}

static USBHostRequest *usb_host_req_find(USBHostDevice *s, USBPacket *p)
{
    USBHostRequest *r;

    QTAILQ_FOREACH(r, &s->requests, next) {
        if (r->p == p) {
            return r;
        }
    }
    return NULL;
}

static void LIBUSB_CALL usb_host_req_complete_ctrl(struct libusb_transfer *xfer)
{
    USBHostRequest *r = xfer->user_data;
    USBHostDevice  *s = r->host;
    bool disconnect = (xfer->status == LIBUSB_TRANSFER_NO_DEVICE);

    if (r->p == NULL) {
        goto out; /* request was canceled */
    }

    r->p->status = status_map[xfer->status];
    r->p->actual_length = xfer->actual_length;
    if (r->in && xfer->actual_length) {
        USBDevice *udev = USB_DEVICE(s);
        struct libusb_config_descriptor *conf = (void *)r->cbuf;
        memcpy(r->cbuf, r->buffer + 8, xfer->actual_length);

        /* Fix up USB-3 ep0 maxpacket size to allow superspeed connected devices
         * to work redirected to a not superspeed capable hcd */
        if (r->usb3ep0quirk && xfer->actual_length >= 18 &&
            r->cbuf[7] == 9) {
            r->cbuf[7] = 64;
        }
        /*
         *If this is GET_DESCRIPTOR request for configuration descriptor,
         * remove 'remote wakeup' flag from it to prevent idle power down
         * in Windows guest
         */
        if (s->suppress_remote_wake &&
            udev->setup_buf[0] == USB_DIR_IN &&
            udev->setup_buf[1] == USB_REQ_GET_DESCRIPTOR &&
            udev->setup_buf[3] == USB_DT_CONFIG && udev->setup_buf[2] == 0 &&
            xfer->actual_length >
                offsetof(struct libusb_config_descriptor, bmAttributes) &&
            (conf->bmAttributes & USB_CFG_ATT_WAKEUP)) {
                trace_usb_host_remote_wakeup_removed(s->bus_num, s->addr);
                conf->bmAttributes &= ~USB_CFG_ATT_WAKEUP;
        }
    }
    trace_usb_host_req_complete(s->bus_num, s->addr, r->p,
                                r->p->status, r->p->actual_length);
    usb_generic_async_ctrl_complete(USB_DEVICE(s), r->p);

out:
    usb_host_req_free(r);
    if (disconnect) {
        usb_host_nodev(s);
    }
}

static void LIBUSB_CALL usb_host_req_complete_data(struct libusb_transfer *xfer)
{
    USBHostRequest *r = xfer->user_data;
    USBHostDevice  *s = r->host;
    bool disconnect = (xfer->status == LIBUSB_TRANSFER_NO_DEVICE);

    if (r->p == NULL) {
        goto out; /* request was canceled */
    }

    r->p->status = status_map[xfer->status];
    if (r->in && xfer->actual_length) {
        usb_packet_copy(r->p, r->buffer, xfer->actual_length);
    }
    trace_usb_host_req_complete(s->bus_num, s->addr, r->p,
                                r->p->status, r->p->actual_length);
    if (usb_host_use_combining(r->p->ep)) {
        usb_combined_input_packet_complete(USB_DEVICE(s), r->p);
    } else {
        usb_packet_complete(USB_DEVICE(s), r->p);
    }

out:
    usb_host_req_free(r);
    if (disconnect) {
        usb_host_nodev(s);
    }
}

static void usb_host_req_abort(USBHostRequest *r)
{
    USBHostDevice  *s = r->host;
    bool inflight = (r->p && r->p->state == USB_PACKET_ASYNC);

    if (inflight) {
        r->p->status = USB_RET_NODEV;
        trace_usb_host_req_complete(s->bus_num, s->addr, r->p,
                                    r->p->status, r->p->actual_length);
        if (r->p->ep->nr == 0) {
            usb_generic_async_ctrl_complete(USB_DEVICE(s), r->p);
        } else {
            usb_packet_complete(USB_DEVICE(s), r->p);
        }
        r->p = NULL;

        usb_host_cancel(s, r->xfer);
    }
}

/* ------------------------------------------------------------------------ */

static void LIBUSB_CALL
usb_host_req_complete_iso(struct libusb_transfer *transfer)
{
    USBHostIsoXfer *xfer = transfer->user_data;

    if (!xfer) {
        /* USBHostIsoXfer released while inflight */
        g_free(transfer->buffer);
        libusb_free_transfer(transfer);
        return;
    }

    QTAILQ_REMOVE(&xfer->ring->inflight, xfer, next);
    if (QTAILQ_EMPTY(&xfer->ring->inflight)) {
        USBHostDevice *s = xfer->ring->host;
        trace_usb_host_iso_stop(s->bus_num, s->addr, xfer->ring->ep->nr);
    }
    if (xfer->ring->ep->pid == USB_TOKEN_IN) {
        QTAILQ_INSERT_TAIL(&xfer->ring->copy, xfer, next);
        if (!xfer->ring->primed) {
            xfer->ring->nr_primed++;
        }
        usb_wakeup(xfer->ring->ep, 0);
    } else {
        QTAILQ_INSERT_TAIL(&xfer->ring->unused, xfer, next);
        if (xfer->ring->out_waiting) {
            xfer->ring->out_waiting = false;
            usb_wakeup(xfer->ring->ep, 0);
        }
    }
}

/*
 * Packets in each transfer of an endpoint's ring: one millisecond of the
 * bus, at most isobsize. A high or super speed endpoint is serviced every
 * 2^(bInterval-1) microframes, eight to a millisecond, and a full speed one
 * every 2^(bInterval-1) frames, one to a millisecond; an endpoint serviced
 * less often than every millisecond gets one packet a transfer.
 */
static unsigned int usb_host_iso_xfer_packets(USBHostDevice *s,
                                              USBEndpoint *ep)
{
    USBDevice *udev = USB_DEVICE(s);
    unsigned int interval, packets = 1;

    if (udev->speed >= USB_SPEED_HIGH) {
        interval = s->ep_interval[ep->pid == USB_TOKEN_IN][ep->nr];
        interval = MIN(MAX(interval, 1), 4);
        packets = 8 >> (interval - 1);
    }
    return MAX(MIN(packets, s->iso_urb_frames), 1);
}

static USBHostIsoRing *usb_host_iso_alloc(USBHostDevice *s, USBEndpoint *ep)
{
    USBHostIsoRing *ring = g_new0(USBHostIsoRing, 1);
    USBHostIsoXfer *xfer;
    unsigned int packets = usb_host_iso_xfer_packets(s, ep);
    unsigned int i;

    ring->host = s;
    ring->ep = ep;
    /* the ring holds isobufs * isobsize packets in all */
    ring->nr_xfers = MAX(DIV_ROUND_UP(s->iso_urb_count * s->iso_urb_frames,
                                      packets), 1);
    QTAILQ_INIT(&ring->unused);
    QTAILQ_INIT(&ring->inflight);
    QTAILQ_INIT(&ring->copy);
    QTAILQ_INSERT_TAIL(&s->isorings, ring, next);

    for (i = 0; i < ring->nr_xfers; i++) {
        xfer = g_new0(USBHostIsoXfer, 1);
        xfer->ring = ring;
        xfer->xfer = libusb_alloc_transfer(packets);
        xfer->xfer->dev_handle = s->dh;
        xfer->xfer->type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;

        xfer->xfer->endpoint = ring->ep->nr;
        if (ring->ep->pid == USB_TOKEN_IN) {
            xfer->xfer->endpoint |= USB_DIR_IN;
        }
        xfer->xfer->callback = usb_host_req_complete_iso;
        xfer->xfer->user_data = xfer;

        xfer->xfer->num_iso_packets = packets;
        xfer->xfer->length = ring->ep->max_packet_size * packets;
        xfer->xfer->buffer = g_malloc0(xfer->xfer->length);

        QTAILQ_INSERT_TAIL(&ring->unused, xfer, next);
    }

    return ring;
}

static USBHostIsoRing *usb_host_iso_find(USBHostDevice *s, USBEndpoint *ep)
{
    USBHostIsoRing *ring;

    QTAILQ_FOREACH(ring, &s->isorings, next) {
        if (ring->ep == ep) {
            return ring;
        }
    }
    return NULL;
}

static void usb_host_iso_reset_xfer(USBHostIsoXfer *xfer)
{
    libusb_set_iso_packet_lengths(xfer->xfer,
                                  xfer->ring->ep->max_packet_size);
    xfer->packet = 0;
    xfer->copy_complete = false;
}

static void usb_host_iso_free_xfer(USBHostIsoXfer *xfer, bool inflight)
{
    if (inflight) {
        xfer->xfer->user_data = NULL;
    } else {
        g_free(xfer->xfer->buffer);
        libusb_free_transfer(xfer->xfer);
    }
    g_free(xfer);
}

static void usb_host_iso_free(USBHostIsoRing *ring)
{
    USBHostIsoXfer *xfer;

    while ((xfer = QTAILQ_FIRST(&ring->inflight)) != NULL) {
        QTAILQ_REMOVE(&ring->inflight, xfer, next);
        usb_host_iso_free_xfer(xfer, true);
    }
    while ((xfer = QTAILQ_FIRST(&ring->unused)) != NULL) {
        QTAILQ_REMOVE(&ring->unused, xfer, next);
        usb_host_iso_free_xfer(xfer, false);
    }
    while ((xfer = QTAILQ_FIRST(&ring->copy)) != NULL) {
        QTAILQ_REMOVE(&ring->copy, xfer, next);
        usb_host_iso_free_xfer(xfer, false);
    }

    QTAILQ_REMOVE(&ring->host->isorings, ring, next);
    g_free(ring);
}

static void usb_host_iso_free_all(USBHostDevice *s)
{
    USBHostIsoRing *ring;

    while ((ring = QTAILQ_FIRST(&s->isorings)) != NULL) {
        usb_host_iso_free(ring);
    }
}

static bool usb_host_iso_data_copy(USBHostIsoXfer *xfer, USBPacket *p)
{
    unsigned int psize;
    unsigned char *buf;

    buf = libusb_get_iso_packet_buffer_simple(xfer->xfer, xfer->packet);
    if (p->pid == USB_TOKEN_OUT) {
        psize = p->iov.size;
        if (psize > xfer->ring->ep->max_packet_size) {
            /* should not happen (guest bug) */
            psize = xfer->ring->ep->max_packet_size;
        }
        xfer->xfer->iso_packet_desc[xfer->packet].length = psize;
    } else {
        psize = xfer->xfer->iso_packet_desc[xfer->packet].actual_length;
        if (psize > p->iov.size) {
            /* should not happen (guest bug) */
            psize = p->iov.size;
        }
    }
    usb_packet_copy(p, buf, psize);
    xfer->packet++;
    xfer->copy_complete = (xfer->packet == xfer->xfer->num_iso_packets);
    return xfer->copy_complete;
}

static void usb_host_iso_data_in(USBHostDevice *s, USBPacket *p)
{
    USBHostIsoRing *ring;
    USBHostIsoXfer *xfer;
    bool disconnect = false;
    bool hold = false;
    int rc;

    ring = usb_host_iso_find(s, p->ep);
    if (ring == NULL) {
        ring = usb_host_iso_alloc(s, p->ep);
    }

    /*
     * A device sends a packet at each of its endpoint's service intervals
     * from the first, but the host's transfers bring them only as each
     * completes. Until two have, a controller that retries a NAKed packet
     * holds the guest's first packet, and the stream starts a transfer of
     * data ahead of the guest; other controllers get empty packets.
     */
    if (!ring->primed) {
        ring->primed = ring->nr_primed >= MIN(2, ring->nr_xfers) ||
                       !usb_bus_from_device(USB_DEVICE(s))->iso_nak_retry;
        hold = !ring->primed;
    }

    /* copy data to guest */
    xfer = hold ? NULL : QTAILQ_FIRST(&ring->copy);
    if (xfer == NULL && usb_bus_from_device(USB_DEVICE(s))->iso_nak_retry) {
        /*
         * The guest has caught up with the device: QEMU ran its packets
         * late, or ran them before handling transfers that had completed
         * meanwhile. The device's packet is on its way; wait for it.
         */
        hold = true;
    }
    if (xfer != NULL) {
        if (usb_host_iso_data_copy(xfer, p)) {
            QTAILQ_REMOVE(&ring->copy, xfer, next);
            QTAILQ_INSERT_TAIL(&ring->unused, xfer, next);
        }
    }

    /* submit empty bufs to host */
    while ((xfer = QTAILQ_FIRST(&ring->unused)) != NULL) {
        QTAILQ_REMOVE(&ring->unused, xfer, next);
        usb_host_iso_reset_xfer(xfer);
        rc = usb_host_submit(s, xfer->xfer);
        if (rc != 0) {
            usb_host_libusb_error("libusb_submit_transfer [iso]", rc);
            QTAILQ_INSERT_TAIL(&ring->unused, xfer, next);
            if (rc == LIBUSB_ERROR_NO_DEVICE) {
                disconnect = true;
            }
            break;
        }
        if (QTAILQ_EMPTY(&ring->inflight)) {
            trace_usb_host_iso_start(s->bus_num, s->addr, p->ep->nr);
        }
        QTAILQ_INSERT_TAIL(&ring->inflight, xfer, next);
    }

    if (hold && !QTAILQ_EMPTY(&ring->inflight)) {
        /* usb_host_req_complete_iso wakes the endpoint */
        p->status = USB_RET_NAK;
    }

    if (disconnect) {
        usb_host_nodev(s);
    }
}

static void usb_host_iso_data_out(USBHostDevice *s, USBPacket *p)
{
    USBHostIsoRing *ring;
    USBHostIsoXfer *xfer;
    bool disconnect = false;
    int rc, filled = 0;

    ring = usb_host_iso_find(s, p->ep);
    if (ring == NULL) {
        ring = usb_host_iso_alloc(s, p->ep);
    }

    /* copy data from guest */
    xfer = QTAILQ_FIRST(&ring->copy);
    while (xfer != NULL && xfer->copy_complete) {
        filled++;
        xfer = QTAILQ_NEXT(xfer, next);
    }
    if (xfer == NULL) {
        xfer = QTAILQ_FIRST(&ring->unused);
        if (xfer == NULL) {
            trace_usb_host_iso_out_of_bufs(s->bus_num, s->addr, p->ep->nr);
            /*
             * Every buffer is on its way to the device. A controller that
             * retries a NAKed packet holds this one until a transfer
             * completes and wakes the endpoint; other controllers complete
             * it with nothing sent.
             */
            if (usb_bus_from_device(USB_DEVICE(s))->iso_nak_retry &&
                !QTAILQ_EMPTY(&ring->inflight)) {
                ring->out_waiting = true;
                p->status = USB_RET_NAK;
            }
            return;
        }
        QTAILQ_REMOVE(&ring->unused, xfer, next);
        usb_host_iso_reset_xfer(xfer);
        QTAILQ_INSERT_TAIL(&ring->copy, xfer, next);
    }
    usb_host_iso_data_copy(xfer, p);

    if (QTAILQ_EMPTY(&ring->inflight)) {
        /* wait until half of our buffers are filled
           before kicking the iso out stream */
        if (filled * 2 < ring->nr_xfers) {
            return;
        }
    }

    /* submit filled bufs to host */
    while ((xfer = QTAILQ_FIRST(&ring->copy)) != NULL &&
           xfer->copy_complete) {
        QTAILQ_REMOVE(&ring->copy, xfer, next);
        rc = usb_host_submit(s, xfer->xfer);
        if (rc != 0) {
            usb_host_libusb_error("libusb_submit_transfer [iso]", rc);
            QTAILQ_INSERT_TAIL(&ring->unused, xfer, next);
            if (rc == LIBUSB_ERROR_NO_DEVICE) {
                disconnect = true;
            }
            break;
        }
        if (QTAILQ_EMPTY(&ring->inflight)) {
            trace_usb_host_iso_start(s->bus_num, s->addr, p->ep->nr);
        }
        QTAILQ_INSERT_TAIL(&ring->inflight, xfer, next);
    }

    if (disconnect) {
        usb_host_nodev(s);
    }
}

/* ------------------------------------------------------------------------ */

static void usb_host_speed_compat(USBHostDevice *s)
{
    USBDevice *udev = USB_DEVICE(s);
    struct libusb_config_descriptor *conf;
    const struct libusb_interface_descriptor *intf;
    const struct libusb_endpoint_descriptor *endp;
#ifdef HAVE_STREAMS
    struct libusb_ss_endpoint_companion_descriptor *endp_ss_comp;
#endif
    bool compat_high = true;
    bool compat_full = true;
    uint8_t type;
    int rc, c, i, a, e;

    for (c = 0;; c++) {
        rc = libusb_get_config_descriptor(s->dev, c, &conf);
        if (rc != 0) {
            break;
        }
        for (i = 0; i < conf->bNumInterfaces; i++) {
            for (a = 0; a < conf->interface[i].num_altsetting; a++) {
                intf = &conf->interface[i].altsetting[a];

                if (intf->bInterfaceClass == LIBUSB_CLASS_MASS_STORAGE &&
                    intf->bInterfaceSubClass == 6) { /* SCSI */
                    udev->flags |= (1 << USB_DEV_FLAG_IS_SCSI_STORAGE);
                    break;
                }

                for (e = 0; e < intf->bNumEndpoints; e++) {
                    endp = &intf->endpoint[e];
                    type = endp->bmAttributes & 0x3;
                    switch (type) {
                    case 0x01: /* ISO */
                        compat_full = false;
                        compat_high = false;
                        break;
                    case 0x02: /* BULK */
#ifdef HAVE_STREAMS
                        rc = libusb_get_ss_endpoint_companion_descriptor
                            (ctx, endp, &endp_ss_comp);
                        if (rc == LIBUSB_SUCCESS) {
                            int streams = endp_ss_comp->bmAttributes & 0x1f;
                            if (streams) {
                                compat_full = false;
                                compat_high = false;
                            }
                            libusb_free_ss_endpoint_companion_descriptor
                                (endp_ss_comp);
                        }
#endif
                        break;
                    case 0x03: /* INTERRUPT */
                        if (endp->wMaxPacketSize > 64) {
                            compat_full = false;
                        }
                        if (endp->wMaxPacketSize > 1024) {
                            compat_high = false;
                        }
                        break;
                    }
                }
            }
        }
        libusb_free_config_descriptor(conf);
    }

    udev->speedmask = (1 << udev->speed);
    if (udev->speed == USB_SPEED_SUPER && compat_high) {
        udev->speedmask |= USB_SPEED_MASK_HIGH;
    }
    if (udev->speed == USB_SPEED_SUPER && compat_full) {
        udev->speedmask |= USB_SPEED_MASK_FULL;
    }
    if (udev->speed == USB_SPEED_HIGH && compat_full) {
        udev->speedmask |= USB_SPEED_MASK_FULL;
    }
}

static void usb_host_ep_update(USBHostDevice *s)
{
    static const char *tname[] = {
        [USB_ENDPOINT_XFER_CONTROL] = "control",
        [USB_ENDPOINT_XFER_ISOC]    = "isoc",
        [USB_ENDPOINT_XFER_BULK]    = "bulk",
        [USB_ENDPOINT_XFER_INT]     = "int",
    };
    USBDevice *udev = USB_DEVICE(s);
    g_autoptr(libusb_config_descriptor) conf = NULL;
    const struct libusb_interface_descriptor *intf;
    const struct libusb_endpoint_descriptor *endp;
#ifdef HAVE_STREAMS
    struct libusb_ss_endpoint_companion_descriptor *endp_ss_comp;
#endif
    uint8_t devep, type;
    int pid, ep, alt;
    int rc, i, e;

    usb_ep_reset(udev);
    rc = libusb_get_active_config_descriptor(s->dev, &conf);
    if (rc != 0) {
        return;
    }
    trace_usb_host_parse_config(s->bus_num, s->addr,
                                conf->bConfigurationValue, true);

    for (i = 0; i < conf->bNumInterfaces; i++) {
        /*
         * The udev->altsetting array indexes alternate settings
         * by the interface number. Get the 0th alternate setting
         * first so that we can grab the interface number, and
         * then correct the alternate setting value if necessary.
         */
        intf = &conf->interface[i].altsetting[0];
        if (intf->bInterfaceNumber >= USB_MAX_INTERFACES) {
            trace_usb_host_parse_error(s->bus_num, s->addr,
                                       "invalid interface number");
            continue;
        }
        alt = udev->altsetting[intf->bInterfaceNumber];

        if (alt != 0) {
            assert(alt < conf->interface[i].num_altsetting);
            intf = &conf->interface[i].altsetting[alt];
        }

        trace_usb_host_parse_interface(s->bus_num, s->addr,
                                       intf->bInterfaceNumber,
                                       intf->bAlternateSetting, true);
        for (e = 0; e < intf->bNumEndpoints; e++) {
            endp = &intf->endpoint[e];

            devep = endp->bEndpointAddress;
            pid = (devep & USB_DIR_IN) ? USB_TOKEN_IN : USB_TOKEN_OUT;
            ep = devep & 0xf;
            type = endp->bmAttributes & 0x3;

            if (ep == 0) {
                trace_usb_host_parse_error(s->bus_num, s->addr,
                                           "invalid endpoint address");
                return;
            }
            if (usb_ep_get_type(udev, pid, ep) != USB_ENDPOINT_XFER_INVALID) {
                trace_usb_host_parse_error(s->bus_num, s->addr,
                                           "duplicate endpoint address");
                return;
            }

            trace_usb_host_parse_endpoint(s->bus_num, s->addr, ep,
                                          (devep & USB_DIR_IN) ? "in" : "out",
                                          tname[type], true);
            usb_ep_set_max_packet_size(udev, pid, ep,
                                       endp->wMaxPacketSize);
            usb_ep_set_type(udev, pid, ep, type);
            s->ep_interval[pid == USB_TOKEN_IN][ep] = endp->bInterval;
            usb_ep_set_ifnum(udev, pid, ep, intf->bInterfaceNumber);
            usb_ep_set_halted(udev, pid, ep, 0);
#ifdef HAVE_STREAMS
            if (type == LIBUSB_TRANSFER_TYPE_BULK &&
                    libusb_get_ss_endpoint_companion_descriptor(ctx, endp,
                        &endp_ss_comp) == LIBUSB_SUCCESS) {
                usb_ep_set_max_streams(udev, pid, ep,
                                       endp_ss_comp->bmAttributes);
                libusb_free_ss_endpoint_companion_descriptor(endp_ss_comp);
            }
#endif
        }
    }
}

static int usb_host_open(USBHostDevice *s, libusb_device *dev, int hostfd)
{
    USBDevice *udev = USB_DEVICE(s);
    int libusb_speed;
    int bus_num = 0;
    int addr = 0;
    int rc;
    Error *local_err = NULL;

    if (s->bh_postld_pending) {
        return -1;
    }
    if (s->dh != NULL) {
        goto fail;
    }

    if (dev) {
        bus_num = libusb_get_bus_number(dev);
        addr = libusb_get_device_address(dev);
        trace_usb_host_open_started(bus_num, addr);

        rc = libusb_open(dev, &s->dh);
        if (rc != 0) {
            goto fail;
        }
    } else {
#if LIBUSB_API_VERSION >= 0x01000107 && !defined(CONFIG_WIN32)
        trace_usb_host_open_hostfd(hostfd);

        rc = libusb_wrap_sys_device(ctx, hostfd, &s->dh);
        if (rc != 0) {
            goto fail;
        }
        s->hostfd  = hostfd;
        dev = libusb_get_device(s->dh);
        bus_num = libusb_get_bus_number(dev);
        addr = libusb_get_device_address(dev);
#else
        g_assert_not_reached();
#endif
    }

    s->dev     = dev;
    s->bus_num = bus_num;
    s->addr    = addr;

    usb_host_detach_kernel(s);

    libusb_get_device_descriptor(dev, &s->ddesc);
    usb_host_get_port(s->dev, s->port, sizeof(s->port));

    usb_ep_init(udev);
    usb_host_ep_update(s);

    libusb_speed = libusb_get_device_speed(dev);
#if LIBUSB_API_VERSION >= 0x01000107 && defined(CONFIG_LINUX) && \
        defined(USBDEVFS_GET_SPEED)
    if (hostfd && libusb_speed == 0) {
        /*
         * Workaround libusb bug: libusb_get_device_speed() does not
         * work for libusb_wrap_sys_device() devices in v1.0.23.
         *
         * Speeds are defined in linux/usb/ch9.h, file not included
         * due to name conflicts.
         */
        rc = ioctl(hostfd, USBDEVFS_GET_SPEED, NULL);
        switch (rc) {
        case 1: /* low */
            libusb_speed = LIBUSB_SPEED_LOW;
            break;
        case 2: /* full */
            libusb_speed = LIBUSB_SPEED_FULL;
            break;
        case 3: /* high */
        case 4: /* wireless */
            libusb_speed = LIBUSB_SPEED_HIGH;
            break;
        case 5: /* super */
            libusb_speed = LIBUSB_SPEED_SUPER;
            break;
        case 6: /* super plus */
#ifdef HAVE_SUPER_PLUS
            libusb_speed = LIBUSB_SPEED_SUPER_PLUS;
#else
            libusb_speed = LIBUSB_SPEED_SUPER;
#endif
            break;
        }
    }
#endif
    udev->speed = speed_map[libusb_speed];
    usb_host_speed_compat(s);

    if (s->ddesc.iProduct) {
        libusb_get_string_descriptor_ascii(s->dh, s->ddesc.iProduct,
                                           (unsigned char *)udev->product_desc,
                                           sizeof(udev->product_desc));
    } else {
        snprintf(udev->product_desc, sizeof(udev->product_desc),
                 "host:%d.%d", bus_num, addr);
    }

    usb_device_attach(udev, &local_err);
    if (local_err) {
        error_report_err(local_err);
        goto fail;
    }

    trace_usb_host_open_success(bus_num, addr);
    return 0;

fail:
    trace_usb_host_open_failure(bus_num, addr);
    if (s->dh != NULL) {
        usb_host_release_interfaces(s);
        libusb_reset_device(s->dh);
        usb_host_attach_kernel(s);
        libusb_close(s->dh);
        s->dh = NULL;
        s->dev = NULL;
    }
    return -1;
}

static void usb_host_abort_xfers(USBHostDevice *s)
{
    USBHostDevice *other;
    USBHostRequest *r, *rtmp;
    int limit = 100;

    QTAILQ_FOREACH_SAFE(r, &s->requests, next, rtmp) {
        usb_host_req_abort(r);
    }

    if (QTAILQ_FIRST(&s->requests) != NULL) {
        /*
         * libusb reaps every device's completed URBs below: no other
         * device's SET_INTERFACE call may still run, or anything wait for it.
         */
        QTAILQ_FOREACH(other, &hostdevs, next) {
            usb_host_set_interface_wait(other);
        }
    }

    while (QTAILQ_FIRST(&s->requests) != NULL) {
        struct timeval tv;
        memset(&tv, 0, sizeof(tv));
        tv.tv_usec = 2500;
        libusb_handle_events_timeout(ctx, &tv);
        if (--limit == 0) {
            /*
             * Don't wait forever for libusb calling the complete
             * callback (which will unlink and free the request).
             *
             * Leaking memory here, to make sure libusb will not
             * access memory which we have released already.
             */
            QTAILQ_FOREACH_SAFE(r, &s->requests, next, rtmp) {
                QTAILQ_REMOVE(&s->requests, r, next);
            }
            return;
        }
    }
}

static int usb_host_close(USBHostDevice *s)
{
    USBDevice *udev = USB_DEVICE(s);

    if (s->dh == NULL) {
        return -1;
    }

    trace_usb_host_close(s->bus_num, s->addr);

    usb_host_set_interface_wait(s);
    usb_host_abort_xfers(s);
    usb_host_iso_free_all(s);

    if (udev->attached) {
        usb_device_detach(udev);
    }

    usb_host_release_interfaces(s);
    libusb_reset_device(s->dh);
    usb_host_attach_kernel(s);
    libusb_close(s->dh);
    s->dh = NULL;
    s->dev = NULL;

    if (s->hostfd != -1) {
        close(s->hostfd);
        s->hostfd = -1;
    }

    usb_host_auto_check(NULL);
    return 0;
}

static void usb_host_nodev_bh(void *opaque)
{
    USBHostDevice *s = opaque;
    usb_host_close(s);
}

static void usb_host_nodev(USBHostDevice *s)
{
    if (!s->bh_nodev) {
        s->bh_nodev = qemu_bh_new_guarded(usb_host_nodev_bh, s,
                                          &DEVICE(s)->mem_reentrancy_guard);
    }
    qemu_bh_schedule(s->bh_nodev);
}

static void usb_host_exit_notifier(struct Notifier *n, void *data)
{
    USBHostDevice *s = container_of(n, USBHostDevice, exit);

    if (s->dh) {
        usb_host_set_interface_wait(s);
        usb_host_abort_xfers(s);
        usb_host_release_interfaces(s);
        libusb_reset_device(s->dh);
        usb_host_attach_kernel(s);
        libusb_close(s->dh);
    }
}

static libusb_device *usb_host_find_ref(int bus, int addr)
{
    libusb_device **devs = NULL;
    libusb_device *ret = NULL;
    int i, n;

    n = libusb_get_device_list(ctx, &devs);
    for (i = 0; i < n; i++) {
        if (libusb_get_bus_number(devs[i]) == bus &&
            libusb_get_device_address(devs[i]) == addr) {
            ret = libusb_ref_device(devs[i]);
            break;
        }
    }
    libusb_free_device_list(devs, 1);
    return ret;
}

static void usb_host_realize(USBDevice *udev, Error **errp)
{
    USBHostDevice *s = USB_HOST_DEVICE(udev);
    libusb_device *ldev;
    int rc;

    if (usb_host_init() != 0) {
        error_setg(errp, "failed to init libusb");
        return;
    }
    if (s->match.vendor_id > 0xffff) {
        error_setg(errp, "vendorid out of range");
        return;
    }
    if (s->match.product_id > 0xffff) {
        error_setg(errp, "productid out of range");
        return;
    }
    if (s->match.addr > 127) {
        error_setg(errp, "hostaddr out of range");
        return;
    }

    loglevel = s->loglevel;
    udev->flags |= (1 << USB_DEV_FLAG_IS_HOST);
    udev->auto_attach = 0;
    QTAILQ_INIT(&s->requests);
    QTAILQ_INIT(&s->isorings);
    QSIMPLEQ_INIT(&s->deferred);
    s->hostfd = -1;

#if LIBUSB_API_VERSION >= 0x01000107 && !defined(CONFIG_WIN32)
    if (s->hostdevice) {
        int fd;
        fd = qemu_open(s->hostdevice, O_RDWR, errp);
        if (fd < 0) {
            return;
        }
        rc = usb_host_open(s, NULL, fd);
        if (rc < 0) {
            error_setg(errp, "failed to open host usb device %s", s->hostdevice);
            return;
        }
    } else
#endif
    if (s->match.addr && s->match.bus_num &&
        !s->match.vendor_id &&
        !s->match.product_id &&
        !s->match.port) {
        ldev = usb_host_find_ref(s->match.bus_num,
                                 s->match.addr);
        if (!ldev) {
            error_setg(errp, "failed to find host usb device %d:%d",
                       s->match.bus_num, s->match.addr);
            return;
        }
        rc = usb_host_open(s, ldev, 0);
        libusb_unref_device(ldev);
        if (rc < 0) {
            error_setg(errp, "failed to open host usb device %d:%d",
                       s->match.bus_num, s->match.addr);
            return;
        }
    }

    s->exit.notify = usb_host_exit_notifier;
    qemu_add_exit_notifier(&s->exit);

    QTAILQ_INSERT_TAIL(&hostdevs, s, next);
    usb_host_auto_check(NULL);
}

static void usb_host_instance_init(Object *obj)
{
    USBDevice *udev = USB_DEVICE(obj);
    USBHostDevice *s = USB_HOST_DEVICE(udev);

    device_add_bootindex_property(obj, &s->bootindex,
                                  "bootindex", NULL,
                                  &udev->qdev);
}

static void usb_host_unrealize(USBDevice *udev)
{
    USBHostDevice *s = USB_HOST_DEVICE(udev);

    qemu_remove_exit_notifier(&s->exit);
    QTAILQ_REMOVE(&hostdevs, s, next);
    usb_host_close(s);
    /*
     * Delete bottom halves after usb_host_close().  During close,
     * libusb callbacks (usb_host_req_complete_ctrl/data) can call
     * usb_host_nodev() which lazily creates bh_nodev.  If we
     * deleted them before close, a newly created BH would leak and
     * fire after unrealize, dereferencing freed USBHostDevice.
     */
    if (s->bh_nodev) {
        qemu_bh_delete(s->bh_nodev);
        s->bh_nodev = NULL;
    }
    if (s->bh_postld) {
        qemu_bh_delete(s->bh_postld);
        s->bh_postld = NULL;
    }
}

static void usb_host_cancel_packet(USBDevice *udev, USBPacket *p)
{
    USBHostDevice *s = USB_HOST_DEVICE(udev);
    USBHostRequest *r;

    if (p->combined) {
        usb_combined_packet_cancel(udev, p);
        return;
    }

    trace_usb_host_req_canceled(s->bus_num, s->addr, p);

    if (s->setintf && s->setintf->p == p) {
        s->setintf->p = NULL; /* the call still runs, and its result applies */
        return;
    }

    r = usb_host_req_find(s, p);
    if (r && r->p) {
        r->p = NULL; /* mark as dead */
        usb_host_cancel(s, r->xfer);
    }
}

static void usb_host_detach_kernel(USBHostDevice *s)
{
    struct libusb_config_descriptor *conf;
    int rc, i;

    rc = libusb_get_active_config_descriptor(s->dev, &conf);
    if (rc != 0) {
        return;
    }
    for (i = 0; i < USB_MAX_INTERFACES; i++) {
        rc = libusb_kernel_driver_active(s->dh, i);
        usb_host_libusb_error("libusb_kernel_driver_active", rc);
        if (rc != 1) {
            if (rc == 0) {
                s->ifs[i].detached = true;
            }
            continue;
        }
        trace_usb_host_detach_kernel(s->bus_num, s->addr, i);
        rc = libusb_detach_kernel_driver(s->dh, i);
        usb_host_libusb_error("libusb_detach_kernel_driver", rc);
        s->ifs[i].detached = true;
    }
    libusb_free_config_descriptor(conf);
}

static void usb_host_attach_kernel(USBHostDevice *s)
{
    struct libusb_config_descriptor *conf;
    int rc, i;

    rc = libusb_get_active_config_descriptor(s->dev, &conf);
    if (rc != 0) {
        return;
    }
    for (i = 0; i < USB_MAX_INTERFACES; i++) {
        if (!s->ifs[i].detached) {
            continue;
        }
        trace_usb_host_attach_kernel(s->bus_num, s->addr, i);
        libusb_attach_kernel_driver(s->dh, i);
        s->ifs[i].detached = false;
    }
    libusb_free_config_descriptor(conf);
}

static int usb_host_claim_interfaces(USBHostDevice *s, int configuration)
{
    USBDevice *udev = USB_DEVICE(s);
    g_autoptr(libusb_config_descriptor) conf = NULL;
    int rc, i, claimed;

    for (i = 0; i < USB_MAX_INTERFACES; i++) {
        udev->altsetting[i] = 0;
    }
    udev->ninterfaces   = 0;
    udev->configuration = 0;

    usb_host_detach_kernel(s);

    rc = libusb_get_active_config_descriptor(s->dev, &conf);
    if (rc != 0) {
        if (rc == LIBUSB_ERROR_NOT_FOUND) {
            /* address state - ignore */
            return USB_RET_SUCCESS;
        }
        return USB_RET_STALL;
    }

    claimed = 0;
    for (i = 0; i < USB_MAX_INTERFACES; i++) {
        trace_usb_host_claim_interface(s->bus_num, s->addr, configuration, i);
        rc = libusb_claim_interface(s->dh, i);
        if (rc == 0) {
            s->ifs[i].claimed = true;
            if (++claimed == conf->bNumInterfaces) {
                break;
            }
        }
    }
    if (claimed != conf->bNumInterfaces) {
        return USB_RET_STALL;
    }

    udev->ninterfaces   = conf->bNumInterfaces;
    udev->configuration = configuration;

    return USB_RET_SUCCESS;
}

static void usb_host_release_interfaces(USBHostDevice *s)
{
    int i, rc;

    for (i = 0; i < USB_MAX_INTERFACES; i++) {
        if (!s->ifs[i].claimed) {
            continue;
        }
        trace_usb_host_release_interface(s->bus_num, s->addr, i);
        rc = libusb_release_interface(s->dh, i);
        usb_host_libusb_error("libusb_release_interface", rc);
        s->ifs[i].claimed = false;
    }
}

static void usb_host_set_address(USBHostDevice *s, int addr)
{
    USBDevice *udev = USB_DEVICE(s);

    trace_usb_host_set_address(s->bus_num, s->addr, addr);
    udev->addr = addr;
}

static void usb_host_set_config(USBHostDevice *s, int config, USBPacket *p)
{
    int rc = 0;

    trace_usb_host_set_config(s->bus_num, s->addr, config);

    usb_host_release_interfaces(s);
    if (s->ddesc.bNumConfigurations != 1) {
        rc = libusb_set_configuration(s->dh, config);
        if (rc != 0) {
            usb_host_libusb_error("libusb_set_configuration", rc);
            p->status = USB_RET_STALL;
            if (rc == LIBUSB_ERROR_NO_DEVICE) {
                usb_host_nodev(s);
            }
            return;
        }
    }
    p->status = usb_host_claim_interfaces(s, config);
    if (p->status != USB_RET_SUCCESS) {
        return;
    }
    usb_host_ep_update(s);
}

static int usb_host_set_interface_worker(void *opaque)
{
    USBHostSetIntf *job = opaque;

    job->rc = libusb_set_interface_alt_setting(job->dh, job->iface, job->alt);
    qemu_event_set(&job->done);
    return 0;
}

/* Apply what the worker's call did, and complete the guest's request */
static void usb_host_set_interface_finish(USBHostSetIntf *job)
{
    USBHostDevice *s = job->host;
    USBDevice *udev = USB_DEVICE(s);
    USBPacket *p = job->p;

    assert(s->setintf == job && !job->finished);
    s->setintf = NULL;
    job->finished = true;
    trace_usb_host_set_interface_done(s->bus_num, s->addr, job->iface,
                                      job->alt, job->rc);

    if (job->rc == 0) {
        udev->altsetting[job->iface] = job->alt;
        usb_host_ep_update(s);
    } else {
        usb_host_libusb_error("libusb_set_interface_alt_setting", job->rc);
    }
    /* what waited for the call, then libusb's events, whatever rc is */
    usb_host_run_deferred(s);
    usb_host_events_resume();
    if (p) {
        p->status = job->rc == 0 ? USB_RET_SUCCESS : USB_RET_STALL;
        trace_usb_host_req_emulated(s->bus_num, s->addr, p, p->status);
        usb_generic_async_ctrl_complete(udev, p);
    }
    if (job->rc == LIBUSB_ERROR_NO_DEVICE) {
        usb_host_nodev(s);
    }
}

static void usb_host_set_interface_complete(void *opaque, int ret)
{
    USBHostSetIntf *job = opaque;

    if (!job->finished) {
        usb_host_set_interface_finish(job);
    }
    qemu_event_destroy(&job->done);
    g_free(job);
}

/*
 * Wait for the SET_INTERFACE request in flight, if any, and apply it, and make
 * the submits and cancels that waited for it, before anything else changes
 * the device's state or closes its handle.
 */
static void usb_host_set_interface_wait(USBHostDevice *s)
{
    USBHostSetIntf *job = s->setintf;

    if (job) {
        qemu_event_wait(&job->done);
        usb_host_set_interface_finish(job);
    } else if (s->deferring) {
        usb_host_run_deferred(s); /* from a completion that run made */
    }
}

static void usb_host_set_interface(USBHostDevice *s, int iface, int alt,
                                   USBPacket *p)
{
    USBHostSetIntf *job;

    trace_usb_host_set_interface(s->bus_num, s->addr, iface, alt);

    usb_host_iso_free_all(s);

    if (iface >= USB_MAX_INTERFACES) {
        p->status = USB_RET_STALL;
        trace_usb_host_req_emulated(s->bus_num, s->addr, p, p->status);
        return;
    }

    job = g_new0(USBHostSetIntf, 1);
    job->host = s;
    job->p = p;
    job->dh = s->dh;
    job->iface = iface;
    job->alt = alt;
    qemu_event_init(&job->done, false);
    s->setintf = job;
    s->deferring = true;
    usb_host_events_pause();
    thread_pool_submit_aio(usb_host_set_interface_worker, job,
                           usb_host_set_interface_complete, job);
    p->status = USB_RET_ASYNC;
}

static void usb_host_handle_control(USBDevice *udev, USBPacket *p,
                                    int request, int value, int index,
                                    int length, uint8_t *data)
{
    USBHostDevice *s = USB_HOST_DEVICE(udev);
    USBHostRequest *r;
    int rc;

    trace_usb_host_req_control(s->bus_num, s->addr, p, request, value, index);

    if (s->dh == NULL) {
        p->status = USB_RET_NODEV;
        trace_usb_host_req_emulated(s->bus_num, s->addr, p, p->status);
        return;
    }

    /*
     * Control requests reach the device in the guest's order. One waits here
     * only when the guest canceled a SET_INTERFACE that still runs.
     */
    usb_host_set_interface_wait(s);

    switch (request) {
    case DeviceOutRequest | USB_REQ_SET_ADDRESS:
        usb_host_set_address(s, value);
        trace_usb_host_req_emulated(s->bus_num, s->addr, p, p->status);
        return;

    case DeviceOutRequest | USB_REQ_SET_CONFIGURATION:
        usb_host_set_config(s, value & 0xff, p);
        trace_usb_host_req_emulated(s->bus_num, s->addr, p, p->status);
        return;

    case InterfaceOutRequest | USB_REQ_SET_INTERFACE:
        usb_host_set_interface(s, index, value, p);
        return;

    case EndpointOutRequest | USB_REQ_CLEAR_FEATURE:
        if (value == 0) { /* clear halt */
            int pid = (index & USB_DIR_IN) ? USB_TOKEN_IN : USB_TOKEN_OUT;
            libusb_clear_halt(s->dh, index);
            usb_ep_set_halted(udev, pid, index & 0x0f, 0);
            trace_usb_host_req_emulated(s->bus_num, s->addr, p, p->status);
            return;
        }
    }

    r = usb_host_req_alloc(s, p, (request >> 8) & USB_DIR_IN, length + 8);
    r->cbuf = data;
    r->clen = length;
    memcpy(r->buffer, udev->setup_buf, 8);
    if (!r->in) {
        memcpy(r->buffer + 8, r->cbuf, r->clen);
    }

    /* Fix up USB-3 ep0 maxpacket size to allow superspeed connected devices
     * to work redirected to a not superspeed capable hcd */
    if ((udev->speedmask & USB_SPEED_MASK_SUPER) &&
        !(udev->port->speedmask & USB_SPEED_MASK_SUPER) &&
        request == 0x8006 && value == 0x100 && index == 0) {
        r->usb3ep0quirk = true;
    }

    libusb_fill_control_transfer(r->xfer, s->dh, r->buffer,
                                 usb_host_req_complete_ctrl, r,
                                 CONTROL_TIMEOUT);
    rc = usb_host_submit(s, r->xfer);
    if (rc != 0) {
        /*
         * Free the request to avoid a dangling r->p pointer.
         * When libusb_submit_transfer fails the caller (xHCI) may
         * free the XHCITransfer that holds the USBPacket r->p points
         * to, so we must remove r from s->requests before returning.
         */
        usb_host_req_free(r);
        p->status = USB_RET_NODEV;
        trace_usb_host_req_complete(s->bus_num, s->addr, p,
                                    p->status, p->actual_length);
        if (rc == LIBUSB_ERROR_NO_DEVICE) {
            usb_host_nodev(s);
        }
        return;
    }

    p->status = USB_RET_ASYNC;
}

static void usb_host_handle_data(USBDevice *udev, USBPacket *p)
{
    USBHostDevice *s = USB_HOST_DEVICE(udev);
    USBHostRequest *r;
    size_t size;
    int ep, rc;

    if (usb_host_use_combining(p->ep) && p->state == USB_PACKET_SETUP) {
        p->status = USB_RET_ADD_TO_QUEUE;
        return;
    }

    trace_usb_host_req_data(s->bus_num, s->addr, p,
                            p->pid == USB_TOKEN_IN,
                            p->ep->nr, p->iov.size);

    if (s->dh == NULL) {
        p->status = USB_RET_NODEV;
        trace_usb_host_req_emulated(s->bus_num, s->addr, p, p->status);
        return;
    }
    if (p->ep->halted) {
        p->status = USB_RET_STALL;
        trace_usb_host_req_emulated(s->bus_num, s->addr, p, p->status);
        return;
    }

    switch (usb_ep_get_type(udev, p->pid, p->ep->nr)) {
    case USB_ENDPOINT_XFER_BULK:
        size = usb_packet_size(p);
        r = usb_host_req_alloc(s, p, p->pid == USB_TOKEN_IN, size);
        if (!r->in) {
            usb_packet_copy(p, r->buffer, size);
        }
        ep = p->ep->nr | (r->in ? USB_DIR_IN : 0);
        if (p->stream) {
#ifdef HAVE_STREAMS
            libusb_fill_bulk_stream_transfer(r->xfer, s->dh, ep, p->stream,
                                             r->buffer, size,
                                             usb_host_req_complete_data, r,
                                             BULK_TIMEOUT);
#else
            usb_host_req_free(r);
            p->status = USB_RET_STALL;
            return;
#endif
        } else {
            libusb_fill_bulk_transfer(r->xfer, s->dh, ep,
                                      r->buffer, size,
                                      usb_host_req_complete_data, r,
                                      BULK_TIMEOUT);
        }
        break;
    case USB_ENDPOINT_XFER_INT:
        r = usb_host_req_alloc(s, p, p->pid == USB_TOKEN_IN, p->iov.size);
        if (!r->in) {
            usb_packet_copy(p, r->buffer, p->iov.size);
        }
        ep = p->ep->nr | (r->in ? USB_DIR_IN : 0);
        libusb_fill_interrupt_transfer(r->xfer, s->dh, ep,
                                       r->buffer, p->iov.size,
                                       usb_host_req_complete_data, r,
                                       INTR_TIMEOUT);
        break;
    case USB_ENDPOINT_XFER_ISOC:
        if (p->pid == USB_TOKEN_IN) {
            usb_host_iso_data_in(s, p);
        } else {
            usb_host_iso_data_out(s, p);
        }
        trace_usb_host_req_complete(s->bus_num, s->addr, p,
                                    p->status, p->actual_length);
        return;
    default:
        p->status = USB_RET_STALL;
        trace_usb_host_req_complete(s->bus_num, s->addr, p,
                                    p->status, p->actual_length);
        return;
    }

    rc = usb_host_submit(s, r->xfer);
    if (rc != 0) {
        /*
         * Free the request to avoid a dangling r->p pointer.
         * When libusb_submit_transfer fails the caller (xHCI) may
         * free the XHCITransfer that holds the USBPacket r->p points
         * to, so we must remove r from s->requests before returning.
         */
        usb_host_req_free(r);
        p->status = USB_RET_NODEV;
        trace_usb_host_req_complete(s->bus_num, s->addr, p,
                                    p->status, p->actual_length);
        if (rc == LIBUSB_ERROR_NO_DEVICE) {
            usb_host_nodev(s);
        }
        return;
    }

    p->status = USB_RET_ASYNC;
}

static void usb_host_flush_ep_queue(USBDevice *dev, USBEndpoint *ep)
{
    if (usb_host_use_combining(ep)) {
        usb_ep_combine_input_packets(ep);
    }
}

static void usb_host_handle_reset(USBDevice *udev)
{
    USBHostDevice *s = USB_HOST_DEVICE(udev);
    int rc;

    if (!s->allow_one_guest_reset && !s->allow_all_guest_resets) {
        return;
    }
    if (!s->allow_all_guest_resets && udev->addr == 0) {
        return;
    }

    trace_usb_host_reset(s->bus_num, s->addr);

    usb_host_set_interface_wait(s);
    rc = libusb_reset_device(s->dh);
    if (rc != 0) {
        usb_host_nodev(s);
    }
}

static int usb_host_alloc_streams(USBDevice *udev, USBEndpoint **eps,
                                  int nr_eps, int streams)
{
#ifdef HAVE_STREAMS
    USBHostDevice *s = USB_HOST_DEVICE(udev);
    unsigned char endpoints[30];
    int i, rc;

    usb_host_set_interface_wait(s);
    for (i = 0; i < nr_eps; i++) {
        endpoints[i] = eps[i]->nr;
        if (eps[i]->pid == USB_TOKEN_IN) {
            endpoints[i] |= 0x80;
        }
    }
    rc = libusb_alloc_streams(s->dh, streams, endpoints, nr_eps);
    if (rc < 0) {
        usb_host_libusb_error("libusb_alloc_streams", rc);
    } else if (rc != streams) {
        error_report("libusb_alloc_streams: got less streams "
                     "then requested %d < %d", rc, streams);
    }

    return (rc == streams) ? 0 : -1;
#else
    error_report("libusb_alloc_streams: error not implemented");
    return -1;
#endif
}

static void usb_host_free_streams(USBDevice *udev, USBEndpoint **eps,
                                  int nr_eps)
{
#ifdef HAVE_STREAMS
    USBHostDevice *s = USB_HOST_DEVICE(udev);
    unsigned char endpoints[30];
    int i;

    usb_host_set_interface_wait(s);
    for (i = 0; i < nr_eps; i++) {
        endpoints[i] = eps[i]->nr;
        if (eps[i]->pid == USB_TOKEN_IN) {
            endpoints[i] |= 0x80;
        }
    }
    libusb_free_streams(s->dh, endpoints, nr_eps);
#endif
}

/*
 * This is *NOT* about restoring state.  We have absolutely no idea
 * what state the host device is in at the moment and whenever it is
 * still present in the first place.  Attempting to continue where we
 * left off is impossible.
 *
 * What we are going to do here is emulate a surprise removal of
 * the usb device passed through, then kick host scan so the device
 * will get re-attached (and re-initialized by the guest) in case it
 * is still present.
 *
 * As the device removal will change the state of other devices (usb
 * host controller, most likely interrupt controller too) we have to
 * wait with it until *all* vmstate is loaded.  Thus post_load just
 * kicks a bottom half which then does the actual work.
 */
static void usb_host_post_load_bh(void *opaque)
{
    USBHostDevice *dev = opaque;
    USBDevice *udev = USB_DEVICE(dev);

    if (dev->dh != NULL) {
        usb_host_close(dev);
    }
    if (udev->attached) {
        usb_device_detach(udev);
    }
    dev->bh_postld_pending = false;
    usb_host_auto_check(NULL);
}

static int usb_host_post_load(void *opaque, int version_id)
{
    USBHostDevice *dev = opaque;

    if (!dev->bh_postld) {
        dev->bh_postld = qemu_bh_new_guarded(usb_host_post_load_bh, dev,
                                             &DEVICE(dev)->mem_reentrancy_guard);
    }
    qemu_bh_schedule(dev->bh_postld);
    dev->bh_postld_pending = true;
    return 0;
}

static const VMStateDescription vmstate_usb_host = {
    .name = "usb-host",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = usb_host_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_USB_DEVICE(parent_obj, USBHostDevice),
        VMSTATE_END_OF_LIST()
    }
};

static const Property usb_host_dev_properties[] = {
    DEFINE_PROP_UINT32("hostbus",  USBHostDevice, match.bus_num,    0),
    DEFINE_PROP_UINT32("hostaddr", USBHostDevice, match.addr,       0),
    DEFINE_PROP_STRING("hostport", USBHostDevice, match.port),
    DEFINE_PROP_UINT32("vendorid",  USBHostDevice, match.vendor_id,  0),
    DEFINE_PROP_UINT32("productid", USBHostDevice, match.product_id, 0),
#if LIBUSB_API_VERSION >= 0x01000107
    DEFINE_PROP_STRING("hostdevice", USBHostDevice, hostdevice),
#endif
    DEFINE_PROP_UINT32("isobufs",  USBHostDevice, iso_urb_count,    4),
    DEFINE_PROP_UINT32("isobsize", USBHostDevice, iso_urb_frames,   32),
    DEFINE_PROP_BOOL("guest-reset", USBHostDevice,
                     allow_one_guest_reset, true),
    DEFINE_PROP_BOOL("guest-resets-all", USBHostDevice,
                     allow_all_guest_resets, false),
    DEFINE_PROP_UINT32("loglevel",  USBHostDevice, loglevel,
                       LIBUSB_LOG_LEVEL_WARNING),
    DEFINE_PROP_BIT("pipeline",    USBHostDevice, options,
                    USB_HOST_OPT_PIPELINE, true),
    DEFINE_PROP_BOOL("suppress-remote-wake", USBHostDevice,
                     suppress_remote_wake, true),
};

static void usb_host_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize        = usb_host_realize;
    uc->product_desc   = "USB Host Device";
    uc->cancel_packet  = usb_host_cancel_packet;
    uc->handle_data    = usb_host_handle_data;
    uc->handle_control = usb_host_handle_control;
    uc->handle_reset   = usb_host_handle_reset;
    uc->unrealize      = usb_host_unrealize;
    uc->flush_ep_queue = usb_host_flush_ep_queue;
    uc->alloc_streams  = usb_host_alloc_streams;
    uc->free_streams   = usb_host_free_streams;
    dc->vmsd = &vmstate_usb_host;
    device_class_set_props(dc, usb_host_dev_properties);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo usb_host_dev_info = {
    .name          = TYPE_USB_HOST_DEVICE,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBHostDevice),
    .class_init    = usb_host_class_initfn,
    .instance_init = usb_host_instance_init,
};
module_obj(TYPE_USB_HOST_DEVICE);
module_kconfig(USB);

static void usb_host_register_types(void)
{
    type_register_static(&usb_host_dev_info);
#ifdef CONFIG_HMP
    monitor_register_hmp("usbhost", true, hmp_info_usbhost);
#endif
}

type_init(usb_host_register_types)

/* ------------------------------------------------------------------------ */

static QEMUTimer *usb_auto_timer;
static VMChangeStateEntry *usb_vmstate;

static void usb_host_vm_state(void *unused, bool running, RunState state)
{
    if (running) {
        usb_host_auto_check(unused);
    }
}

static void usb_host_auto_check(void *unused)
{
    struct USBHostDevice *s;
    struct USBAutoFilter *f;
    libusb_device **devs = NULL;
    struct libusb_device_descriptor ddesc;
    int i, n;

    if (usb_host_init() != 0) {
        return;
    }

    if (runstate_is_running()) {
        n = libusb_get_device_list(ctx, &devs);
        for (i = 0; i < n; i++) {
            if (libusb_get_device_descriptor(devs[i], &ddesc) != 0) {
                continue;
            }
            if (ddesc.bDeviceClass == LIBUSB_CLASS_HUB) {
                continue;
            }
            QTAILQ_FOREACH(s, &hostdevs, next) {
                f = &s->match;
                if (f->bus_num > 0 &&
                    f->bus_num != libusb_get_bus_number(devs[i])) {
                    continue;
                }
                if (f->addr > 0 &&
                    f->addr != libusb_get_device_address(devs[i])) {
                    continue;
                }
                if (f->port != NULL) {
                    char port[16] = "-";
                    usb_host_get_port(devs[i], port, sizeof(port));
                    if (strcmp(f->port, port) != 0) {
                        continue;
                    }
                }
                if (f->vendor_id > 0 &&
                    f->vendor_id != ddesc.idVendor) {
                    continue;
                }
                if (f->product_id > 0 &&
                    f->product_id != ddesc.idProduct) {
                    continue;
                }

                /* We got a match */
                s->seen++;
                if (s->errcount >= 3) {
                    continue;
                }
                if (s->dh != NULL) {
                    continue;
                }
                if (usb_host_open(s, devs[i], 0) < 0) {
                    s->errcount++;
                    continue;
                }
                s->errcount = 0;
                break;
            }
        }
        libusb_free_device_list(devs, 1);

        QTAILQ_FOREACH(s, &hostdevs, next) {
            if (s->seen == 0) {
                if (s->dh) {
                    usb_host_close(s);
                }
                s->errcount = 0;
            }
            s->seen = 0;
        }
    }

    if (!usb_vmstate) {
        usb_vmstate = qemu_add_vm_change_state_handler(usb_host_vm_state, NULL);
    }
    if (!usb_auto_timer) {
        usb_auto_timer = timer_new_ms(QEMU_CLOCK_REALTIME, usb_host_auto_check, NULL);
        if (!usb_auto_timer) {
            return;
        }
        trace_usb_host_auto_scan_enabled();
    }
    timer_mod(usb_auto_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
}

#ifdef CONFIG_HMP
void hmp_info_usbhost(MonitorHMP *hmp, const QDict *qdict)
{
    libusb_device **devs = NULL;
    struct libusb_device_descriptor ddesc;
    char port[16];
    int i, n;

    if (usb_host_init() != 0) {
        return;
    }

    n = libusb_get_device_list(ctx, &devs);
    for (i = 0; i < n; i++) {
        if (libusb_get_device_descriptor(devs[i], &ddesc) != 0) {
            continue;
        }
        if (ddesc.bDeviceClass == LIBUSB_CLASS_HUB) {
            continue;
        }
        usb_host_get_port(devs[i], port, sizeof(port));
        monitor_hmp_printf(hmp, "  Bus %d, Addr %d, Port %s, Speed %s Mb/s\n",
                           libusb_get_bus_number(devs[i]),
                           libusb_get_device_address(devs[i]),
                           port,
                           speed_name[libusb_get_device_speed(devs[i])]);
        monitor_hmp_printf(hmp, "    Class %02x:", ddesc.bDeviceClass);
        monitor_hmp_printf(hmp, " USB device %04x:%04x",
                           ddesc.idVendor, ddesc.idProduct);
        if (ddesc.iProduct) {
            libusb_device_handle *handle;
            if (libusb_open(devs[i], &handle) == 0) {
                unsigned char name[64] = "";
                libusb_get_string_descriptor_ascii(handle,
                                                   ddesc.iProduct,
                                                   name, sizeof(name));
                libusb_close(handle);
                monitor_hmp_printf(hmp, ", %s", name);
            }
        }
        monitor_hmp_printf(hmp, "\n");
    }
    libusb_free_device_list(devs, 1);
}
#endif
