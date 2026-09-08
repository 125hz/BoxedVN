"""Exercise production X11 pixel operations against clipped and overlapping images."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
source = (repo / 'source/x11/xdrawable.cpp').read_text()
def method(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
code = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <vector>
#include <unordered_map>
using U8=uint8_t; using U32=uint32_t; using U64=uint64_t; using S32=int32_t; using S64=int64_t;
enum {Success, BadMatch, BadValue, BadImplementation};
enum {GXclear,GXand,GXandReverse,GXcopy,GXandInverted,GXnoop,GXxor,GXor,GXnor,GXequiv,GXinvert,GXorReverse,GXcopyInverted,GXorInverted,GXnand,GXset};
enum {CapNotLast=0,FillSolid=0,FillStippled=2,FillOpaqueStippled=3};
#define BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(m)
#define klog_fmt(...)
#define kwarn_fmt(...)
struct KThread {};
struct XGC {
 struct { U32 clip_mask=0,foreground=0x123456,background=0,plane_mask=~0u,stipple=0,tile=0;
 S32 clip_x_origin=0,clip_y_origin=0,function=GXcopy,cap_style=1,fill_style=0,ts_x_origin=0,ts_y_origin=0;} values;
 struct Rect {S32 x,y;U32 width,height;};
 bool clipRectsSet=false; std::vector<Rect> clip_rects;
};
struct Visual {U32 bits_per_rgb=32;};
struct XDrawable {
 U32 w,h,bytes_per_line,id=1;std::vector<U8> storage;U8* data;
 std::shared_ptr<Visual> visual=std::make_shared<Visual>();int dirty=0;
 XDrawable(U32 w,U32 h):w(w),h(h),bytes_per_line(w*4),storage(w*h*4),data(storage.data()){}
 U32 width(){return w;} U32 height(){return h;} U32 getBytesPerLine(){return bytes_per_line;}
 U32 getBitsPerPixel(){return visual->bits_per_rgb;} U8* getData(){return data;}
 void lockData(){} void unlockData(){} void setDirtyRect(S32,S32,U32,U32){++dirty;}
 U32& pixel(U32 x,U32 y){return ((U32*)data)[y*w+x];}
 int copyHostImageData(const std::shared_ptr<XGC>&,const U8*,U32,U32,S32,S32,S32,S32,S32,U32,U32);
 int copy(KThread*,const std::shared_ptr<XGC>&,const std::shared_ptr<XDrawable>&,S32,S32,U32,U32,S32,S32);
 int drawLine(KThread*,const std::shared_ptr<XGC>&,S32,S32,S32,S32);
 int fillRectangle(KThread*,const std::shared_ptr<XGC>&,S32,S32,U32,U32);
};
using XDrawablePtr=std::shared_ptr<XDrawable>;
struct XServer {
 std::unordered_map<U32,XDrawablePtr> images;
 static XServer* getServer(){static XServer s;return &s;}
 XDrawablePtr getDrawable(U32 id){return images[id];}
};
'''
code += method('class XPixelClip') + ';'
for signature in ['U32 x11ApplyRasterOperation(', 'int XDrawable::copyHostImageData(',
                  'int XDrawable::copy(', 'int XDrawable::drawLine(', 'int XDrawable::fillRectangle(']:
    code += method(signature)
code += r'''
int main(){
 auto gc=std::make_shared<XGC>(); auto d=std::make_shared<XDrawable>(6,5);
 U32 src[30];for(int i=0;i<30;++i)src[i]=i+1;
 auto put=[&]{return d->copyHostImageData(gc,(U8*)src,sizeof(src),24,32,0,0,0,0,6,5);};
 gc->clipRectsSet=true;assert(put()==Success);assert(d->pixel(0,0)==0); // empty means draw nowhere
 gc->values.clip_x_origin=1;gc->values.clip_y_origin=1;gc->clip_rects={{0,0,2,1},{3,2,1,1}};
 assert(put()==Success);assert(d->pixel(1,1)==8 && d->pixel(2,1)==9 && d->pixel(4,3)==23);
 assert(d->pixel(0,1)==0 && d->pixel(3,1)==0);
 gc->clipRectsSet=false;gc->clip_rects.clear();gc->values.clip_x_origin=gc->values.clip_y_origin=0;
 assert(put()==Success); // None removes the clip
 assert(d->copy(nullptr,gc,d,0,0,5,1,1,0)==Success);
 assert(d->pixel(0,0)==1 && d->pixel(1,0)==1 && d->pixel(5,0)==5); // snapshot, not cascading scroll
 assert(d->copyHostImageData(gc,(U8*)src,sizeof(src),24,32,0,0,-2,-1,6,5)==Success);
 assert(d->pixel(0,0)==9 && d->pixel(3,0)==12);
 assert(d->copyHostImageData(gc,(U8*)src,2,24,32,0,0,0,0,1,1)==BadValue);
 auto mask=std::make_shared<XDrawable>(6,5);mask->pixel(2,2)=1;XServer::getServer()->images[9]=mask;
 gc->values.clip_mask=9;gc->values.function=GXxor;gc->values.plane_mask=0xff;
 U32 old=d->pixel(2,2), neighbor=d->pixel(1,2);assert(put()==Success);
 assert(d->pixel(2,2)==(old^(src[14]&0xff)) && d->pixel(1,2)==neighbor);
 gc->values.clip_mask=0;gc->values.function=GXcopy;gc->values.plane_mask=~0u;
 std::fill(d->storage.begin(),d->storage.end(),0);
 d->drawLine(nullptr,gc,2,1,4,1);assert(d->pixel(0,1)==0 && d->pixel(2,1)==0x123456 && d->pixel(4,1)==0x123456);
 d->drawLine(nullptr,gc,0,0,4,4);assert(d->pixel(3,3)==0x123456);
 gc->values.cap_style=CapNotLast;d->drawLine(nullptr,gc,5,0,5,4);
 assert(d->pixel(5,3)==0x123456 && d->pixel(5,4)==0);
 gc->values.foreground=0xabcdef;d->fillRectangle(nullptr,gc,-2,-1,3,2);
 assert(d->pixel(0,0)==0xabcdef && d->pixel(1,0)==0);
 gc->clipRectsSet=true;gc->clip_rects={{2,2,1,1}};
 d->fillRectangle(nullptr,gc,0,0,6,5);assert(d->pixel(2,2)==0xabcdef && d->pixel(1,0)==0);
 assert(d->dirty>0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp=Path(tmp)/'pixels.cpp'; binary=Path(tmp)/'pixels';cpp.write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-g',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
print('X11 production pixel regression tests passed')
