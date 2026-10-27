#pragma once
#import <Foundation/Foundation.h>
enum { noErr = 0 };
typedef struct OpaqueAudioComponent* AudioComponent;
typedef struct ComponentInstanceRecord* AudioComponentInstance;
typedef AudioComponentInstance AudioUnit;
typedef struct { UInt32 componentType, componentSubType, componentManufacturer, componentFlags, componentFlagsMask; } AudioComponentDescription;
enum { kAudioUnitType_Output = 'auou', kAudioUnitSubType_RemoteIO = 'rioc', kAudioUnitManufacturer_Apple = 'appl' };
typedef struct { double mSampleRate; UInt32 mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved; } AudioStreamBasicDescription;
enum { kAudioFormatLinearPCM = 'lpcm', kLinearPCMFormatFlagIsFloat = 1, kLinearPCMFormatFlagIsSignedInteger = 4, kLinearPCMFormatFlagIsPacked = 8 };
enum { kAudioUnitProperty_StreamFormat = 8, kAudioUnitProperty_SetRenderCallback = 23, kAudioUnitScope_Input = 1 };
typedef UInt32 AudioUnitRenderActionFlags;
typedef struct { double mSampleTime; } AudioTimeStamp;
typedef struct { UInt32 mNumberChannels, mDataByteSize; void* mData; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef OSStatus (*AURenderCallback)(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList*);
typedef struct { AURenderCallback inputProc; void* inputProcRefCon; } AURenderCallbackStruct;
#ifdef __cplusplus
extern "C" {
#endif
AudioComponent AudioComponentFindNext(AudioComponent c, const AudioComponentDescription* d);
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance* out);
OSStatus AudioUnitSetProperty(AudioUnit u, UInt32 id, UInt32 scope, UInt32 element, const void* data, UInt32 size);
OSStatus AudioUnitInitialize(AudioUnit u);
OSStatus AudioOutputUnitStart(AudioUnit u);
#ifdef __cplusplus
}
#endif
