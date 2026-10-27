#pragma once
#include <string.h>
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef double CGFloat;
typedef struct { CGFloat x, y; } CGPoint;
typedef struct { CGFloat width, height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
typedef unsigned int UInt32;
typedef int OSStatus;
typedef unsigned long long UInt64;
#define nil __null   /* the SDK: a null pointer constant in C++ */
@class NSString, NSArray, NSDictionary, NSError, NSSet;
@protocol NSObject
@end
typedef struct { unsigned long state; id __unsafe_unretained* itemsPtr; unsigned long* mutationsPtr; unsigned long extra[5]; } NSFastEnumerationState;
@protocol NSFastEnumeration
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState*)s objects:(id __unsafe_unretained[])b count:(NSUInteger)n;
@end
__attribute__((objc_root_class)) @interface NSObject <NSObject>
+ (instancetype)alloc;
- (instancetype)init;
+ (Class)class;
@end
@interface NSString : NSObject
- (NSString*)stringByAppendingPathComponent:(NSString*)c;
- (const char*)fileSystemRepresentation;
- (const char*)UTF8String;
@end
@interface NSArray : NSObject <NSFastEnumeration>
- (id)objectAtIndex:(NSUInteger)i;
@end
@interface NSSet : NSObject <NSFastEnumeration>
@end
@interface NSDictionary : NSObject
@end
@interface NSError : NSObject
@end
@interface NSBundle : NSObject
+ (NSBundle*)mainBundle;
- (NSString*)resourcePath;
@end
typedef enum { NSDocumentDirectory = 9 } NSSearchPathDirectory;
typedef enum { NSUserDomainMask = 1 } NSSearchPathDomainMask;
NSArray* NSSearchPathForDirectoriesInDomains(NSSearchPathDirectory d, NSSearchPathDomainMask m, BOOL expand);
void NSLog(NSString* fmt, ...);
NSString* NSStringFromClass(Class c);
