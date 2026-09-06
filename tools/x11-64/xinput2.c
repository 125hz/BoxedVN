/* XInput2 client metadata; event delivery stays in BoxedWine's X server. */
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "boxedwine_x64_x11_bridge.h"
#define BW_EXPORT __attribute__((visibility("default")))
#define POINTER_ID 2
BW_EXPORT Status XIQueryVersion(Display *dpy, int *major, int *minor) {
    if (!major || !minor || *major < 2) return BadValue;
    *major = 2; *minor = 0; return Success;
}
BW_EXPORT Bool XIGetClientPointer(Display *dpy, Window win, int *device) {
    if (!device) return False;
    *device = POINTER_ID; return True;
}
struct PointerInfo {
    XIDeviceInfo info;
    XIAnyClassInfo *classes[2];
    XIValuatorClassInfo axes[2];
    char name[24];
};
BW_EXPORT XIDeviceInfo *XIQueryDevice(Display *dpy, int id, int *count) {
    if (!count) return NULL;
    *count = 0;
    if (id != POINTER_ID && id != XIAllDevices && id != XIAllMasterDevices) return NULL;
    struct PointerInfo *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    strcpy(p->name, "BoxedWine pointer");
    p->info = (XIDeviceInfo){POINTER_ID, p->name, XIMasterPointer, 3, True, 2, p->classes};
    for (int i=0; i<2; ++i) {
        p->axes[i].type=XIValuatorClass; p->axes[i].sourceid=POINTER_ID;
        p->axes[i].number=i; p->axes[i].resolution=1;
        p->axes[i].mode=XIModeRelative;
        p->classes[i]=(XIAnyClassInfo*)&p->axes[i];
    }
    *count=1; return &p->info;
}
BW_EXPORT void XIFreeDeviceInfo(XIDeviceInfo *info) { free(info); }
BW_EXPORT int XISelectEvents(Display *dpy, Window win, XIEventMask *masks, int count) {
    if (count < 0 || (count && !masks)) return BadValue;
    uint32_t selected=0;
    for (int i=0; i<count; ++i) {
        if (masks[i].mask_len < 0 || (masks[i].mask_len && !masks[i].mask)) return BadValue;
        if (masks[i].deviceid != POINTER_ID && masks[i].deviceid != XIAllDevices &&
            masks[i].deviceid != XIAllMasterDevices) return BadValue;
        for (int j=0; j<masks[i].mask_len && j<4; ++j)
            selected |= (uint32_t)masks[i].mask[j] << (8*j);
    }
    uint64_t args[]={(uintptr_t)dpy, win, selected};
    return (int)boxedwine_x64_x11_call(BOXEDWINE_X64_X11_OP_XI_SELECT_EVENTS,args,3);
}
