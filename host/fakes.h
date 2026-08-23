#pragma once

// Test doubles for the two things ParamUI is templated/injected on: a Board and an IEngine.
//
// Both are recording fakes rather than mocks - they answer honestly and remember what happened, so a
// test asserts on observable outcomes (what was written to the engine, what was drawn on the screen)
// instead of on call sequences.

#include "board/controls.h"
#include "engine/iengine.h"

#include <cstdint>
#include <string>
#include <vector>

namespace daisyapps::test {

// A Board with a screen, satisfying the duck-typed surface app/param_ui.h uses. Draw calls are
// recorded as text so a test can assert on what a player would actually read.
struct FakeBoard {
    static constexpr int  kAnalogCount    = 4;
    static constexpr int  kButtonCount    = 0;
    static constexpr int  kGateCount      = 2;
    static constexpr int  kIndicatorCount = 2;
    static constexpr int  kCvOutCount     = 2;
    static constexpr int  kAudioChannels  = 4;
    static constexpr bool kHasScreen      = true;
    static constexpr int  kScreenWidth    = 128;
    static constexpr int  kScreenHeight   = 64;

    struct Rect { int x, y, w, h; bool fill; };
    // The last value written to each indicator, and how many writes it has had. Recorded rather than
    // acted on, so a test asserts what a player would SEE - a colour and a brightness - instead of a
    // call sequence.
    struct Led { float r = 0.f, g = 0.f, b = 0.f; int writes = 0;
                 bool dark() const { return r <= 0.f && g <= 0.f && b <= 0.f; }
                 float peak() const { return r > g ? (r > b ? r : b) : (g > b ? g : b); } };

    std::vector<std::string> lines;    // every ScreenText of the CURRENT frame
    std::vector<Rect>        rects;
    int                      updates = 0;
    Led                      led[4];
    bool                     user_led = false;

    void ScreenClear() { lines.clear(); rects.clear(); }
    void ScreenText(int, int, const char* text, bool = true) { lines.emplace_back(text); }
    void ScreenRect(int x, int y, int w, int h, bool fill) { rects.push_back({x, y, w, h, fill}); }
    void ScreenUpdate() { updates++; }

    void SetIndicator(int idx, float r, float g, float b)
    {
        if (idx < 0 || idx >= kIndicatorCount) return;   // out of range is a no-op, as on a real board
        led[idx] = { r, g, b, led[idx].writes + 1 };
    }
    void SetUserLed(bool on) { user_led = on; }

    // True if any drawn line contains `needle`.
    bool drew(const char* needle) const
    {
        for (const std::string& l : lines)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }
    // The first drawn line containing `needle`, or "" if none does.
    std::string line_with(const char* needle) const
    {
        for (const std::string& l : lines)
            if (l.find(needle) != std::string::npos) return l;
        return {};
    }
};

// A board with no display, for the paths that must compile away or refuse (open_actions, render).
struct FakeScreenlessBoard : FakeBoard {
    static constexpr bool kHasScreen   = false;
    static constexpr int  kAnalogCount = 2;
    static constexpr int  kGateCount   = 0;
};

// A board with no discrete LEDs at all - the Daisy Patch, which has the OLED instead. The display
// adapter must compile away to nothing against this rather than write somewhere harmless.
struct FakeLedlessBoard : FakeBoard {
    static constexpr int kIndicatorCount = 0;
};

// A single mono LED, as on patch.init(): its driver collapses any colour to on/off, which is why the
// adapter has a brightness floor at all.
struct FakeMonoBoard : FakeBoard {
    static constexpr bool kHasScreen      = false;
    static constexpr int  kIndicatorCount = 1;
};

// An IEngine that stores its params, records the calls it received, and lets a test declare which
// params/configs are live and which pads exist.
class FakeEngine : public IEngine {
public:
    struct SetParam { ParamId id; DeckRef::Ref deck; float value; };
    struct SetConfig { ConfigId id; DeckRef::Ref deck; int value; };

    // --- required lifecycle (unused here, but IEngine is abstract without them) -----------------
    void init(const EngineContext&) override {}
    void prepare() override {}
    void process(const float* const*, float**, size_t) override {}

    // --- declarations a test controls ----------------------------------------------------------
    Capabilities capabilities() const override { return caps; }
    ParamMask    live_params() const override  { return param_mask; }
    ConfigMask   live_configs() const override { return config_mask; }
    const char*  param_label(ParamId id) const override
    {
        return labels[static_cast<uint8_t>(id)];
    }

    // --- params --------------------------------------------------------------------------------
    void set_param(ParamId id, DeckRef::Ref d, float v) override
    {
        values[deck_index(d)][static_cast<uint8_t>(id)] = v;
        writes.push_back({id, d, v});
    }
    float param(ParamId id, DeckRef::Ref d) const override
    {
        return values[deck_index(d)][static_cast<uint8_t>(id)];
    }

    bool set_config(ConfigId id, DeckRef::Ref d, int v) override
    {
        configs.push_back({id, d, v});
        return true;
    }

    // --- pads: overridden so engine_pads.h's detection reports them -----------------------------
    // (ParamUI takes the mask as a parameter, so a test can also pass one directly.)
    bool on_play_pad(DeckRef::Ref, bool reverse) override { plays++; last_reverse = reverse; return true; }
    void on_record_pad(DeckRef::Ref, bool) override { records++; }

    // --- helpers -------------------------------------------------------------------------------
    static int deck_index(DeckRef::Ref d) { return d == DeckRef::A ? 0 : 1; }

    void set_value(ParamId id, DeckRef::Ref d, float v)
    {
        values[deck_index(d)][static_cast<uint8_t>(id)] = v;
    }
    void clear_calls() { writes.clear(); configs.clear(); }

    Capabilities caps        = 0;
    ParamMask    param_mask  = 0;
    ConfigMask   config_mask = 0;
    const char*  labels[static_cast<int>(ParamId::Count)] = {};

    float values[2][static_cast<int>(ParamId::Count)] = {};
    std::vector<SetParam>  writes;
    std::vector<SetConfig> configs;
    int  plays = 0, records = 0;
    bool last_reverse = false;
};

// Build a ParamMask from a list of ParamIds.
inline IEngine::ParamMask mask_of(std::initializer_list<ParamId> ids)
{
    IEngine::ParamMask m = 0;
    for (ParamId id : ids) m |= IEngine::ParamMask{1} << static_cast<uint8_t>(id);
    return m;
}

inline IEngine::ConfigMask config_mask_of(std::initializer_list<ConfigId> ids)
{
    IEngine::ConfigMask m = 0;
    for (ConfigId id : ids) m |= static_cast<IEngine::ConfigMask>(1u << static_cast<uint8_t>(id));
    return m;
}

// A Controls snapshot with `n` analog values.
inline Controls controls_of(std::initializer_list<float> analog)
{
    Controls c;
    c.analog_count = 0;
    for (float v : analog) c.analog[c.analog_count++] = v;
    return c;
}

} // namespace daisyapps::test
