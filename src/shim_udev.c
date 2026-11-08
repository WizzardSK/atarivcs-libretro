/* A libudev that knows one device: the shim's pad. Rewired's native Linux
 * input (Rewired_Linux.dll, Unity games such as Breakout Recharged) finds
 * its pads only through libudev, which the real one answers with the
 * machine's devices and never the shim's; without libudev it falls back to
 * Unity's input, which did not see the pad either. So when the game opens
 * libudev it gets this: an input device event250 with its input250 parent
 * and a USB parent, as an Xbox 360 pad (xpad) shows in udev. The other
 * functions Rewired binds answer nothing. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include "shim_pad.h"

#define EXPORT __attribute__((visibility("default")))

char atarivcs_fake_udev_handle;

typedef struct entry {
    const char *name, *value;
    struct entry *next;
} entry;

typedef struct device {
    const char *syspath, *sysname, *devpath, *subsystem, *devtype, *devnode;
    dev_t devnum;
    struct device *parent;
    entry *props, *attrs;
} device;

/* The USB device */
static entry usb_attrs[] = {
    { "idVendor", "045e", &usb_attrs[1] },
    { "idProduct", "028e", &usb_attrs[2] },
    { "bcdDevice", "0114", &usb_attrs[3] },
    { "manufacturer", "Microsoft", &usb_attrs[4] },
    { "product", "Controller", &usb_attrs[5] },
    { "serial", "ATARIVCS0001", NULL },
};
static entry usb_props[] = {
    { "SUBSYSTEM", "usb", &usb_props[1] },
    { "DEVTYPE", "usb_device", &usb_props[2] },
    { "ID_VENDOR_ID", "045e", &usb_props[3] },
    { "ID_MODEL_ID", "028e", NULL },
};
static device usb_dev = {
    "/sys/devices/virtual/atarivcs/usb1/1-1", "1-1", "/devices/virtual/atarivcs/usb1/1-1",
    "usb", "usb_device", NULL, 0, NULL, usb_props, usb_attrs,
};

/* The input device */
static entry input_attrs[] = {
    { "name", "Microsoft X-Box 360 pad", &input_attrs[1] },
    { "id/bustype", "0003", &input_attrs[2] },
    { "id/vendor", "045e", &input_attrs[3] },
    { "id/product", "028e", &input_attrs[4] },
    { "id/version", "0114", &input_attrs[5] },
    { "phys", "usb-atarivcs-1/input0", &input_attrs[6] },
    { "uniq", "", NULL },
};
static entry input_props[] = {
    { "SUBSYSTEM", "input", &input_props[1] },
    { "NAME", "\"Microsoft X-Box 360 pad\"", &input_props[2] },
    { "ID_INPUT", "1", &input_props[3] },
    { "ID_INPUT_JOYSTICK", "1", &input_props[4] },
    { "ID_BUS", "usb", &input_props[5] },
    { "ID_VENDOR_ID", "045e", &input_props[6] },
    { "ID_MODEL_ID", "028e", NULL },
};
static device input_dev = {
    "/sys/devices/virtual/atarivcs/usb1/1-1/input/input250", "input250",
    "/devices/virtual/atarivcs/usb1/1-1/input/input250", "input", NULL, NULL, 0, &usb_dev, input_props, input_attrs,
};

/* The event node the game opens */
static entry event_props[] = {
    { "SUBSYSTEM", "input", &event_props[1] },
    { "DEVNAME", PAD_PATH, &event_props[2] },
    { "MAJOR", "13", &event_props[3] },
    { "MINOR", "1023", &event_props[4] },
    { "ID_INPUT", "1", &event_props[5] },
    { "ID_INPUT_JOYSTICK", "1", &event_props[6] },
    { "ID_BUS", "usb", &event_props[7] },
    { "ID_VENDOR_ID", "045e", &event_props[8] },
    { "ID_MODEL_ID", "028e", &event_props[9] },
    { "ID_SERIAL", "Microsoft_Controller_ATARIVCS0001", NULL },
};
static device event_dev = {
    "/sys/devices/virtual/atarivcs/usb1/1-1/input/input250/" PAD_SYSNAME, PAD_SYSNAME,
    "/devices/virtual/atarivcs/usb1/1-1/input/input250/" PAD_SYSNAME, "input", NULL, PAD_PATH, 0, &input_dev,
    event_props, NULL,
};

static device *devices[] = { &event_dev, &input_dev, &usb_dev };

static device *by_syspath(const char *path)
{
    for (size_t i = 0; path && i < sizeof(devices) / sizeof(devices[0]); i++)
        if (!strcmp(devices[i]->syspath, path))
            return devices[i];
    return NULL;
}

static const char *lookup(const entry *e, const char *name)
{
    for (; e && name; e = e->next)
        if (!strcmp(e->name, name))
            return e->value;
    return NULL;
}

/* ---- udev ---- */

static int udev_ctx;

static void *f_udev_new(void) { return &udev_ctx; }
static void *f_udev_ref(void *u) { return u; }
static void *f_udev_unref(void *u) { (void)u; return NULL; }
static const char *f_udev_get_sys_path(void *u) { (void)u; return "/sys"; }
static const char *f_udev_get_dev_path(void *u) { (void)u; return "/dev"; }
static const char *f_udev_get_run_path(void *u) { (void)u; return "/run/udev"; }

/* ---- Enumerate: the devices of the subsystem asked for ---- */

typedef struct {
    char subsystem[32];
    entry list[3];
} enumerate;
static enumerate the_enum;

static void *f_enumerate_new(void *u)
{
    (void)u;
    memset(&the_enum, 0, sizeof(the_enum));
    return &the_enum;
}
static void *f_enumerate_ref(void *e) { return e; }
static void *f_enumerate_unref(void *e) { (void)e; return NULL; }
static void *f_enumerate_get_udev(void *e) { (void)e; return &udev_ctx; }

static int f_enumerate_add_match_subsystem(void *e, const char *subsystem)
{
    enumerate *en = e;
    snprintf(en->subsystem, sizeof(en->subsystem), "%s", subsystem ? subsystem : "");
    return 0;
}

static int f_enumerate_ok(void) { return 0; }

static int f_enumerate_scan_devices(void *e)
{
    enumerate *en = e;
    entry *prev = NULL;
    size_t n = 0;
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        if (en->subsystem[0] && strcmp(en->subsystem, devices[i]->subsystem))
            continue;
        en->list[n] = (entry){ devices[i]->syspath, NULL, NULL };
        if (prev)
            prev->next = &en->list[n];
        prev = &en->list[n];
        n++;
    }
    if (!n)
        en->list[0].name = NULL;
    return 0;
}

static void *f_enumerate_get_list_entry(void *e)
{
    enumerate *en = e;
    return en->list[0].name ? &en->list[0] : NULL;
}

/* ---- List entries ---- */

static void *f_list_entry_get_next(void *e) { return e ? ((entry *)e)->next : NULL; }
static const char *f_list_entry_get_name(void *e) { return e ? ((entry *)e)->name : NULL; }
static const char *f_list_entry_get_value(void *e) { return e ? ((entry *)e)->value : NULL; }

static void *f_list_entry_get_by_name(void *e, const char *name)
{
    for (entry *x = e; x && name; x = x->next)
        if (!strcmp(x->name, name))
            return x;
    return NULL;
}

/* ---- Devices ---- */

static void *f_device_new_from_syspath(void *u, const char *path)
{
    (void)u;
    return by_syspath(path);
}

static void *f_device_new_from_subsystem_sysname(void *u, const char *subsystem, const char *sysname)
{
    (void)u;
    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++)
        if (subsystem && sysname && !strcmp(devices[i]->subsystem, subsystem) && !strcmp(devices[i]->sysname, sysname))
            return devices[i];
    return NULL;
}

static void *f_device_new_from_devnum(void *u, char type, dev_t devnum)
{
    (void)u;
    (void)type;
    return devnum == makedev(PAD_MAJOR, PAD_MINOR) ? &event_dev : NULL;
}

static void *f_device_ref(void *d) { return d; }
static void *f_device_unref(void *d) { (void)d; return NULL; }
static void *f_device_get_udev(void *d) { (void)d; return &udev_ctx; }
static const char *f_device_get_syspath(void *d) { return d ? ((device *)d)->syspath : NULL; }
static const char *f_device_get_sysname(void *d) { return d ? ((device *)d)->sysname : NULL; }
static const char *f_device_get_devpath(void *d) { return d ? ((device *)d)->devpath : NULL; }
static const char *f_device_get_subsystem(void *d) { return d ? ((device *)d)->subsystem : NULL; }
static const char *f_device_get_devtype(void *d) { return d ? ((device *)d)->devtype : NULL; }
static const char *f_device_get_devnode(void *d) { return d ? ((device *)d)->devnode : NULL; }
static const char *f_device_get_driver(void *d) { (void)d; return NULL; }
static const char *f_device_get_action(void *d) { (void)d; return NULL; }
static const char *f_device_get_sysnum(void *d)
{
    const char *s = f_device_get_sysname(d);
    if (!s)
        return NULL;
    const char *p = s + strlen(s);
    while (p > s && p[-1] >= '0' && p[-1] <= '9')
        p--;
    return *p ? p : NULL;
}
static dev_t f_device_get_devnum(void *d) { return d == &event_dev ? makedev(PAD_MAJOR, PAD_MINOR) : 0; }
static void *f_device_get_parent(void *d) { return d ? ((device *)d)->parent : NULL; }

static void *f_device_get_parent_with_subsystem_devtype(void *d, const char *subsystem, const char *devtype)
{
    for (device *p = d ? ((device *)d)->parent : NULL; p; p = p->parent)
        if ((!subsystem || !strcmp(p->subsystem, subsystem)) &&
            (!devtype || (p->devtype && !strcmp(p->devtype, devtype))))
            return p;
    return NULL;
}

static const char *f_device_get_property_value(void *d, const char *key)
{
    return d ? lookup(((device *)d)->props, key) : NULL;
}

static const char *f_device_get_sysattr_value(void *d, const char *name)
{
    return d ? lookup(((device *)d)->attrs, name) : NULL;
}

static void *f_device_get_properties_list_entry(void *d) { return d ? ((device *)d)->props : NULL; }
static void *f_device_get_sysattr_list_entry(void *d) { return d ? ((device *)d)->attrs : NULL; }
static int f_device_get_is_initialized(void *d) { (void)d; return 1; }
static unsigned long long f_device_get_usec_since_initialized(void *d) { (void)d; return 1000000; }

/* ---- Monitor: nothing is ever plugged in or out ---- */

static int monitor_fds[2] = { -1, -1 };
static int the_monitor;

static void *f_monitor_new(void *u, const char *name)
{
    (void)u;
    (void)name;
    if (monitor_fds[0] < 0 && pipe(monitor_fds) != 0)
        return NULL;
    return &the_monitor;
}
static void *f_monitor_ref(void *m) { return m; }
static void *f_monitor_unref(void *m) { (void)m; return NULL; }
static void *f_monitor_get_udev(void *m) { (void)m; return &udev_ctx; }
static int f_monitor_get_fd(void *m) { (void)m; return monitor_fds[0]; }
static void *f_monitor_receive_device(void *m) { (void)m; return NULL; }

/* ---- Everything else: nothing ---- */

static void *f_null(void) { return NULL; }

static const struct {
    const char *name;
    void *fn;
} table[] = {
    { "udev_new", (void *)f_udev_new },
    { "udev_ref", (void *)f_udev_ref },
    { "udev_unref", (void *)f_udev_unref },
    { "udev_get_sys_path", (void *)f_udev_get_sys_path },
    { "udev_get_dev_path", (void *)f_udev_get_dev_path },
    { "udev_get_run_path", (void *)f_udev_get_run_path },
    { "udev_enumerate_new", (void *)f_enumerate_new },
    { "udev_enumerate_ref", (void *)f_enumerate_ref },
    { "udev_enumerate_unref", (void *)f_enumerate_unref },
    { "udev_enumerate_get_udev", (void *)f_enumerate_get_udev },
    { "udev_enumerate_add_match_subsystem", (void *)f_enumerate_add_match_subsystem },
    { "udev_enumerate_add_nomatch_subsystem", (void *)f_enumerate_ok },
    { "udev_enumerate_add_match_sysattr", (void *)f_enumerate_ok },
    { "udev_enumerate_add_nomatch_sysattr", (void *)f_enumerate_ok },
    { "udev_enumerate_add_match_property", (void *)f_enumerate_ok },
    { "udev_enumerate_add_match_sysname", (void *)f_enumerate_ok },
    { "udev_enumerate_add_match_tag", (void *)f_enumerate_ok },
    { "udev_enumerate_add_match_parent", (void *)f_enumerate_ok },
    { "udev_enumerate_add_match_is_initialized", (void *)f_enumerate_ok },
    { "udev_enumerate_add_syspath", (void *)f_enumerate_ok },
    { "udev_enumerate_scan_devices", (void *)f_enumerate_scan_devices },
    { "udev_enumerate_scan_subsystems", (void *)f_enumerate_ok },
    { "udev_enumerate_get_list_entry", (void *)f_enumerate_get_list_entry },
    { "udev_list_entry_get_next", (void *)f_list_entry_get_next },
    { "udev_list_entry_get_name", (void *)f_list_entry_get_name },
    { "udev_list_entry_get_value", (void *)f_list_entry_get_value },
    { "udev_list_entry_get_by_name", (void *)f_list_entry_get_by_name },
    { "udev_device_new_from_syspath", (void *)f_device_new_from_syspath },
    { "udev_device_new_from_subsystem_sysname", (void *)f_device_new_from_subsystem_sysname },
    { "udev_device_new_from_devnum", (void *)f_device_new_from_devnum },
    { "udev_device_ref", (void *)f_device_ref },
    { "udev_device_unref", (void *)f_device_unref },
    { "udev_device_get_udev", (void *)f_device_get_udev },
    { "udev_device_get_syspath", (void *)f_device_get_syspath },
    { "udev_device_get_sysname", (void *)f_device_get_sysname },
    { "udev_device_get_sysnum", (void *)f_device_get_sysnum },
    { "udev_device_get_devpath", (void *)f_device_get_devpath },
    { "udev_device_get_subsystem", (void *)f_device_get_subsystem },
    { "udev_device_get_devtype", (void *)f_device_get_devtype },
    { "udev_device_get_devnode", (void *)f_device_get_devnode },
    { "udev_device_get_devnum", (void *)f_device_get_devnum },
    { "udev_device_get_driver", (void *)f_device_get_driver },
    { "udev_device_get_action", (void *)f_device_get_action },
    { "udev_device_get_parent", (void *)f_device_get_parent },
    { "udev_device_get_parent_with_subsystem_devtype", (void *)f_device_get_parent_with_subsystem_devtype },
    { "udev_device_get_property_value", (void *)f_device_get_property_value },
    { "udev_device_get_sysattr_value", (void *)f_device_get_sysattr_value },
    { "udev_device_get_properties_list_entry", (void *)f_device_get_properties_list_entry },
    { "udev_device_get_sysattr_list_entry", (void *)f_device_get_sysattr_list_entry },
    { "udev_device_get_is_initialized", (void *)f_device_get_is_initialized },
    { "udev_device_get_usec_since_initialized", (void *)f_device_get_usec_since_initialized },
    { "udev_monitor_new_from_netlink", (void *)f_monitor_new },
    { "udev_monitor_ref", (void *)f_monitor_ref },
    { "udev_monitor_unref", (void *)f_monitor_unref },
    { "udev_monitor_get_udev", (void *)f_monitor_get_udev },
    { "udev_monitor_get_fd", (void *)f_monitor_get_fd },
    { "udev_monitor_receive_device", (void *)f_monitor_receive_device },
    { "udev_monitor_enable_receiving", (void *)f_enumerate_ok },
    { "udev_monitor_filter_add_match_subsystem_devtype", (void *)f_enumerate_ok },
    { "udev_monitor_filter_add_match_tag", (void *)f_enumerate_ok },
    { "udev_monitor_filter_update", (void *)f_enumerate_ok },
    { "udev_monitor_filter_remove", (void *)f_enumerate_ok },
    { "udev_monitor_set_receive_buffer_size", (void *)f_enumerate_ok },
};

void *atarivcs_fake_udev_symbol(const char *name)
{
    static int logged;
    if (!name || strncmp(name, "udev_", 5))
        return NULL;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (!strcmp(table[i].name, name))
            return table[i].fn;
    /* Mono's P/Invoke also looks for the name with W or A after it */
    const size_t len = strlen(name);
    if (name[len - 1] == 'W' || name[len - 1] == 'A') {
        char bare[128];
        if (len - 1 < sizeof(bare)) {
            memcpy(bare, name, len - 1);
            bare[len - 1] = '\0';
            for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
                if (!strcmp(table[i].name, bare))
                    return table[i].fn;
        }
    }
    if (logged++ < 20)
        fprintf(stderr, "atarivcs shim: udev %s answers nothing\n", name);
    return (void *)f_null;
}
