#include "boxedvn_test.h"
#include "guest_wine_audio_registration.h"

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
