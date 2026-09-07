// Project missing Wine timezone keys before the guest starts. GPLv2.
#pragma once
#include "guest_wine_audio_registration.h"
#include "guest_wine_timezone_data.h"

namespace boxedvn {
inline bool registerWineTimezones(std::string& contents) {
    // Preserve existing keys (including user-customized rules). The projected
    // builtin DLLs never run setupapi's WINE_REGISTRY installation pass, so a
    // prefix can lack the entire database even though kernelbase.dll exists.
    const std::string existing = registryAsciiLower(contents);
    bool changed = false;
    for (const char* entry : wineTimezoneRegistrySeed) {
        const std::string seed = entry;
        const size_t close = seed.find(']');
        const std::string header = registryAsciiLower(seed.substr(0, close + 1));
        size_t found = existing.find(header);
        while (found != std::string::npos && found && existing[found - 1] != '\n')
            found = existing.find(header, found + 1);
        if (found == std::string::npos) {
            if (!contents.empty() && contents.back() != '\n') contents += '\n';
            contents += '\n';
            contents += seed;
            changed = true;
        }
    }
    return changed;
}
}
