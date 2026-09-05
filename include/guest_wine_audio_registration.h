// Wine 9 MMDeviceEnumerator registration for projected builtin DLLs.
// GPLv2; see license.txt.
#pragma once
#include <string>
#include <cctype>

namespace boxedvn {
inline std::string registryAsciiLower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// Wine's on-disk escaping is supplied by the caller. Preserve unknown values,
// timestamps, other sections, and any existing registration (including native
// overrides). Registry key/value names are case insensitive.
inline bool insertMissingWineRegistryValue(std::string& contents,
        const std::string& section, const std::string& valueName,
        const std::string& quotedValue) {
    const std::string lower = registryAsciiLower(contents);
    const std::string header = "[" + registryAsciiLower(section) + "]";
    const std::string prefix = registryAsciiLower(valueName) + "=";
    const std::string assignment = valueName + "=" + quotedValue + "\n";
    size_t start = lower.find(header);
    while (start != std::string::npos && start && lower[start - 1] != '\n')
        start = lower.find(header, start + 1);
    if (start == std::string::npos) {
        if (!contents.empty() && contents.back() != '\n') contents += '\n';
        contents += "\n[" + section + "] 0\n" + assignment;
        return true;
    }
    size_t end = lower.find("\n[", start);
    if (end == std::string::npos) end = lower.size();
    size_t line = lower.find('\n', start);
    while (line != std::string::npos && ++line < end) {
        if (lower.compare(line, prefix.size(), prefix) == 0) return false;
        line = lower.find('\n', line);
    }
    // Append before the next section; leave #time directly below its header.
    if (end && contents[end - 1] != '\n') contents.insert(end++, "\n");
    contents.insert(end, assignment);
    return true;
}

inline bool registerWineAudioEnumerator(std::string& contents, bool pe64, bool pe32) {
    bool changed = false;
    for (int bitness : {64, 32}) {
        if (!(bitness == 64 ? pe64 : pe32)) continue;
        const std::string section = std::string("Software\\\\Classes\\\\") +
            (bitness == 32 ? "Wow6432Node\\\\" : "") +
            "CLSID\\\\{bcde0395-e52f-467c-8e3d-c4579291692e}\\\\InprocServer32";
        const bool added = insertMissingWineRegistryValue(contents, section, "@",
            bitness == 64 ? "\"C:\\\\windows\\\\system32\\\\mmdevapi.dll\"" :
                            "\"C:\\\\windows\\\\syswow64\\\\mmdevapi.dll\"");
        // A pre-existing class belongs to Wine/the user. Only complete the
        // threading model when this repair supplies the missing server path.
        if (added) insertMissingWineRegistryValue(contents, section,
            "\"ThreadingModel\"", "\"Both\"");
        changed |= added;
    }
    return changed;
}
}
