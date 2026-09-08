// Wine 9 COM registration repairs for projected builtin DLLs.
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

// Wine 11 dlls/rsaenh/rsaenh.rgs. Projected DLLs can be available before
// wineboot has run their registration resources. CryptoAPI needs these keys
// even for RNG creation; a missing provider can surface as a managed null RNG.
inline bool registerWineCryptoProviders(std::string& contents, bool pe64, bool pe32) {
    bool changed = false;
    struct Provider { const char* name; const char* type; };
    const Provider providers[] = {
        {"Microsoft Base Cryptographic Provider v1.0", "dword:00000001"},
        {"Microsoft Enhanced Cryptographic Provider v1.0", "dword:00000001"},
        {"Microsoft Strong Cryptographic Provider", "dword:00000001"},
        {"Microsoft Enhanced RSA and AES Cryptographic Provider", "dword:00000018"},
        {"Microsoft Enhanced RSA and AES Cryptographic Provider (Prototype)", "dword:00000018"},
        {"Microsoft RSA SChannel Cryptographic Provider", "dword:0000000c"}
    };
    for (int bits : {64, 32}) {
        if (!(bits == 64 ? pe64 : pe32)) continue;
        const std::string root = std::string("Software\\\\") +
            (bits == 32 ? "Wow6432Node\\\\" : "") + "Microsoft\\\\Cryptography\\\\Defaults\\\\";
        for (const auto& provider : providers) {
            const auto key = root + "Provider\\\\" + provider.name;
            const bool added = insertMissingWineRegistryValue(contents, key, "\"Image Path\"",
                bits == 64 ? "\"C:\\\\windows\\\\system32\\\\rsaenh.dll\"" :
                             "\"C:\\\\windows\\\\syswow64\\\\rsaenh.dll\"");
            if (added) {
                insertMissingWineRegistryValue(contents, key, "\"Type\"", provider.type);
                insertMissingWineRegistryValue(contents, key, "\"Signature\"", "hex:de,ad,be,ef");
            }
            changed |= added;
        }
        changed |= insertMissingWineRegistryValue(contents, root + "Provider Types\\\\Type 001", "\"Name\"",
            "\"Microsoft Enhanced Cryptographic Provider v1.0\"");
        changed |= insertMissingWineRegistryValue(contents, root + "Provider Types\\\\Type 012", "\"Name\"",
            "\"Microsoft RSA SChannel Cryptographic Provider\"");
        changed |= insertMissingWineRegistryValue(contents, root + "Provider Types\\\\Type 024", "\"Name\"",
            "\"Microsoft Enhanced RSA and AES Cryptographic Provider\"");
    }
    return changed;
}

// Wine 11 actxprxy_servprov.idl registers this proxy factory for
// IServiceProvider. Shell services marshal it between processes.
inline bool registerWineServiceProviderProxy(std::string& contents, bool pe64, bool pe32) {
    bool changed = false;
    for (int bits : {64, 32}) {
        if (!(bits == 64 ? pe64 : pe32)) continue;
        const std::string root = std::string("Software\\\\Classes\\\\") +
            (bits == 32 ? "Wow6432Node\\\\" : "");
        const std::string factory = root + "CLSID\\\\{b8da6310-e19b-11d0-933c-00a0c90dcaa9}\\\\InprocServer32";
        const bool added = insertMissingWineRegistryValue(contents, factory, "@", bits == 64 ?
            "\"C:\\\\windows\\\\system32\\\\actxprxy.dll\"" : "\"C:\\\\windows\\\\syswow64\\\\actxprxy.dll\"");
        if (added) insertMissingWineRegistryValue(contents, factory, "\"ThreadingModel\"", "\"Both\"");
        changed |= added;
        const std::string iface = root + "Interface\\\\{6d5140c1-7436-11ce-8034-00aa006009fa}";
        changed |= insertMissingWineRegistryValue(contents, iface, "@", "\"IServiceProvider\"");
        changed |= insertMissingWineRegistryValue(contents, iface + "\\\\ProxyStubClsid32", "@",
            "\"{b8da6310-e19b-11d0-933c-00a0c90dcaa9}\"");
        changed |= insertMissingWineRegistryValue(contents, iface + "\\\\NumMethods", "@", "\"4\"");
    }
    return changed;
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

// Wine 9 dlls/dxdiagn/dxdiagn.idl declares an apartment-threaded provider.
// Projecting a builtin PE does not run its registration resource on an
// already-created prefix. Supply missing registrations, never replace one.
inline bool registerWineDxDiagProvider(std::string& contents, bool pe64, bool pe32) {
    bool changed = false;
    for (int bitness : {64, 32}) {
        if (!(bitness == 64 ? pe64 : pe32)) continue;
        const std::string section = std::string("Software\\\\Classes\\\\") +
            (bitness == 32 ? "Wow6432Node\\\\" : "") +
            "CLSID\\\\{a65b8071-3bfe-4213-9a5b-491da4461ca7}\\\\InprocServer32";
        const bool added = insertMissingWineRegistryValue(contents, section, "@",
            bitness == 64 ? "\"C:\\\\windows\\\\system32\\\\dxdiagn.dll\"" :
                            "\"C:\\\\windows\\\\syswow64\\\\dxdiagn.dll\"");
        if (added) insertMissingWineRegistryValue(contents, section,
            "\"ThreadingModel\"", "\"Apartment\"");
        changed |= added;
    }
    return changed;
}
// Wine 9's desktop folder returns ::{CLSID} for parsing unless the filesystem
// backed namespace has WantsForParsing. A projected shell32 alone does not
// register that metadata in an existing WoW64 prefix. Keep native overrides.
inline bool registerWineDocumentsFolder(std::string& contents, bool pe64, bool pe32) {
    bool changed = false;
    for (int bits : {64, 32}) {
        if (!(bits == 64 ? pe64 : pe32)) continue;
        const std::string key = std::string("Software\\\\Classes\\\\") +
            (bits == 32 ? "Wow6432Node\\\\" : "") +
            "CLSID\\\\{450d8fba-ad25-11d0-98a8-0800361b1103}";
        const bool added = insertMissingWineRegistryValue(contents,
            key + "\\\\InprocServer32", "@", bits == 64 ?
            "\"C:\\\\windows\\\\system32\\\\shell32.dll\"" :
            "\"C:\\\\windows\\\\syswow64\\\\shell32.dll\"");
        if (added) insertMissingWineRegistryValue(contents,
            key + "\\\\InprocServer32", "\"ThreadingModel\"", "\"Apartment\"");
        changed |= added;
        changed |= insertMissingWineRegistryValue(contents,
            key + "\\\\ShellFolder", "\"WantsForParsing\"", "\"\"");
    }
    return changed;
}

// WMI's locator is used by system-information queries, including DXDiag.
// These class IDs and threading models come from Wine 9 wbemprox.idl.
inline bool registerWineWbemLocator(std::string& contents, bool pe64, bool pe32) {
    bool changed = false;
    for (int bits : {64, 32}) {
        if (!(bits == 64 ? pe64 : pe32)) continue;
        for (const char* clsid : {"4590f811-1d3a-11d0-891f-00aa004b2e24",
                                  "674b6698-ee92-11d0-ad71-00c04fd8fdff",
                                  "cb8555cc-9128-11d1-ad9b-00c04fd8fdff"}) {
            const std::string key = std::string("Software\\\\Classes\\\\") +
                (bits == 32 ? "Wow6432Node\\\\" : "") + "CLSID\\\\{" + clsid +
                "}\\\\InprocServer32";
            const bool added = insertMissingWineRegistryValue(contents, key, "@",
                bits == 64 ? "\"C:\\\\windows\\\\system32\\\\wbemprox.dll\"" :
                             "\"C:\\\\windows\\\\syswow64\\\\wbemprox.dll\"");
            if (added) insertMissingWineRegistryValue(contents, key,
                "\"ThreadingModel\"", "\"Both\"");
            changed |= added;
        }
    }
    return changed;
}

// Wine 9 devenum_classes.idl: device enumeration and device monikers.
inline bool registerWineMediaDeviceEnumerator(std::string& contents, bool pe64, bool pe32) {
    bool changed = false;
    for (int bits : {64, 32}) {
        if (!(bits == 64 ? pe64 : pe32)) continue;
        for (const char* clsid : {"62be5d10-60eb-11d0-bd3b-00a0c911ce86",
                                  "4315d437-5b8c-11d0-bd3b-00a0c911ce86"}) {
            const std::string key = std::string("Software\\\\Classes\\\\") +
                (bits == 32 ? "Wow6432Node\\\\" : "") + "CLSID\\\\{" + clsid +
                "}\\\\InprocServer32";
            const bool added = insertMissingWineRegistryValue(contents, key, "@",
                bits == 64 ? "\"C:\\\\windows\\\\system32\\\\devenum.dll\"" :
                             "\"C:\\\\windows\\\\syswow64\\\\devenum.dll\"");
            if (added) insertMissingWineRegistryValue(contents, key,
                "\"ThreadingModel\"", "\"Both\"");
            changed |= added;
        }
    }
    return changed;
}

}
