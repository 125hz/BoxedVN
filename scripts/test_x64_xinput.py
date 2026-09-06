"""Exercise the compiled guest XI2 metadata and raw-cookie ownership on Linux."""
import ctypes as c
from pathlib import Path
import sys

class Axis(c.Structure):
    _fields_=[("type",c.c_int),("source",c.c_int),("number",c.c_int),
              ("label",c.c_ulong),("min",c.c_double),("max",c.c_double),
              ("value",c.c_double),("resolution",c.c_int),("mode",c.c_int)]
class Device(c.Structure):
    _fields_=[("id",c.c_int),("name",c.c_char_p),("use",c.c_int),
              ("attachment",c.c_int),("enabled",c.c_int),("count",c.c_int),
              ("classes",c.POINTER(c.POINTER(Axis)))]
class Cookie(c.Structure):
    _fields_=[("type",c.c_int),("serial",c.c_ulong),("send",c.c_int),
              ("display",c.c_void_p),("extension",c.c_int),("evtype",c.c_int),
              ("cookie",c.c_uint),("data",c.c_void_p)]
class Valuators(c.Structure):
    _fields_=[("length",c.c_int),("mask",c.POINTER(c.c_ubyte)),
              ("values",c.POINTER(c.c_double))]
class Raw(c.Structure):
    _fields_=Cookie._fields_[:6]+[("time",c.c_ulong),("device",c.c_int),
        ("source",c.c_int),("detail",c.c_int),("flags",c.c_int),
        ("valuators",Valuators),("raw",c.POINTER(c.c_double))]
root=Path(sys.argv[1]).resolve()
xi=c.CDLL(str(root/"libXi.so.6")); xlib=c.CDLL(str(root/"libX11.so.6"))
xi.XIQueryDevice.argtypes=[c.c_void_p,c.c_int,c.POINTER(c.c_int)]
xi.XIQueryDevice.restype=c.POINTER(Device)
xi.XIFreeDeviceInfo.argtypes=[c.POINTER(Device)]
count=c.c_int()
d=xi.XIQueryDevice(None,2,c.byref(count))
assert count.value==1 and d.contents.id==2 and d.contents.enabled
assert d.contents.count==2
for i in range(2):
    axis=d.contents.classes[i].contents
    assert axis.number==i and axis.mode==0 and axis.min==axis.max
xi.XIFreeDeviceInfo(d)
assert not xi.XIQueryDevice(None,999,c.byref(count)) and count.value==0
xlib.XGetEventData.argtypes=[c.c_void_p,c.POINTER(Cookie)]
xlib.XFreeEventData.argtypes=[c.c_void_p,c.POINTER(Cookie)]
for dx,dy in [(31,-17),(-2147483648,2147483647),(0,0)]:
    cookie=Cookie(35,91,0,None,131,17,123456,(dx&0xffffffff)|((dy&0xffffffff)<<32))
    assert xlib.XGetEventData(None,c.byref(cookie))==1
    raw=c.cast(cookie.data,c.POINTER(Raw)).contents
    assert raw.time==123456 and raw.serial==91 and raw.device==2
    assert raw.valuators.length==1 and raw.valuators.mask[0]==3
    assert tuple(raw.valuators.values[i] for i in range(2))==(dx,dy)
    assert tuple(raw.raw[i] for i in range(2))==(dx,dy)
    xlib.XFreeEventData(None,c.byref(cookie)); assert not cookie.data
print("XI2 device metadata, signed deltas, timestamps and cookie ownership passed")
