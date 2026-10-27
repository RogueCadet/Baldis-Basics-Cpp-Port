// BBAppDelegate.mm -- window + root view controller (iOS 6 rotation is driven by the root view
// controller) and the app lifecycle, forwarded to the game as Unity's OnApplicationPause.
#import "BBAppDelegate.h"
#import "BBViewController.h"
#import "BBAudio.h"
#include "app-core.h"

@implementation BBAppDelegate {
  BBViewController* _game;
}

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  [application setStatusBarHidden:YES];
  self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
  _game = [[BBViewController alloc] init];
  self.window.rootViewController = _game;
  [self.window makeKeyAndVisible];
  BBAudioStart();
  return YES;
}

// The genuine iOS GameControllerScript.OnApplicationPause pauses the game on suspend AND resume
// (it ignores pauseStatus); app-core forwards both as Unity does. Before the first frame there is
// no scene yet, so nothing is sent.
- (void)applicationWillResignActive:(UIApplication*)application {
  if ([_game started]) app::applicationPause(true);
  _game.paused = YES;
}

- (void)applicationDidBecomeActive:(UIApplication*)application {
  if ([_game started]) app::applicationPause(false);
  _game.paused = NO;
}

@end
