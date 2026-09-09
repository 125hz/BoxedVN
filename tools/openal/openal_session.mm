// Activate playback before RemoteIO opens, including programs which never
// open Wine's OSS/SDL device. No recording category or microphone permission.
#import <AVFoundation/AVFoundation.h>
#include <cstdio>
extern "C" bool bvnOpenALPrepareSession() {
    @autoreleasepool {
        AVAudioSession* session=[AVAudioSession sharedInstance];
        NSError* error=nil;
        if (![session setCategory:AVAudioSessionCategoryPlayback error:&error] ||
            ![session setActive:YES error:&error]) {
            std::fprintf(stderr,"BOXEDWINE_OPENAL_SESSION error=%s\n",error.localizedDescription.UTF8String);
            return false;
        }
        return true;
    }
}
