// PathGuardTests.cpp — filesDropped self-drop guard: gent::pathIsWithin.
//
// filesDropped (PluginEditor.cpp) rejects drops of our OWN temp exports —
// GentSampler_Pad*.wav / Performance.mid, which live in tempDir(). Re-loading
// one restores a silent/empty source and reads as a paint bug (CLAUDE.md
// landmine 2026-07-02). Production canonicalizes both paths via
// juce::File::getFullPathName() and calls gent::pathIsWithin(); these cases
// pin the pure comparison — case-insensitivity, separator-agnosticism, the
// trailing-slash strip, and the sibling-prefix false-positive guard — without
// JUCE or any filesystem access (logic-only test rig).

#include "doctest.h"
#include "EngineMath.h"

namespace
{
    // A representative Windows temp export dir + one of our exported files.
    const std::string kDir  = "C:\\Users\\JoeyD\\AppData\\Local\\Temp\\GentSampler";
    const std::string kPad  = "C:\\Users\\JoeyD\\AppData\\Local\\Temp\\GentSampler\\GentSampler_Pad12.wav";
    const std::string kPerf = "C:\\Users\\JoeyD\\AppData\\Local\\Temp\\GentSampler\\GentSampler_Performance.mid";
}

TEST_CASE ("PG.1 rejects our own temp exports (direct children of tempDir)")
{
    CHECK (gent::pathIsWithin (kPad,  kDir));
    CHECK (gent::pathIsWithin (kPerf, kDir));
}

TEST_CASE ("PG.2 case-insensitive — drive letter, folder, and file case all vary")
{
    CHECK (gent::pathIsWithin ("c:\\users\\joeyd\\appdata\\local\\temp\\gentsampler\\GENTSAMPLER_PAD12.WAV", kDir));
    CHECK (gent::pathIsWithin (kPad, "C:\\USERS\\JOEYD\\APPDATA\\LOCAL\\TEMP\\GENTSAMPLER"));
}

TEST_CASE ("PG.3 separator-agnostic — forward vs back slashes on either side")
{
    const std::string padFwd = "C:/Users/JoeyD/AppData/Local/Temp/GentSampler/GentSampler_Pad12.wav";
    CHECK (gent::pathIsWithin (padFwd, kDir));                       // fwd path, back dir
    CHECK (gent::pathIsWithin (kPad, "C:/Users/JoeyD/AppData/Local/Temp/GentSampler")); // back path, fwd dir
}

TEST_CASE ("PG.4 trailing slash on the dir is tolerated")
{
    CHECK (gent::pathIsWithin (kPad, kDir + "\\"));
    CHECK (gent::pathIsWithin (kPad, kDir + "/"));
}

TEST_CASE ("PG.5 a nested export (subfolder inside tempDir) is still inside")
{
    CHECK (gent::pathIsWithin (kDir + "\\sub\\GentSampler_Pad03.wav", kDir));
}

TEST_CASE ("PG.6 path == dir counts as within (defensive; not a real drop)")
{
    CHECK (gent::pathIsWithin (kDir, kDir));
    CHECK (gent::pathIsWithin (kDir + "\\", kDir));
}

TEST_CASE ("PG.7 sibling-prefix false positive is NOT matched")
{
    // A different folder that merely shares the tempDir name as a prefix must
    // not be treated as inside it — the classic string-prefix bug.
    CHECK_FALSE (gent::pathIsWithin ("C:\\Users\\JoeyD\\AppData\\Local\\Temp\\GentSampler2\\x.wav", kDir));
    CHECK_FALSE (gent::pathIsWithin ("C:\\Users\\JoeyD\\AppData\\Local\\Temp\\GentSamplerBak\\GentSampler_Pad12.wav", kDir));
}

TEST_CASE ("PG.8 a genuine user drop from elsewhere is allowed (guard returns false)")
{
    CHECK_FALSE (gent::pathIsWithin ("C:\\Music\\breaks\\amen.wav", kDir));
    CHECK_FALSE (gent::pathIsWithin ("D:\\Samples\\GentSampler_Pad12.wav", kDir)); // same filename, different tree
}

TEST_CASE ("PG.9 degenerate inputs never throw and never falsely match")
{
    CHECK_FALSE (gent::pathIsWithin ("", kDir));      // empty path
    CHECK_FALSE (gent::pathIsWithin (kPad, ""));      // empty dir -> never within
    CHECK_FALSE (gent::pathIsWithin ("", ""));        // both empty
    CHECK_FALSE (gent::pathIsWithin ("C:\\a", kDir)); // path shorter than dir
}

TEST_CASE ("PG.10 POSIX absolute paths (mac tempDir shape) — separator-agnostic core")
{
    // MACOS_PORT_SPEC.md — pathIsWithin is separator-agnostic and portable
    // (EngineMath.h normPathForCompare), but every existing fixture above is
    // Windows-shaped. Pin the POSIX equivalent: forward-slash absolute paths,
    // as juce::File::getFullPathName() would canonicalize on mac.
    const std::string macDir  = "/Users/joeyd/Library/Caches/GentSampler";
    const std::string macPad  = "/Users/joeyd/Library/Caches/GentSampler/GentSampler_Pad12.wav";
    const std::string macOutside = "/Users/joeyd/Music/breaks/amen.wav";
    const std::string macSiblingPrefix = "/Users/joeyd/Library/Caches/GentSampler2/x.wav";

    CHECK (gent::pathIsWithin (macPad, macDir));                 // inside
    CHECK_FALSE (gent::pathIsWithin (macOutside, macDir));       // genuinely elsewhere
    CHECK_FALSE (gent::pathIsWithin (macSiblingPrefix, macDir)); // sibling-prefix false positive
}
