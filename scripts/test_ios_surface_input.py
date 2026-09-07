"""Compile production resize/warp methods with a small native-platform harness.

No GPU is emulated here: this verifies dispatch, lifetime checks, unchanged-size
fast paths and input ordering. Real Metal presentation remains a device test.
"""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent

def method(file, signature):
    source = (repo / file).read_text()
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

code = r"""
#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>
#include <cassert>
#include <atomic>
#include <thread>
using U32 = unsigned; using S32 = int;
#define BOXEDWINE_IOS
int dispatches=0, fits=0, warps=0, callbacks=0;
#define DISPATCH_MAIN_THREAD_BLOCK_THIS_BEGIN ++dispatches; [=, this]() -> U32 {
#define DISPATCH_MAIN_THREAD_BLOCK_END return 0; }();
void klog_fmt(const char*, ...) {}
struct XWindowChanges { int x=0,y=0; unsigned width=0,height=0; };
constexpr unsigned CWX=1,CWY=2,CWWidth=4,CWHeight=8;
constexpr int Success=0;
struct XWindow {
 int left=0,top=0; unsigned w=394,h=275, id=12, configured=0;
 unsigned width() const { return w; } unsigned height() const { return h; }
 int configure(unsigned mask, XWindowChanges* c) {
   assert(mask==15); left=c->x; top=c->y; w=c->width; h=c->height;
   ++configured; return Success;
 }
 int moveResize(S32,S32,U32,U32);
};
using XWindowPtr=std::shared_ptr<XWindow>;
struct Screen { unsigned w=394,h=275,refreshes=0;
 void setScreenSize(unsigned x,unsigned y) {w=x;h=y;}
 void refreshIOSGuestPointerTransform(){++refreshes;}
};
struct Record { void* surface; XWindowPtr window; bool presentationVisible=true,presentation=true;
 unsigned drawableWidth=394,drawableHeight=275; };
struct KVulkdanSDLImpl { std::mutex surfacesMutex; std::vector<Record> surfaces;
 std::shared_ptr<Screen> screen=std::make_shared<Screen>();
 void syncVulkanSurface(void*);
};
unsigned fittedW=0,fittedH=0;
void BVNApplyGuestPresentationAspect(void*,unsigned w,unsigned h,int){++fits;fittedW=w;fittedH=h;}
int BVNGuestPreferredPresentationMode(){return 0;}
void BVNGuestPointerPositionChanged(int,int){++callbacks;}
struct XServer { int moves=0,x=0,y=0; bool relative=true;
 static XServer* getServer(bool){static XServer s;return &s;}
 void mouseMove(int a,int b,bool r){++moves;x=a;y=b;relative=r;}
};
struct KNativeSystem { static void warpMouse(int,int){++warps;} };
struct KNativeInputSDL {int injectedX=0,injectedY=0;bool hasInjectedPointer=false;
 void setMousePos(int,int); int xToScreen(int x){return x;} int yToScreen(int y){return y;}
};
using SDL_AudioDeviceID=unsigned;
struct SDL_AudioSpec {};
const int SDL_AUDIO_ALLOW_ANY_CHANGE=15;
std::mutex dspDeviceLifecycleMutex;
std::atomic<int> audioInFlight{0};
SDL_AudioDeviceID SDL_OpenAudioDevice(const char*,int,const SDL_AudioSpec*,SDL_AudioSpec*,int) {
 assert(audioInFlight.fetch_add(1)==0);std::this_thread::yield();--audioInFlight;return 2;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID) {
 assert(audioInFlight.fetch_add(1)==0);std::this_thread::yield();--audioInFlight;
}
"""
code += method("platform/sdl/kvulkanSDL.cpp", "void KVulkdanSDLImpl::syncVulkanSurface(")
code += method("platform/sdl/knativeinputSDL.cpp", "void KNativeInputSDL::setMousePos(")
code += method("source/x11/xwindow.cpp", "int XWindow::moveResize(")
code += method("platform/sdl/kdspaudio.cpp", "static SDL_AudioDeviceID openDspAudioDevice(")
code += method("platform/sdl/kdspaudio.cpp", "static void closeDspAudioDevice(")
code += r"""
int main(){
 auto w=std::make_shared<XWindow>(); KVulkdanSDLImpl b;
 void* s=(void*)1; b.surfaces.push_back({s,w});
 b.syncVulkanSurface((void*)2); b.syncVulkanSurface(s);
 assert(dispatches==0 && fits==0);
 w->w=640;w->h=480; b.syncVulkanSurface(s);
 assert(dispatches==1 && fits==1 && fittedW==640 && fittedH==480);
 assert(b.screen->w==640 && b.screen->h==480 && b.screen->refreshes==1);
 for(int i=0;i<1000;++i) b.syncVulkanSurface(s);
 assert(dispatches==1 && fits==1);
 w->w=1280;w->h=720;b.syncVulkanSurface(s);
 assert(fits==2 && fittedW==1280 && fittedH==720);
 b.surfaces[0].presentation=false;w->w=800;b.syncVulkanSurface(s);
 assert(fits==2); b.surfaces[0].presentation=true;b.surfaces[0].presentationVisible=false;
 b.syncVulkanSurface(s);assert(fits==2);
 b.surfaces.clear();b.syncVulkanSurface(s);assert(fits==2);
 KNativeInputSDL input;auto server=XServer::getServer(true);
 input.setMousePos(320,240);
 assert(input.hasInjectedPointer && input.injectedX==320 && input.injectedY==240);
 assert(server->moves==1 && server->x==320 && server->y==240 && !server->relative);
 assert(warps==0 && callbacks==1);
 input.setMousePos(330,242); assert(server->moves==2 && warps==0 && input.injectedX==330);
 XWindow child;child.moveResize(40,60,200,150);
 assert(child.left==40 && child.top==60 && child.w==200 && child.h==150 && child.configured==1);
 child.moveResize(40,60,200,150);assert(child.configured==1);
 child.moveResize(10,20,200,150);assert(child.left==10 && child.top==20 && child.configured==2);
 std::vector<std::thread> workers;
 for(int t=0;t<8;++t) workers.emplace_back([] {
   SDL_AudioSpec spec;
   for(int i=0;i<200;++i) closeDspAudioDevice(openDspAudioDevice(&spec,&spec));
 });
 for(auto& worker:workers) worker.join();
 assert(audioInFlight==0);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    source=Path(tmp)/"native.cpp";exe=Path(tmp)/"native"
    source.write_text(code)
    subprocess.run(["g++","-std=c++20","-pthread",str(source),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("Production surface resizing, virtual warp delivery and window movement passed")
