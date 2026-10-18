#import <AppKit/AppKit.h>
#include <atomic>
#include <chrono>
#include <thread>

int beauty_test_main(int argc, char **argv);

int main(int argc, char **argv)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        [NSApp finishLaunching];
        id activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiated
                                                                 reason:@"OBS-Beauty render validation"];
        std::atomic<bool> done{false};
        int result = 1;
        std::thread worker([&] {
            @autoreleasepool {
                result = beauty_test_main(argc, argv);
            }
            done.store(true);
        });
        // Service AppKit while libobs renders.
        while (!done.load()) {
            @autoreleasepool {
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                        beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        worker.join();
        if (activity)
            [NSProcessInfo.processInfo endActivity:activity];
        return result;
    }
}
