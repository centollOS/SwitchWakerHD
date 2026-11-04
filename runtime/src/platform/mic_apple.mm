// SPDX-License-Identifier: MPL-2.0
// The microphone service's backend for the AppKit host (mic.h): macOS asks for permission the first time (the app's
// NSMicrophoneUsageDescription; a plain binary started from a terminal gets the terminal's permission), then an
// AudioQueue records 48 kHz mono floats, which go straight to the pitch analysis (mic::feed) and nowhere else.
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <mutex>

#include "../mic.h"
#include "../runtime.h"

namespace mic {
namespace {
std::mutex g_queue_mu;
AudioQueueRef g_queue = nullptr;
constexpr int kBuffers = 3, kFrames = 1024, kRate = 48000;

void input(void*, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp*, UInt32, const AudioStreamPacketDescription*) {
    feed(static_cast<const float*>(b->mAudioData), b->mAudioDataByteSize / sizeof(float), kRate);
    AudioQueueEnqueueBuffer(q, b, 0, nullptr);
}

void open_queue() {
    AudioStreamBasicDescription f{};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBitsPerChannel = 32;
    f.mChannelsPerFrame = 1;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = f.mBytesPerPacket = 4;
    std::lock_guard lock(g_queue_mu);
    AudioQueueRef q = nullptr;
    if (AudioQueueNewInput(&f, input, nullptr, nullptr, nullptr, 0, &q) != noErr || !q) {
        started(kUnavailable);
        return;
    }
    for (int i = 0; i < kBuffers; i++) {
        AudioQueueBufferRef b = nullptr;
        if (AudioQueueAllocateBuffer(q, kFrames * 4, &b) == noErr) AudioQueueEnqueueBuffer(q, b, 0, nullptr);
    }
    if (AudioQueueStart(q, nullptr) != noErr) {
        AudioQueueDispose(q, true);
        started(kUnavailable);
        return;
    }
    g_queue = q;
    started(kOpen);
}

// an app bundle without a microphone description would be stopped by the system at the ask: no microphone then
bool may_ask() {
    NSBundle* b = [NSBundle mainBundle];
    if (![[b bundlePath] hasSuffix:@".app"]) return true;
    return [b objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"] != nil;
}
}  // namespace

void backend_start() {
    if (!may_ask()) {
        LOG("[mic] this app has no microphone description (Info.plist): no microphone");
        started(kUnavailable);
        return;
    }
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusAuthorized:
        open_queue();
        break;
    case AVAuthorizationStatusNotDetermined:
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                 completionHandler:^(BOOL granted) {
                                   if (granted) open_queue();
                                   else started(kDenied);
                                 }];
        break;
    default:  // denied or restricted: the system settings decide
        started(kDenied);
        break;
    }
}

void backend_stop() {
    std::lock_guard lock(g_queue_mu);
    if (!g_queue) return;
    AudioQueueStop(g_queue, true);
    AudioQueueDispose(g_queue, true);
    g_queue = nullptr;
}

}  // namespace mic
