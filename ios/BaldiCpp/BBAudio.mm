// BBAudio.mm -- sound output: a RemoteIO audio unit pulling 44100 Hz interleaved stereo from the
// engine's mixer through app::audioPullFloat / app::audioPull (the same pull the Windows testbed's
// waveOut thread makes). Ambient session: mixes with other audio and honours the silent switch.
//
// Float32 first, like Unity's FMOD: the engine's mix is handed over UNCLAMPED, so a loud sum (the
// game-over's stack of per-frame BAL_Screech copies) is not hard-clipped by us before the system
// output stage -- clamping to int16 first turned 72% of the jumpscare into a harsh square wave on
// Windows. RemoteIO's input scope converts linear PCM formats, Float32 included (iOS 5+); if this
// device still refuses it, SInt16 (the format every iOS version takes) is used instead.
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include "BBAudio.h"
#include "app-core.h"

static AudioUnit s_unit = 0;
static bool s_float = false;

static OSStatus BBRender(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* stamp,
                         UInt32 bus, UInt32 frames, AudioBufferList* io) {
  // Interleaved format: one buffer holding frames * 2 samples.
  for (UInt32 b = 0; b < io->mNumberBuffers; b++) {
    if (s_float) app::audioPullFloat((float*)io->mBuffers[b].mData, (int)frames);
    else app::audioPull((short*)io->mBuffers[b].mData, (int)frames);
  }
  return noErr;
}

static AudioStreamBasicDescription BBFormat(bool flt) {
  AudioStreamBasicDescription fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.mSampleRate = 44100.0;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags = (flt ? kLinearPCMFormatFlagIsFloat : kLinearPCMFormatFlagIsSignedInteger) |
                     kLinearPCMFormatFlagIsPacked;
  fmt.mBitsPerChannel = flt ? 32 : 16;
  fmt.mChannelsPerFrame = 2;
  fmt.mFramesPerPacket = 1;
  fmt.mBytesPerFrame = fmt.mChannelsPerFrame * fmt.mBitsPerChannel / 8;
  fmt.mBytesPerPacket = fmt.mBytesPerFrame;
  return fmt;
}

void BBAudioStart(void) {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  [session setCategory:AVAudioSessionCategoryAmbient error:NULL];
  [session setPreferredSampleRate:44100.0 error:NULL];   // iOS 6+
  // Unity's DSP buffer (AudioManager.asset m_DSPBufferSize 1024): voices start on block boundaries,
  // so this spaces the game-over's per-frame screech copies the way the genuine game does.
  [session setPreferredIOBufferDuration:1024.0 / 44100.0 error:NULL];
  [session setActive:YES error:NULL];                    // iOS 6+

  AudioComponentDescription desc;
  memset(&desc, 0, sizeof(desc));
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_RemoteIO;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent comp = AudioComponentFindNext(NULL, &desc);
  if (!comp || AudioComponentInstanceNew(comp, &s_unit) != noErr) {
    NSLog(@"BaldiCpp: no RemoteIO unit -- running silent");
    return;
  }
  // The input scope of the output element is what we feed; RemoteIO converts to the hardware rate.
  AudioStreamBasicDescription fmt = BBFormat(true);
  s_float = AudioUnitSetProperty(s_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                 &fmt, sizeof(fmt)) == noErr;
  if (!s_float) {
    fmt = BBFormat(false);
    AudioUnitSetProperty(s_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
  }
  NSLog(@"BaldiCpp: RemoteIO output %s", s_float ? "Float32" : "SInt16 (Float32 refused)");
  AURenderCallbackStruct cb;
  cb.inputProc = BBRender;
  cb.inputProcRefCon = NULL;
  AudioUnitSetProperty(s_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
  if (AudioUnitInitialize(s_unit) != noErr || AudioOutputUnitStart(s_unit) != noErr)
    NSLog(@"BaldiCpp: RemoteIO failed to start -- running silent");
}
