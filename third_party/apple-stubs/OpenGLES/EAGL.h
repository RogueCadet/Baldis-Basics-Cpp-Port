#pragma once
#import <Foundation/Foundation.h>
typedef enum { kEAGLRenderingAPIOpenGLES1 = 1, kEAGLRenderingAPIOpenGLES2 = 2 } EAGLRenderingAPI;
@interface EAGLContext : NSObject
- (instancetype)initWithAPI:(EAGLRenderingAPI)api;
+ (BOOL)setCurrentContext:(EAGLContext*)c;
@end
