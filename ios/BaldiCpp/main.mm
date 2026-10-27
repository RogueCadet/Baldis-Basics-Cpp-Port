// main.mm -- iOS entry point. Everything game-side runs through src/app-core (see app-core.h);
// these ios/BaldiCpp/*.mm files are only the iOS glue: window, GL view + display link, touches,
// RemoteIO audio, app lifecycle. Target: iOS 6.0, armv7/armv7s, Xcode 4.5 - 5.1.1.
#import <UIKit/UIKit.h>
#import "BBAppDelegate.h"

int main(int argc, char* argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass([BBAppDelegate class]));
  }
}
