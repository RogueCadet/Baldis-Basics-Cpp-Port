#pragma once
#import <Foundation/Foundation.h>
@interface UIResponder : NSObject
@end
@class UIApplication, UIWindow, UIEvent;
@protocol UIApplicationDelegate <NSObject>
@end
@interface UIScreen : NSObject
+ (UIScreen*)mainScreen;
@property (nonatomic, readonly) CGRect bounds;
@property (nonatomic, readonly) CGFloat scale;
@end
@interface UIView : UIResponder
- (instancetype)initWithFrame:(CGRect)f;
@property (nonatomic) CGFloat contentScaleFactor;
@property (nonatomic, getter=isMultipleTouchEnabled) BOOL multipleTouchEnabled;
@end
@interface UIViewController : UIResponder
@property (nonatomic, strong) UIView* view;
- (void)loadView;
- (void)viewDidLoad;
@end
@interface UIWindow : UIView
@property (nonatomic, strong) UIViewController* rootViewController;
- (void)makeKeyAndVisible;
@end
@interface UIApplication : UIResponder
- (void)setStatusBarHidden:(BOOL)h;
@end
@interface UITouch : NSObject
- (CGPoint)locationInView:(UIView*)v;
@end
@interface UIEvent : NSObject
@end
typedef enum { UIUserInterfaceIdiomPhone, UIUserInterfaceIdiomPad } UIUserInterfaceIdiom;
@interface UIDevice : NSObject
+ (UIDevice*)currentDevice;
@property (nonatomic, readonly) UIUserInterfaceIdiom userInterfaceIdiom;
@property (nonatomic, readonly) NSString* model;          // iOS 2.0
@property (nonatomic, readonly) NSString* systemVersion;  // iOS 2.0
@end
typedef enum { UIInterfaceOrientationPortrait = 1, UIInterfaceOrientationLandscapeLeft = 4, UIInterfaceOrientationLandscapeRight = 3 } UIInterfaceOrientation;
enum { UIInterfaceOrientationMaskLandscape = (1 << 3) | (1 << 4) };
#define UIInterfaceOrientationIsLandscape(o) ((o) == UIInterfaceOrientationLandscapeLeft || (o) == UIInterfaceOrientationLandscapeRight)
int UIApplicationMain(int argc, char* argv[], NSString* principal, NSString* delegate);
