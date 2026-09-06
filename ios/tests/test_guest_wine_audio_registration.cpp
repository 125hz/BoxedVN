#include "boxedvn_test.h"
#include "guest_wine_audio_registration.h"

BOXEDVN_TEST(dxdiag_registration_preserves_overrides_and_uses_apartment) {
    std::string text = "WINE REGISTRY Version 2\n#arch=win64\n";
    CHECK(!boxedvn::registerWineDxDiagProvider(text, false, false));
    CHECK(boxedvn::registerWineDxDiagProvider(text, false, true));
    CHECK(text.find("Wow6432Node") != std::string::npos);
    CHECK(text.find("syswow64\\\\dxdiagn.dll") != std::string::npos);
    CHECK(text.find("system32") == std::string::npos);
    CHECK(text.find("\"ThreadingModel\"=\"Apartment\"") != std::string::npos);
    CHECK(boxedvn::registerWineDxDiagProvider(text, true, true));
    auto saved = text;
    CHECK(!boxedvn::registerWineDxDiagProvider(text, true, true));
    CHECK_EQ(text, saved);
    const auto path = text.find("syswow64\\\\dxdiagn.dll");
    text.replace(path, std::string("syswow64\\\\dxdiagn.dll").size(), "custom\\\\provider.dll");
    saved = text;
    CHECK(!boxedvn::registerWineDxDiagProvider(text, true, true));
    CHECK_EQ(text, saved);
}

BOXEDVN_TEST(audio_registration_repairs_only_packaged_architectures) {
    std::string text = "WINE REGISTRY Version 2\n#arch=win64\n";
    CHECK(!boxedvn::registerWineAudioEnumerator(text, false, false));
    CHECK(boxedvn::registerWineAudioEnumerator(text, true, false));
    CHECK(text.find("system32") != std::string::npos);
    CHECK(text.find("Wow6432Node") == std::string::npos);
    CHECK(boxedvn::registerWineAudioEnumerator(text, true, true));
    CHECK(text.find("syswow64") != std::string::npos);
    const std::string saved = text;
    CHECK(!boxedvn::registerWineAudioEnumerator(text, true, true));
    CHECK_EQ(text, saved);
}

BOXEDVN_TEST(audio_registration_preserves_existing_case_insensitive_override) {
    std::string text = "WINE REGISTRY Version 2\r\n"
        "[Software\\\\Classes\\\\CLSID\\\\{BCDE0395-E52F-467C-8E3D-C4579291692E}\\\\InprocServer32] 123\r\n"
        "#time=123\r\n@=\"custom.dll\"\r\n\"ThreadingModel\"=\"Apartment\"\r\n";
    const std::string saved = text;
    CHECK(!boxedvn::registerWineAudioEnumerator(text, true, false));
    CHECK_EQ(text, saved);
}

BOXEDVN_TEST(audio_registration_keeps_neighbouring_sections_and_timestamp) {
    std::string text = "[First] 1\n#time=abc\n\"Unrelated\"=\"value\"\n[Last] 2\n@=\"keep\"";
    CHECK(boxedvn::insertMissingWineRegistryValue(text, "First", "@", "\"new\""));
    CHECK(text.find("[First] 1\n#time=abc") != std::string::npos);
    CHECK(text.find("@=\"new\"\n\n[Last] 2\n@=\"keep\"") != std::string::npos);
}

BOXEDVN_TEST(documents_namespace_registration_enables_filesystem_parsing) {
    std::string text = "WINE REGISTRY Version 2\n#arch=win64\n";
    CHECK(!boxedvn::registerWineDocumentsFolder(text, false, false));
    CHECK(boxedvn::registerWineDocumentsFolder(text, false, true));
    CHECK(text.find("Wow6432Node") != std::string::npos);
    CHECK(text.find("system32") == std::string::npos);
    CHECK(text.find("syswow64\\\\shell32.dll") != std::string::npos);
    CHECK(text.find("\\\\ShellFolder] 0\n\"WantsForParsing\"=\"\"") != std::string::npos);
    CHECK(boxedvn::registerWineDocumentsFolder(text, true, true));
    auto saved = text;
    CHECK(!boxedvn::registerWineDocumentsFolder(text, true, true));
    CHECK_EQ(text, saved);
    // A custom COM server and its threading model survive a prefix upgrade.
    const auto path = text.find("syswow64\\\\shell32.dll");
    text.replace(path, std::string("syswow64\\\\shell32.dll").size(), "custom\\\\shell.dll");
    saved = text;
    CHECK(!boxedvn::registerWineDocumentsFolder(text, true, true));
    CHECK_EQ(text, saved);
}

BOXEDVN_TEST(wbem_registration_is_architecture_scoped_and_preserves_existing_values) {
    std::string text = "WINE REGISTRY Version 2\n";
    CHECK(!boxedvn::registerWineWbemLocator(text, false, false));
    CHECK(boxedvn::registerWineWbemLocator(text, true, false));
    CHECK(text.find("Wow6432Node") == std::string::npos);
    CHECK(text.find("\"ThreadingModel\"=\"Both\"") != std::string::npos);
    CHECK(boxedvn::registerWineWbemLocator(text, true, true));
    const auto path = text.find("syswow64\\\\wbemprox.dll");
    CHECK(path != std::string::npos);
    text.replace(path, std::string("syswow64\\\\wbemprox.dll").size(), "custom\\\\wmi.dll");
    auto saved = text;
    CHECK(!boxedvn::registerWineWbemLocator(text, true, true));
    CHECK_EQ(text, saved);
}

BOXEDVN_TEST(media_device_registration_is_architecture_scoped_and_preserves_existing_values) {
    std::string text = "WINE REGISTRY Version 2\n";
    CHECK(!boxedvn::registerWineMediaDeviceEnumerator(text, false, false));
    CHECK(boxedvn::registerWineMediaDeviceEnumerator(text, true, false));
    CHECK(text.find("Wow6432Node") == std::string::npos);
    CHECK(text.find("\"ThreadingModel\"=\"Both\"") != std::string::npos);
    CHECK(boxedvn::registerWineMediaDeviceEnumerator(text, true, true));
    const auto path = text.find("syswow64\\\\devenum.dll");
    CHECK(path != std::string::npos);
    text.replace(path, std::string("syswow64\\\\devenum.dll").size(), "custom\\\\wmi.dll");
    auto saved = text;
    CHECK(!boxedvn::registerWineMediaDeviceEnumerator(text, true, true));
    CHECK_EQ(text, saved);
}
