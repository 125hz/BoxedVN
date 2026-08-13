/*
 * BoxedVN - writable Wine-prefix preparation.
 * GPLv2; see license.txt.
 */

#include "boxedvn/wine_prefix.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
#include "unzip.h"
}

namespace fs = std::filesystem;

namespace boxedvn {
namespace {

constexpr const char* kUserRegistryEntry =
    "home/username/.wine/user.reg";
constexpr const char* kSystemRegistryEntry =
    "home/username/.wine/system.reg";

std::string escapedSection(const std::string& section) {
    std::string result;
    result.reserve(section.size() * 2 + 2);
    result.push_back('[');
    for (const char c : section) {
        if (c == '\\') {
            result.push_back('\\');
        }
        result.push_back(c);
    }
    result.push_back(']');
    return result;
}

bool readFile(const fs::path& path, std::string& contents, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "Could not read Wine registry overlay '" + path.string() +
                "'.";
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    contents = buffer.str();
    if (contents.rfind("WINE REGISTRY Version 2", 0) != 0) {
        error = "Wine registry overlay '" + path.string() +
                "' has an unrecognised format.";
        return false;
    }
    return true;
}

bool atomicWrite(const fs::path& path, const std::string& contents,
                 std::string& error) {
    const fs::path temporary = path.string() + ".boxedvn-tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Could not create temporary Wine registry '" +
                    temporary.string() + "'.";
            return false;
        }
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            error = "Could not write Wine registry '" + temporary.string() +
                    "'. The device may be full.";
            return false;
        }
    }

    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        error = "Could not install Wine registry overlay '" + path.string() +
                "': " + ec.message();
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

bool extractEntry(const std::string& archivePath, const char* entryName,
                  const fs::path& destination, std::string& error) {
    unzFile zip = unzOpen64(archivePath.c_str());
    if (zip == nullptr) {
        error = "Could not open root filesystem ZIP '" + archivePath + "'.";
        return false;
    }

    if (unzLocateFile(zip, entryName, 0) != UNZ_OK) {
        error = "Root filesystem ZIP does not contain '" +
                std::string(entryName) + "'.";
        unzClose(zip);
        return false;
    }
    int status = unzOpenCurrentFile(zip);
    if (status != UNZ_OK) {
        error = "Could not open '" + std::string(entryName) +
                "' inside the root filesystem ZIP.";
        unzClose(zip);
        return false;
    }

    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        error = "Could not create Wine prefix directory '" +
                destination.parent_path().string() + "': " + ec.message();
        unzCloseCurrentFile(zip);
        unzClose(zip);
        return false;
    }

    const fs::path temporary = destination.string() + ".boxedvn-extract";
    std::FILE* output = std::fopen(temporary.string().c_str(), "wb");
    if (output == nullptr) {
        error = "Could not create Wine registry overlay '" +
                temporary.string() + "': " + std::strerror(errno);
        unzCloseCurrentFile(zip);
        unzClose(zip);
        return false;
    }

    std::vector<unsigned char> buffer(64 * 1024);
    int read = 0;
    bool writeFailed = false;
    while ((read = unzReadCurrentFile(zip, buffer.data(),
                                      static_cast<unsigned>(buffer.size()))) > 0) {
        if (std::fwrite(buffer.data(), 1, static_cast<size_t>(read), output) !=
            static_cast<size_t>(read)) {
            writeFailed = true;
            break;
        }
    }
    std::fclose(output);
    const int closeStatus = unzCloseCurrentFile(zip);
    unzClose(zip);

    if (writeFailed || read < 0 || closeStatus != UNZ_OK) {
        error = "Could not extract '" + std::string(entryName) +
                "' from the root filesystem ZIP.";
        fs::remove(temporary, ec);
        return false;
    }

    fs::rename(temporary, destination, ec);
    if (ec) {
        error = "Could not install Wine registry overlay '" +
                destination.string() + "': " + ec.message();
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

bool ensureRegistry(const std::string& archivePath, const char* entryName,
                    const fs::path& destination, bool& extracted,
                    std::string& error) {
    std::error_code ec;
    if (fs::is_regular_file(destination, ec) && !ec) {
        return true;
    }
    extracted = true;
    return extractEntry(archivePath, entryName, destination, error);
}

}  // namespace

bool setWineRegistryValue(std::string& registry,
                          const std::string& section,
                          const std::string& name,
                          const std::string& serialisedValue) {
    const std::string sectionLine = escapedSection(section);
    const std::string keyPrefix = "\"" + name + "\"=";
    const std::string replacement = keyPrefix + serialisedValue;

    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= registry.size()) {
        const size_t end = registry.find('\n', start);
        std::string line = registry.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    const auto isSectionLine = [&sectionLine](const std::string& line) {
        return line.rfind(sectionLine, 0) == 0 &&
               (line.size() == sectionLine.size() ||
                line[sectionLine.size()] == ' ');
    };

    size_t sectionIndex = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        // Wine appends a key timestamp after the closing bracket. Match the
        // section name while preserving that timestamp verbatim.
        if (isSectionLine(lines[i])) {
            sectionIndex = i;
            break;
        }
    }

    bool changed = false;
    if (sectionIndex == lines.size()) {
        if (!lines.empty() && !lines.back().empty()) {
            lines.push_back("");
        }
        lines.push_back(sectionLine);
        lines.push_back(replacement);
        changed = true;
    } else {
        size_t insertion = lines.size();
        for (size_t i = sectionIndex + 1; i < lines.size(); ++i) {
            if (!lines[i].empty() && lines[i].front() == '[') {
                insertion = i;
                break;
            }
            if (lines[i].rfind(keyPrefix, 0) == 0) {
                if (lines[i] != replacement) {
                    lines[i] = replacement;
                    changed = true;
                }
                insertion = lines.size();
                break;
            }
        }
        if (insertion != lines.size()) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertion),
                         replacement);
            changed = true;
        } else {
            bool keyFound = false;
            for (size_t i = sectionIndex + 1; i < lines.size(); ++i) {
                if (!lines[i].empty() && lines[i].front() == '[') {
                    break;
                }
                if (lines[i].rfind(keyPrefix, 0) == 0) {
                    keyFound = true;
                    break;
                }
            }
            if (!keyFound) {
                lines.push_back(replacement);
                changed = true;
            }
        }
    }

    if (!changed) {
        return false;
    }
    std::ostringstream rebuilt;
    for (size_t i = 0; i < lines.size(); ++i) {
        rebuilt << lines[i];
        if (i + 1 < lines.size()) {
            rebuilt << '\n';
        }
    }
    registry = rebuilt.str();
    return true;
}

bool installBundledDxvk(const std::string& sourceDirectory,
                        const std::string& writableRootPath,
                        bool& changed,
                        std::string& error) {
    changed = false;
    if (sourceDirectory.empty()) {
        return true;
    }
    std::error_code ec;
    const fs::path source(sourceDirectory);
    if (!fs::is_directory(source, ec)) {
        return true;
    }
    const fs::path destination = fs::path(writableRootPath) / "home" /
        "username" / ".wine" / "drive_c" / "dxvk";
    fs::create_directories(destination, ec);
    if (ec) {
        error = "Could not create the DXVK directory: " + ec.message();
        return false;
    }

    for (const fs::directory_entry& entry : fs::directory_iterator(source, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const fs::path& from = entry.path();
        if (from.extension() != ".dll") {
            continue;
        }
        const fs::path to = destination / from.filename();

        // Always overwrite. An earlier version skipped files whose size
        // already matched, which silently kept a stale module: rebuilding DXVK
        // with a one-line feature change produced a byte-identical size, so the
        // device kept running the previous build and reported the very error
        // the change fixed. Copying roughly eight megabytes per launch is
        // irrelevant next to Wine's cold start, and correctness here is not
        // worth trading for it.
        std::error_code copyEc;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, copyEc);
        if (copyEc) {
            error = "Could not install " + from.filename().string() + ": " +
                    copyEc.message();
            return false;
        }
        changed = true;
    }
    return true;
}

namespace {

bool isFontFileName(const std::string& lowerName) {
    static const char* const suffixes[] = {".ttf", ".ttc", ".otf", ".fon"};
    for (const char* suffix : suffixes) {
        const std::string candidate(suffix);
        if (lowerName.size() > candidate.size() &&
            lowerName.compare(lowerName.size() - candidate.size(),
                              candidate.size(), candidate) == 0) {
            return true;
        }
    }
    return false;
}

std::string lowered(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool directoryContainsExactName(const fs::path& directory,
                                const std::string& name) {
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().filename().string() == name) {
            return true;
        }
    }
    return false;
}

bool ensureLowercaseGuestAlias(const fs::path& hostTarget,
                               const std::string& guestTarget) {
    const std::string lowerName = lowered(hostTarget.filename().string());
    if (lowerName == hostTarget.filename().string() ||
        directoryContainsExactName(hostTarget.parent_path(), lowerName)) {
        return false;
    }

    // Boxedwine represents a guest symbolic link as a small host-side .link
    // file.  Keep Wine's canonical shell-folder spelling and expose the
    // lowercase spelling as an alias to the same directory instead of making
    // two directories whose contents can diverge.
    const fs::path alias = hostTarget.parent_path() / (lowerName + ".link");
    {
        std::ifstream existing(alias, std::ios::binary);
        if (existing) {
            const std::string contents((std::istreambuf_iterator<char>(existing)),
                                       std::istreambuf_iterator<char>());
            if (contents == guestTarget) {
                return false;
            }
        }
    }

    std::ofstream stream(alias, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(guestTarget.data(),
                 static_cast<std::streamsize>(guestTarget.size()));
    return static_cast<bool>(stream);
}

// True when `lowerPath` sits directly or indirectly under `lowerPrefix`.
bool isUnder(const std::string& lowerPath, const char* lowerPrefix) {
    const std::string prefix(lowerPrefix);
    return lowerPath.size() > prefix.size() &&
           lowerPath.compare(0, prefix.size(), prefix) == 0;
}

bool isScalableFontName(const std::string& lowerName) {
    static const char* const suffixes[] = {".ttf", ".ttc", ".otf"};
    for (const char* suffix : suffixes) {
        const std::string candidate(suffix);
        if (lowerName.size() > candidate.size() &&
            lowerName.compare(lowerName.size() - candidate.size(),
                              candidate.size(), candidate) == 0) {
            return true;
        }
    }
    return false;
}

void countFormat(GuestFontCensus& census, const std::string& lowerName) {
    if (isScalableFontName(lowerName)) {
        census.scalable++;
    } else {
        census.bitmap++;
    }
}

void noteExample(GuestFontCensus& census, const std::string& name) {
    if (std::count(census.examples.begin(), census.examples.end(), ',') >= 2) {
        return;
    }
    if (!census.examples.empty()) {
        census.examples += ", ";
    }
    census.examples += name;
}

}  // namespace

GuestFontCensus censusGuestFonts(const std::string& rootFilesystemZipPath,
                                 const std::string& writableRootPath) {
    GuestFontCensus census;

    std::error_code ec;
    const fs::path prefixFonts = fs::path(writableRootPath) / "home" /
        "username" / ".wine" / "drive_c" / "windows" / "Fonts";
    if (fs::is_directory(prefixFonts, ec)) {
        for (const fs::directory_entry& entry :
             fs::directory_iterator(prefixFonts, ec)) {
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc) || entryEc) {
                continue;
            }
            const std::string name = entry.path().filename().string();
            const std::string lowerName = lowered(name);
            if (isFontFileName(lowerName)) {
                census.inPrefix++;
                countFormat(census, lowerName);
                noteExample(census, name);
            }
        }
    }

    if (rootFilesystemZipPath.empty()) {
        census.ok = true;
        return census;
    }
    unzFile zip = unzOpen64(rootFilesystemZipPath.c_str());
    if (zip == nullptr) {
        census.error = "Could not open the root filesystem ZIP to count its "
                       "fonts.";
        return census;
    }

    // Walk the central directory only. Names are all that is needed, so no
    // entry is opened and nothing is decompressed - this stays cheap on an
    // archive with tens of thousands of entries.
    int status = unzGoToFirstFile(zip);
    while (status == UNZ_OK) {
        char rawName[512] = {};
        unz_file_info64 info{};
        if (unzGetCurrentFileInfo64(zip, &info, rawName, sizeof(rawName) - 1,
                                    nullptr, 0, nullptr, 0) != UNZ_OK) {
            break;
        }

        std::string name(rawName);
        std::replace(name.begin(), name.end(), '\\', '/');
        const std::string lowerName = lowered(name);
        if (isFontFileName(lowerName)) {
            const std::string base =
                name.substr(name.find_last_of('/') + 1);
            if (isUnder(lowerName,
                        "home/username/.wine/drive_c/windows/fonts/")) {
                census.inRootFilesystem++;
                countFormat(census, lowerName);
                noteExample(census, base);
            } else if (isUnder(lowerName, "opt/wine/share/wine/fonts/")) {
                census.wineBundled++;
                countFormat(census, lowerName);
                noteExample(census, base);
            }
        }
        status = unzGoToNextFile(zip);
    }
    unzClose(zip);

    census.ok = true;
    return census;
}

GuestFontInstallResult installGuestFonts(const std::string& sourceDirectory,
                                         const std::string& writableRootPath) {
    GuestFontInstallResult result;
    if (sourceDirectory.empty()) {
        return result;
    }
    std::error_code ec;
    const fs::path source(sourceDirectory);
    if (!fs::is_directory(source, ec)) {
        return result;
    }

    const fs::path destination = fs::path(writableRootPath) / "home" /
        "username" / ".wine" / "drive_c" / "windows" / "Fonts";

    bool createdDestination = false;
    for (const fs::directory_entry& entry : fs::directory_iterator(source, ec)) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entryEc) {
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (extension != ".ttf" && extension != ".ttc" &&
            extension != ".otf" && extension != ".fon") {
            continue;
        }
        result.available++;

        // Deferred until the first real font is found, so a user with an
        // empty Fonts folder does not get an empty directory written into
        // every prefix.
        if (!createdDestination) {
            std::error_code createEc;
            fs::create_directories(destination, createEc);
            if (createEc) {
                result.ok = false;
                result.error = "Could not create the guest font directory: " +
                               createEc.message();
                return result;
            }
            createdDestination = true;
        }

        const fs::path target = destination / entry.path().filename();
        std::error_code copyEc;
        if (fs::exists(target, copyEc)) {
            continue;
        }
        fs::copy_file(entry.path(), target, copyEc);
        if (copyEc) {
            result.ok = false;
            result.error = "Could not install the font " +
                           entry.path().filename().string() + ": " +
                           copyEc.message();
            return result;
        }
        result.installed++;
    }
    return result;
}

WinePrefixPreparationResult prepareWinePrefix(
    const std::string& rootFilesystemZipPath,
    const std::string& writableRootPath,
    WineRenderer renderer,
    WineAnsiCodepage codepage) {
    WinePrefixPreparationResult result;
    const fs::path wineDirectory =
        fs::path(writableRootPath) / "home" / "username" / ".wine";
    const fs::path userRegistry = wineDirectory / "user.reg";
    const fs::path systemRegistry = wineDirectory / "system.reg";

    bool extracted = false;
    if (!ensureRegistry(rootFilesystemZipPath, kUserRegistryEntry, userRegistry,
                        extracted, result.error) ||
        !ensureRegistry(rootFilesystemZipPath, kSystemRegistryEntry, systemRegistry,
                        extracted, result.error)) {
        return result;
    }
    result.changed = extracted;

    {
        std::string contents;
        if (!readFile(userRegistry, contents, result.error)) {
            return result;
        }
        bool userRegistryChanged = false;

        // Wine offers to download Wine Mono and Wine Gecko the first time an
        // application touches mscoree or mshtml, because the root filesystem
        // ships neither. On a phone that dialog is a dead end: it appears
        // over the game with no obvious cause, the download is large, and it
        // has to happen again for every prefix, which is once per imported
        // game. Disabling the two modules is Wine's own documented way to
        // suppress the offer - the same thing WINEDLLOVERRIDES="mscoree,
        // mshtml=" does - and it costs only what was already unavailable:
        // a .NET Framework or embedded-Internet-Explorer application now
        // fails to find the module instead of asking for a runtime BoxedVN
        // cannot supply. Everything else is unaffected; these two modules are
        // not used by native Windows code.
        for (const char* missingRuntime : {"mscoree", "mshtml"}) {
            userRegistryChanged |= setWineRegistryValue(
                contents, "Software\\Wine\\DllOverrides", missingRuntime,
                "\"\"");
        }

        if (renderer != WineRenderer::Default) {
            const char* rendererValue = renderer == WineRenderer::Vulkan
                ? "\"vulkan\""
                : "\"gdi\"";
            userRegistryChanged |= setWineRegistryValue(
                contents, "Software\\Wine\\Direct3D", "renderer", rendererValue);

            if (renderer == WineRenderer::Vulkan) {
                // Metal has no geometry shader stage, so MoltenVK reports
                // geometryShader = false on every device, and it always will.
                // Direct3D 10 requires geometry shaders, so wined3d's Vulkan
                // adapter can never reach shader model 4. Its DXBC reader then
                // skips every SM4+ code chunk ("Skipping SM4+ shader"), and a
                // shader with nothing left to read fails with E_INVALIDARG.
                //
                // Saya's device log shows both halves of this. Shaders carrying an
                // Aon9 feature-level-9 chunk are read as ps_2_1/vs_2_0 and created
                // successfully; SM4-only shaders fail, and every draw with them
                // fails behind it.
                //
                // Capping the reported shader model makes the device honestly
                // feature level 9_3 instead of advertising a capability the
                // backend will not honour. A game that ships _level_9_x shaders -
                // as this one does - then selects the set that actually works.
                // This cannot raise a capability, only stop one being claimed, so
                // it cannot break a title that already renders.
                for (const char* stage : {"MaxShaderModelVS", "MaxShaderModelPS"}) {
                    userRegistryChanged |= setWineRegistryValue(
                        contents, "Software\\Wine\\Direct3D", stage,
                        "dword:00000003");
                }
                userRegistryChanged |= setWineRegistryValue(
                    contents, "Software\\Wine\\Direct3D", "MaxShaderModelGS",
                    "dword:00000000");
            }
        }

        if (userRegistryChanged) {
            if (!atomicWrite(userRegistry, contents, result.error)) {
                return result;
            }
            result.changed = true;
        }
    }

    std::string contents;
    if (!readFile(systemRegistry, contents, result.error)) {
        return result;
    }
    if (setWineRegistryValue(contents,
                             "System\\ControlSet001\\Services\\winebth",
                             "Start", "dword:00000004")) {
        result.changed = true;
    }
    if (setWineRegistryValue(contents,
                             "System\\ControlSet001\\Services\\winebus",
                             "Start", "dword:00000003")) {
        result.changed = true;
    }
    // Wine 11 auto-starts NDIS on every cold prefix, but Boxedwine exposes
    // networking to Wine through nsiproxy/netlink rather than an NDIS kernel
    // driver. The unsupported service waits for its start result for 57-59
    // seconds before services.exe continues. Disable only that dead driver;
    // Wine services and nsiproxy remain enabled.
    if (setWineRegistryValue(contents,
                             "System\\ControlSet001\\Services\\NDIS",
                             "Start", "dword:00000004")) {
        result.changed = true;
    }

    // Wine 10's services.exe starts root PnP drivers even when their service
    // Start value is SERVICE_DISABLED.  wineboot also leaves an existing root
    // device untouched, so keeping the device but clearing its associated
    // service prevents winebth from being selected by the root-device scan on
    // this and subsequent launches.  Bluetooth is not exposed by Boxedwine on
    // iOS; attempting to start the driver only adds a roughly one-minute
    // timeout before any Wine window can appear.
    if (setWineRegistryValue(
            contents,
            "System\\ControlSet001\\Enum\\ROOT\\WINE\\WINEBTH",
            "Service", "\"\"")) {
        result.changed = true;
    }

    // Build 24 tested disconnecting winebus because the timestamped trace
    // resumed near Winedevice2. Physical-device timing remained unchanged:
    // the same 57-61 second cold-start gap occurred, while a second guest in
    // the same Boxedwine process started quickly. Restore Wine's HID bus so a
    // disproven optimization does not disable future raw-HID/controllers.
    if (setWineRegistryValue(
            contents,
            "System\\ControlSet001\\Enum\\ROOT\\WINE\\WINEBUS",
            "Service", "\"winebus\"")) {
        result.changed = true;
    }

    // Windows programs assume the user's shell folders exist. Wine creates
    // them when it bootstraps a prefix, but BoxedVN's prefixes are made by
    // copying the registry out of a pre-built root filesystem, so whatever
    // that root happened to contain is what a game gets - and a game that
    // cannot find My Documents does not report a missing folder, it reports
    // whatever its own fallback path fails at three steps later.
    //
    // Some Windows runtimes turn their file URLs lowercase before asking Wine
    // to create the save directory. The Boxedwine overlay is case-sensitive,
    // so merely creating "Documents" still leaves the exact parent
    // "documents" missing. Give each conventional shell folder a lowercase
    // guest symlink to the canonical directory. This is a prefix-level Windows
    // compatibility rule; no game or game-specific save path is named here.
    {
        const fs::path user = fs::path(writableRootPath) / "home" / "username" /
            ".wine" / "drive_c" / "users" / "username";
        const std::string guestUser =
            "/home/username/.wine/drive_c/users/username";
        for (const char* folder : {"Documents", "Desktop", "Downloads",
                                   "Music", "Pictures", "Videos",
                                   "Saved Games", "AppData", "AppData/Roaming",
                                   "AppData/Local"}) {
            std::error_code createEc;
            const fs::path relative(folder);
            const fs::path target = user / relative;
            if (!fs::exists(target, createEc)) {
                fs::create_directories(target, createEc);
                if (!createEc) {
                    result.changed = true;
                }
            }
            if (!createEc && ensureLowercaseGuestAlias(
                    target, guestUser + "/" + relative.generic_string())) {
                result.changed = true;
            }
        }
    }

    if (codepage == WineAnsiCodepage::Japanese) {
        // What Wine consults to convert a program's 8-bit strings, including
        // every filename it opens. MACCP is the Macintosh codepage Wine also
        // keeps here; 10001 is its Japanese counterpart and is set for
        // consistency rather than because anything reads it.
        for (const auto& entry : {std::make_pair("ACP", "\"932\""),
                                  std::make_pair("OEMCP", "\"932\""),
                                  std::make_pair("MACCP", "\"10001\"")}) {
            if (setWineRegistryValue(
                    contents, "System\\ControlSet001\\Control\\Nls\\CodePage",
                    entry.first, entry.second)) {
                result.changed = true;
            }
        }
        if (setWineRegistryValue(
                contents, "System\\ControlSet001\\Control\\Nls\\Language",
                "Default", "\"0411\"")) {
            result.changed = true;
        }
    }

    if (result.changed && !atomicWrite(systemRegistry, contents, result.error)) {
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace boxedvn
