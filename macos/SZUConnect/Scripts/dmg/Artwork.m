// Draw installation guidance and create the Finder background alias using macOS frameworks.
#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>

static void Require(BOOL condition, NSString *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message.UTF8String);
        exit(1);
    }
}

static void DrawCentered(NSString *text, CGFloat y, CGFloat fontSize, NSFontWeight weight,
                         NSColor *color) {
    NSDictionary *attributes = @{NSFontAttributeName: [NSFont systemFontOfSize:fontSize weight:weight],
                                 NSForegroundColorAttributeName: color};
    NSSize size = [text sizeWithAttributes:attributes];
    [text drawAtPoint:NSMakePoint((680 - size.width) / 2, y) withAttributes:attributes];
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        Require(argc == 4, @"Usage: dmg-artwork draw|alias|check-alias input output");
        NSString *mode = @(argv[1]);
        NSString *input = @(argv[2]);
        NSString *output = @(argv[3]);
        if ([mode isEqualToString:@"draw"]) {
            NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc]
                initWithBitmapDataPlanes:NULL pixelsWide:1360 pixelsHigh:880 bitsPerSample:8
                samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                bytesPerRow:0 bitsPerPixel:0];
            Require(bitmap != nil, @"Unable to allocate DMG background");
            bitmap.size = NSMakeSize(680, 440);
            [NSGraphicsContext saveGraphicsState];
            [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap]];
            [[NSColor colorWithCalibratedRed:0.965 green:0.976 blue:0.992 alpha:1] setFill];
            NSRectFill(NSMakeRect(0, 0, 680, 440));
            // Finder icon coordinates use a top-left origin. This curve ends at the folder icon.
            [[NSColor colorWithCalibratedRed:0.17 green:0.35 blue:0.75 alpha:1] setStroke];
            NSBezierPath *arrow = [NSBezierPath bezierPath];
            arrow.lineWidth = 4;
            arrow.lineCapStyle = NSLineCapStyleRound;
            arrow.lineJoinStyle = NSLineJoinStyleRound;
            [arrow moveToPoint:NSMakePoint(282, 260)];
            [arrow curveToPoint:NSMakePoint(398, 260)
                 controlPoint1:NSMakePoint(320, 278) controlPoint2:NSMakePoint(360, 278)];
            [arrow moveToPoint:NSMakePoint(384, 280)];
            [arrow lineToPoint:NSMakePoint(398, 260)];
            [arrow lineToPoint:NSMakePoint(375, 258)];
            [arrow stroke];
            DrawCentered(@"安装深大随连", 359, 23, NSFontWeightSemibold,
                         [NSColor colorWithCalibratedRed:0.13 green:0.17 blue:0.24 alpha:1]);
            DrawCentered(@"将「深大随连」拖到「应用程序」", 81, 21, NSFontWeightMedium,
                         [NSColor colorWithCalibratedRed:0.13 green:0.17 blue:0.24 alpha:1]);
            [NSGraphicsContext restoreGraphicsState];
            NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            Require(png != nil && [png writeToFile:output atomically:YES], @"Unable to write DMG background");
        } else if ([mode isEqualToString:@"alias"]) {
            FSRef target;
            Require(FSPathMakeRef((const UInt8 *)input.fileSystemRepresentation, &target, NULL) == noErr,
                    @"Unable to locate DMG background");
            AliasHandle alias = NULL;
            // Finder's icon-view backgroundImageAlias uses an Alias Manager record, not a URL bookmark.
            Require(FSNewAliasMinimal(&target, &alias) == noErr && alias != NULL,
                    @"Unable to create Finder background alias");
            HLock((Handle)alias);
            NSData *data = [NSData dataWithBytes:*alias length:GetHandleSize((Handle)alias)];
            Require([data writeToFile:output atomically:YES], @"Unable to write Finder background alias");
            HUnlock((Handle)alias);
            DisposeHandle((Handle)alias);
        } else if ([mode isEqualToString:@"check-alias"]) {
            NSData *data = [NSData dataWithContentsOfFile:input];
            Require(data != nil && data.length >= sizeof(AliasRecord), @"Invalid Finder background alias");
            AliasHandle alias = (AliasHandle)NewHandle(data.length);
            Require(alias != NULL, @"Unable to allocate Finder alias");
            HLock((Handle)alias);
            memcpy(*alias, data.bytes, data.length);
            HUnlock((Handle)alias);
            FSRef resolved, expected;
            Boolean changed = false;
            OSErr status = FSResolveAliasWithMountFlags(NULL, alias, &resolved, &changed,
                                                       kResolveAliasFileNoUI);
            DisposeHandle((Handle)alias);
            Require(status == noErr, @"Unable to resolve Finder background alias");
            Require(FSPathMakeRef((const UInt8 *)output.fileSystemRepresentation, &expected, NULL) == noErr
                    && FSCompareFSRefs(&resolved, &expected) == noErr,
                    @"Finder background alias resolved outside this image");
        } else {
            Require(NO, @"Unknown DMG artwork operation");
        }
    }
    return 0;
}
