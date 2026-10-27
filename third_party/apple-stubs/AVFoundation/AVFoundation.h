#pragma once
#import <Foundation/Foundation.h>
extern NSString* const AVAudioSessionCategoryAmbient;
@interface AVAudioSession : NSObject
+ (AVAudioSession*)sharedInstance;
- (BOOL)setCategory:(NSString*)c error:(NSError**)e;
- (BOOL)setPreferredSampleRate:(double)r error:(NSError**)e;
- (BOOL)setPreferredIOBufferDuration:(double)d error:(NSError**)e;   // iOS 6.0
- (BOOL)setActive:(BOOL)a error:(NSError**)e;
@end
