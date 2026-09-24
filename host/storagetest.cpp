// Persistence: slot arithmetic, erase semantics, and rejection.
//
// Everything here fails silently on hardware if it is wrong. A slot stride that
// is not sector-aligned erases the tail of its neighbour. An Erase() given a
// length instead of an end address leaves most of the old save in place, and
// because NOR flash writes can only clear bits, the result reads back as a
// valid header followed by the AND of two patterns. Neither shows up as an
// error — both show up weeks later as "that pattern sounds wrong".

#include <cstdio>
#include <cstring>

#include "../src/io/storage.h"

using namespace drom;

namespace {

int failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("  %-58s %s\n", what, ok ? "ok" : "*** FAIL ***");
    if(!ok)
        ++failures;
}

void Is(Storage::Result got, Storage::Result want, const char *what)
{
    const bool ok = got == want;
    std::printf("  %-58s %s%s\n", what, ok ? "ok" : "*** FAIL *** got ",
                ok ? "" : Storage::Describe(got));
    if(!ok)
        ++failures;
}

// 8 MB of fake chip, plus the module under test. Both are statics for the same
// reason Machine is: Storage carries a Patch-sized staging buffer.
RamFlash<kQspiBytes> g_flash;
Storage              g_storage;
Patch                g_a, g_b, g_out;

/// A patch that is distinguishable from any other by `tag`.
void MakePatch(Patch &p, uint8_t tag)
{
    InitPatch(p);
    p.pattern.bpm_x10 = static_cast<uint16_t>(1000 + tag);
    p.pattern.swing   = tag;
    for(int t = 0; t < kNumTracks; ++t)
        for(int s = 0; s < kMaxSteps; ++s)
        {
            p.pattern.tracks[t].steps[s].velocity = static_cast<uint8_t>((tag + t + s) & 0x7F);
            p.pattern.tracks[t].steps[s].flags    = ((s + tag) % 4 == 0) ? kStepActive : 0;
        }
    p.kit.params[0][0] = 0.1f * static_cast<float>(tag);
}

bool Same(const Patch &x, const Patch &y)
{
    return std::memcmp(&x.pattern, &y.pattern, sizeof(Pattern)) == 0
           && std::memcmp(&x.kit, &y.kit, sizeof(Kit)) == 0;
}

} // namespace

int main()
{
    g_storage.Init(&g_flash);

    std::printf("layout:\n");
    {
        std::printf("    Patch %zu B, stride %u kB, patterns end 0x%06X, chip 0x%06X\n",
                    sizeof(Patch), kPatternStride / 1024,
                    kSongBase, kQspiBytes);
        Check(kFreeBase <= kQspiBytes, "the whole layout fits the chip");
        Check(kPatternStride % 4096 == 0, "pattern slots are sector-aligned");
        Check(kPatternStride >= sizeof(Patch), "a slot holds a whole Patch");
        Check(kPatternBase >= 0x100000,
              "user data starts clear of the staged app image");
    }

    std::printf("\nround trip:\n");
    {
        MakePatch(g_a, 7);
        Is(g_storage.SavePatch(3, g_a), Storage::Result::Ok, "a patch saves");
        Is(g_storage.LoadPatch(3, g_out), Storage::Result::Ok, "and loads back");
        Check(Same(g_a, g_out), "byte for byte identical");
    }

    std::printf("\nan un-written slot:\n");
    {
        // Erased NOR reads as 0xFF, so this must be distinguishable from a
        // corrupt save — on a fresh unit it is the normal case.
        Is(g_storage.LoadPatch(9, g_out), Storage::Result::Empty,
           "reads as Empty, not as corruption");
    }

    std::printf("\noverwrite:\n");
    {
        MakePatch(g_a, 11);
        MakePatch(g_b, 200);
        Is(g_storage.SavePatch(4, g_a), Storage::Result::Ok, "first save");
        Is(g_storage.SavePatch(4, g_b), Storage::Result::Ok, "second save over it");
        Is(g_storage.LoadPatch(4, g_out), Storage::Result::Ok, "loads");
        // If Erase() had been given a length rather than an end address, only
        // the first sector would clear and the rest would read as A AND B.
        Check(Same(g_b, g_out), "the slot holds the second patch, not both ANDed");
        Check(!Same(g_a, g_out), "and definitely not the first");
    }

    std::printf("\nslot isolation:\n");
    {
        static Patch p[3];
        for(int i = 0; i < 3; ++i)
        {
            MakePatch(p[i], static_cast<uint8_t>(40 + i));
            g_storage.SavePatch(static_cast<uint32_t>(20 + i), p[i]);
        }
        // Re-saving the middle slot must not disturb either neighbour — this is
        // what the sector alignment is for.
        MakePatch(g_a, 99);
        g_storage.SavePatch(21, g_a);

        bool ok = true;
        g_storage.LoadPatch(20, g_out); ok &= Same(p[0], g_out);
        g_storage.LoadPatch(22, g_out); ok &= Same(p[2], g_out);
        Check(ok, "rewriting a slot leaves both neighbours intact");
        g_storage.LoadPatch(21, g_out);
        Check(Same(g_a, g_out), "and the rewritten slot holds the new patch");
    }

    std::printf("\nreject, never reinterpret:\n");
    {
        MakePatch(g_a, 5);
        g_storage.SavePatch(30, g_a);

        // Bump the stored version. The payload is still a perfectly good Patch
        // of the previous shape, which is exactly why reinterpreting it would
        // sound like corruption rather than like a mismatch.
        const uint32_t addr = PatternAddr(30);
        g_flash.Poke(addr + 4, static_cast<uint8_t>(kPatchVersion + 1));
        Is(g_storage.LoadPatch(30, g_out), Storage::Result::BadVersion,
           "a version mismatch is rejected");

        g_storage.SavePatch(31, g_a);
        g_flash.Poke(PatternAddr(31), 0x00);
        Is(g_storage.LoadPatch(31, g_out), Storage::Result::BadMagic,
           "a broken magic is rejected");

        g_storage.SavePatch(32, g_a);
        g_flash.Poke(PatternAddr(32) + 6, 0x00); // payload_size low byte
        Is(g_storage.LoadPatch(32, g_out), Storage::Result::BadSize,
           "a size mismatch is rejected");
    }

    std::printf("\nbounds:\n");
    {
        Is(g_storage.SavePatch(kPatternSlots, g_a), Storage::Result::BadSlot,
           "one past the last pattern slot is refused");
        Is(g_storage.LoadPatch(kPatternSlots + 100, g_out), Storage::Result::BadSlot,
           "and so is a wild index");
    }

    std::printf("\nkits:\n");
    {
        static Kit k, kout;
        InitKit(k);
        k.params[3][2] = 0.321f;
        k.fx[static_cast<int>(FxId::DelayFeedback)] = 0.75f;
        std::strncpy(k.name, "TESTKIT", sizeof(k.name) - 1);

        Is(g_storage.SaveKit(5, k), Storage::Result::Ok, "a kit saves");
        Is(g_storage.LoadKit(5, kout), Storage::Result::Ok, "and loads");
        Check(std::memcmp(&k, &kout, sizeof(Kit)) == 0, "byte for byte identical");
        Is(g_storage.LoadKit(6, kout), Storage::Result::Empty,
           "an unwritten kit slot reads Empty");
        Is(g_storage.SaveKit(kKitSlots, k), Storage::Result::BadSlot,
           "and a bad kit slot is refused");
    }

    std::printf("\nsettings:\n");
    {
        Settings a, b;
        Check(!(a != b), "two default Settings compare equal");
        b.led_brightness_pct = 20;
        Check(a != b, "a changed field compares unequal");
        b = a;
        b.calibration[64] = 1.5f;
        Check(a != b, "and so does a changed calibration word");
        Check(sizeof(Settings) <= 4096, "Settings fits its 4 kB slot");
    }

    std::printf("\nno flash attached:\n");
    {
        Storage bare;
        Is(bare.SavePatch(0, g_a), Storage::Result::NoFlash,
           "saving without Init is refused rather than crashing");
    }

    if(failures)
    {
        std::printf("\n%d storage check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall storage tests passed\n");
    return 0;
}
