#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../machine.h"
#include "ui.h"

namespace drom {

/// 128x64 at a size readable from playing distance is about 21 columns by 4
/// rows. Rendering to text rather than pixels keeps this testable and keeps
/// the font choice a display-driver concern.
struct DisplayLines
{
    static constexpr int kCols = 21;
    static constexpr int kRows = 4;
    char text[kRows][kCols + 1] = {};

    void Clear()
    {
        for(int r = 0; r < kRows; ++r)
            text[r][0] = '\0';
    }
};

/// The screen explains; it never gates. Touch a knob and it tells you what
/// that knob is doing; otherwise it shows where you are in the pattern. No
/// menu ever becomes the only route to a feature.
class DisplayRenderer
{
  public:
    /// How long a knob stays on screen after you stop moving it.
    static constexpr uint32_t kParamHoldMs = 1500;

    void Render(const Machine &m, const Ui &ui, DisplayLines &d) const
    {
        d.Clear();
        const int   sel  = ui.selected_track();
        const char *name = kTrackName[sel];

        const int pot = ui.last_pot();
        if(pot >= 0 && ui.since_last_pot_ms() < kParamHoldMs)
        {
            RenderParam(m, ui, sel, name, pot, d);
            return;
        }
        RenderOverview(m, ui, sel, name, d);
    }

  private:
    static void RenderParam(const Machine &m,
                            const Ui      &ui,
                            int            sel,
                            const char    *name,
                            int            pot,
                            DisplayLines  &d)
    {
        const ParamId id     = static_cast<ParamId>(pot);
        const float   stored = m.patch().kit.params[sel][pot];

        std::snprintf(d.text[0], DisplayLines::kCols + 1, "%-4s %s", name, ParamName(id));

        if(ui.held_step() >= 0)
            std::snprintf(d.text[1], DisplayLines::kCols + 1,
                          "LOCK step %-2d  %3d", ui.held_step() + 1, Pct(ui.last_raw(pot)));
        else
            std::snprintf(d.text[1], DisplayLines::kCols + 1, "%18d", Pct(stored));

        if(!ui.pot_caught(pot))
        {
            // Soft takeover made visible. Without this the knob feels broken:
            // you turn it, nothing happens, and nothing says why.
            std::snprintf(d.text[2], DisplayLines::kCols + 1,
                          "knob %3d -> %3d", Pct(ui.last_raw(pot)), Pct(stored));
            std::snprintf(d.text[3], DisplayLines::kCols + 1, "turn to pick up");
        }
        else
        {
            std::snprintf(d.text[3], DisplayLines::kCols + 1, "%s", Bar(stored));
        }
    }

    static void RenderOverview(const Machine &m,
                               const Ui      &ui,
                               int            sel,
                               const char    *name,
                               DisplayLines  &d)
    {
        const auto &st   = m.state();
        const int   pos  = static_cast<int>(st.position[sel].load(std::memory_order_relaxed));
        const int   len  = m.patch().pattern.tracks[sel].length;
        const bool  play = st.playing.load(std::memory_order_relaxed);
        const bool  ext  = st.external_sync.load(std::memory_order_relaxed);
        const float bpm  = st.tempo.load(std::memory_order_relaxed);

        std::snprintf(d.text[0], DisplayLines::kCols + 1, "%-4s %s%s", name,
                      play ? "PLAY" : "STOP",
                      m.patch().pattern.tracks[sel].muted ? "  MUTE" : "");
        std::snprintf(d.text[1], DisplayLines::kCols + 1, "%5.1f BPM  %s", bpm,
                      ext ? "EXT" : "INT");
        std::snprintf(d.text[2], DisplayLines::kCols + 1, "step %2d/%-2d", pos + 1, len);
        if(ui.mode() == Ui::Mode::Mute)
            std::snprintf(d.text[3], DisplayLines::kCols + 1, "MUTE");
    }

    static int Pct(float v)
    {
        if(v < 0.f) v = 0.f;
        if(v > 1.f) v = 1.f;
        return static_cast<int>(v * 100.f + 0.5f);
    }

    /// A crude bar, because a number alone is hard to read while playing.
    static const char *Bar(float v)
    {
        static char buf[DisplayLines::kCols + 1];
        const int   n = Pct(v) * 16 / 100;
        int         i = 0;
        for(; i < n && i < 16; ++i)
            buf[i] = '#';
        for(; i < 16; ++i)
            buf[i] = '.';
        buf[i] = '\0';
        return buf;
    }
};

} // namespace drom
