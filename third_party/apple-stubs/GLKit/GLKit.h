#pragma once
#import <UIKit/UIKit.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
typedef enum { GLKViewDrawableColorFormatRGBA8888 = 0, GLKViewDrawableColorFormatRGB565 } GLKViewDrawableColorFormat;
typedef enum { GLKViewDrawableDepthFormatNone = 0, GLKViewDrawableDepthFormat16, GLKViewDrawableDepthFormat24 } GLKViewDrawableDepthFormat;
@class GLKView;
@protocol GLKViewDelegate <NSObject>
- (void)glkView:(GLKView*)view drawInRect:(CGRect)rect;
@end
@interface GLKView : UIView
@property (nonatomic, strong) EAGLContext* context;
@property (nonatomic, assign) id<GLKViewDelegate> delegate;
@property (nonatomic) GLKViewDrawableColorFormat drawableColorFormat;
@property (nonatomic) GLKViewDrawableDepthFormat drawableDepthFormat;
@property (nonatomic, readonly) NSInteger drawableWidth;
@property (nonatomic, readonly) NSInteger drawableHeight;
@end
@interface GLKViewController : UIViewController <GLKViewDelegate>
@property (nonatomic) NSInteger preferredFramesPerSecond;
@property (nonatomic, readonly) double timeSinceLastDraw;
@property (nonatomic, getter=isPaused) BOOL paused;
@end
