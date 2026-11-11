// Read-only system-state snapshot; not a temperature or frequency measurement.
#import <Foundation/Foundation.h>
#include <iostream>
int main() {
    @autoreleasepool {
        NSProcessInfo* info = [NSProcessInfo processInfo];
        std::cout << "{\"thermal_state\":" << static_cast<long>(info.thermalState)
                  << ",\"low_power_mode\":" << (info.lowPowerModeEnabled ? "true" : "false") << "}\n";
    }
}
