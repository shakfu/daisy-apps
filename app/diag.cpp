// Hardware diagnostic: one firmware that puts every subsystem this repo depends on onto the screen at
// once, so bringing up a board is a single flash rather than a bisect across fifteen engines.
//
//   make ENGINE=diag BOARD=patch          -> build-diag-patch/diag.bin
//
// It links NO engine. Everything here talks to the board driver and to FatFs directly, which is the
// point: when an engine is silent the question is usually "did the ADC read, did the card mount, is
// the codec running", and an engine cannot answer any of those.
//
// Panel:
//   encoder turn      change page
//   encoder click     the page's action (toggle the test tone, rescan the card, step through files...)
//   everything else   is what is being measured
//
// The audio callback passes input to output and measures both, so a diag build is also a passthrough:
// if you hear your input, the codec and the SAI are working.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>   // strcasecmp (POSIX, not in <cstring>)

#include "daisy_seed.h"

#include "board/board.h"
#include "sd_card.h"

using namespace daisy;

static constexpr int kBlock = 48;

// A modest SDRAM block, written and read back once at boot. The engines lean on SDRAM heavily (48 MB
// of arena, 2 MB of streaming rings), and a board with an unreliable SDRAM presents as engines that
// work until they touch a buffer - which is a miserable thing to diagnose from the far end.
static constexpr uint32_t kSdramTestBytes = 1u * 1024u * 1024u;
static uint32_t DSY_SDRAM_BSS s_sdram_test[kSdramTestBytes / sizeof(uint32_t)];

static daisyapps::Board  board;
static daisyapps::SdCard card;

// --- audio-side state (written in the ISR, read by the main loop) --------------------------------
static volatile float s_in_peak[2]  = {0.f, 0.f};
static volatile float s_out_peak[2] = {0.f, 0.f};
static volatile bool  s_tone_on     = false;
static volatile uint32_t s_blocks   = 0;      // proves the callback is actually running
static float s_phase = 0.f;

static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    const float sr   = 48000.f;
    const float step = 2.f * static_cast<float>(M_PI) * 440.f / sr;

    float ip[2] = {0.f, 0.f}, op[2] = {0.f, 0.f};
    for (size_t i = 0; i < size; i++) {
        const float l = in[0][i], r = in[1][i];
        ip[0] = std::fmax(ip[0], std::fabs(l));
        ip[1] = std::fmax(ip[1], std::fabs(r));

        float ol = l, orr = r;
        if (s_tone_on) {
            const float t = 0.25f * std::sin(s_phase);
            s_phase += step;
            if (s_phase > 2.f * static_cast<float>(M_PI)) s_phase -= 2.f * static_cast<float>(M_PI);
            ol = orr = t;
        }
        out[0][i] = ol;
        out[1][i] = orr;
        op[0] = std::fmax(op[0], std::fabs(ol));
        op[1] = std::fmax(op[1], std::fabs(orr));
    }

    // Silence any channels beyond the stereo pair (the Patch is 4-out and reuses its DMA buffers).
    for (int ch = 2; ch < daisyapps::Board::kAudioChannels; ch++)
        for (size_t i = 0; i < size; i++) out[ch][i] = 0.f;

    s_in_peak[0]  = ip[0];  s_in_peak[1]  = ip[1];
    s_out_peak[0] = op[0];  s_out_peak[1] = op[1];
    s_blocks++;
}

// --- MIDI ----------------------------------------------------------------------------------------
static uint32_t s_midi_count = 0;
static uint8_t  s_midi_last[3] = {0, 0, 0};

static const char* midi_type_name(uint8_t status)
{
    if (status >= 0xF8) {
        switch (status) {
            case 0xF8: return "clock";
            case 0xFA: return "start";
            case 0xFB: return "cont";
            case 0xFC: return "stop";
            default:   return "rt";
        }
    }
    switch (status & 0xF0) {
        case 0x80: return "noteoff";
        case 0x90: return "noteon";
        case 0xA0: return "polyat";
        case 0xB0: return "cc";
        case 0xC0: return "prog";
        case 0xD0: return "chanat";
        case 0xE0: return "bend";
        default:   return "?";
    }
}

// MIDI note number -> name, for the note pages ("60" is far less useful than "C4" when you are
// checking that a keyboard is wired to the right octave).
static void note_name(uint8_t note, char* out, int cap)
{
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::snprintf(out, cap, "%s%d", kNames[note % 12], (note / 12) - 1);
}

// --- SD scanning ---------------------------------------------------------------------------------
// The same rules scripts/make_sd_content.py --verify applies on the host, applied here to the actual
// card: a file in the wrong one of the two WAV formats is the failure mode that otherwise presents as
// "the engine just does not play anything".

struct FileCheck {
    char name[24];
    char verdict[32];
    bool ok;
};

static constexpr int kMaxFiles = 48;
static FileCheck s_files[kMaxFiles];
static int       s_file_count = 0;
static bool      s_sd_mounted = false;
static char      s_sd_note[40] = "not scanned";

// Directories whose contents must be format A (mono float32 @48k) vs format B (mono 16-bit PCM).
static const char* kDirsA[] = {"tapes", "shuttle", "softcut"};
static const char* kDirsB[] = {"radio", "pstretch", "bard"};

static uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Minimal chunk walk: enough to recover (format, channels, rate, bits) from a canonical WAV.
static bool wav_fmt(const char* path, uint16_t& fmt, uint16_t& ch, uint32_t& rate, uint16_t& bits,
                    bool& opened)
{
    // STATIC, not a local: a FIL carries a 512-byte sector buffer the SDMMC DMA writes into, and the
    // DMA cannot reach DTCMRAM, where the stack lives. A stack FIL fails every read (see sd_card.h).
    static FIL f;
    opened = false;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    opened = true;
    uint8_t hdr[12];
    UINT got = 0;
    bool ok = false;
    if (f_read(&f, hdr, sizeof(hdr), &got) == FR_OK && got == sizeof(hdr)
        && std::memcmp(hdr, "RIFF", 4) == 0 && std::memcmp(hdr + 8, "WAVE", 4) == 0) {
        for (int i = 0; i < 8; i++) {                    // a few chunks is plenty
            uint8_t ch8[8];
            if (f_read(&f, ch8, sizeof(ch8), &got) != FR_OK || got != sizeof(ch8)) break;
            const uint32_t size = rd32(ch8 + 4);
            if (std::memcmp(ch8, "fmt ", 4) == 0 && size >= 16) {
                uint8_t fb[16];
                if (f_read(&f, fb, sizeof(fb), &got) != FR_OK || got != sizeof(fb)) break;
                fmt = rd16(fb); ch = rd16(fb + 2); rate = rd32(fb + 4); bits = rd16(fb + 14);
                ok = true;
                break;
            }
            if (f_lseek(&f, f_tell(&f) + size + (size & 1u)) != FR_OK) break;
        }
    }
    f_close(&f);
    return ok;
}

static void check_file(const char* dir, const char* name, bool format_a)
{
    if (s_file_count >= kMaxFiles) return;
    FileCheck& fc = s_files[s_file_count];
    std::snprintf(fc.name, sizeof(fc.name), "%.6s/%.8s", dir, name);
    fc.ok = true;
    std::snprintf(fc.verdict, sizeof(fc.verdict), "ok");

    const char* dot = std::strrchr(name, '.');
    const bool is_wav = dot && (strcasecmp(dot, ".wav") == 0);
    const bool is_raw = dot && (strcasecmp(dot, ".raw") == 0);
    if (!is_wav && !is_raw) return;                        // not audio; nothing to check

    // The bank scanner stores 13 bytes per name and skips anything longer, so an over-long name is
    // invisible to the engine however well-formed the file is. Checked FIRST, before the path is
    // built: FatFs hands back long filenames here, and a truncated path would not open anyway.
    if (std::strlen(name) > 12) {
        fc.ok = false;
        std::snprintf(fc.verdict, sizeof(fc.verdict), "name not 8.3");
        s_file_count++;
        return;
    }

    char path[80];   // dir (bounded by the scan roots + one level) + '/' + an 8.3 name
    std::snprintf(path, sizeof(path), "%s/%.12s", dir, name);

    FILINFO fno;
    const uint32_t size = (f_stat(path, &fno) == FR_OK) ? static_cast<uint32_t>(fno.fsize) : 0;

    // Below the scanner's 32 KB floor a file is likewise dropped (the filter exists to skip macOS
    // `._*` metadata stubs).
    if (size < 32u * 1024u) {
        fc.ok = false; std::snprintf(fc.verdict, sizeof(fc.verdict), "under 32K floor");
    } else if (is_wav) {
        uint16_t fmt = 0, ch = 0, bits = 0; uint32_t rate = 0; bool opened = false;
        if (!wav_fmt(path, fmt, ch, rate, bits, opened)) {
            fc.ok = false;
            std::snprintf(fc.verdict, sizeof(fc.verdict),
                          opened ? "no fmt chunk" : "cannot open/read");
        } else if (ch != 1) {
            fc.ok = false; std::snprintf(fc.verdict, sizeof(fc.verdict), "%u ch, need mono", ch);
        } else if (format_a && (fmt != 3 || bits != 32 || rate != 48000)) {
            fc.ok = false;
            std::snprintf(fc.verdict, sizeof(fc.verdict), "need f32/48k, is %u/%u", fmt, bits);
        } else if (!format_a && (fmt != 1 || bits != 16)) {
            fc.ok = false;
            std::snprintf(fc.verdict, sizeof(fc.verdict), "need i16, is %u/%u", fmt, bits);
        }
    } else if (is_raw && (size & 1u)) {
        fc.ok = false; std::snprintf(fc.verdict, sizeof(fc.verdict), "odd byte count");
    }

    s_file_count++;
}

// Walk one directory, one level of subdirectories deep (radio/0/, bard/0/ nest their content).
static void scan_dir(const char* dir, bool format_a, int depth)
{
    DIR d;
    if (f_opendir(&d, dir) != FR_OK) return;
    FILINFO fno;
    while (f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) {
            if (depth > 0) {
                char sub[128];
                std::snprintf(sub, sizeof(sub), "%s/%s", dir, fno.fname);
                scan_dir(sub, format_a, depth - 1);
            }
        } else {
            check_file(dir, fno.fname, format_a);
        }
    }
    f_closedir(&d);
}

static void rescan_card()
{
    s_file_count = 0;
    if (!s_sd_mounted) {
        // Say WHY. "not mounted" alone sends people to the wrong problem; the FatFs result
        // distinguishes an unreadable/absent card from a card whose filesystem FatFs will not accept.
        std::snprintf(s_sd_note, sizeof(s_sd_note), "sdmmc %s  1b:%d 4b:%d 4bF:%d",
                      card.init_ok() ? "ok" : "FAIL",
                      card.attempt(0), card.attempt(1), card.attempt(2));
        return;
    }
    for (unsigned i = 0; i < sizeof(kDirsA) / sizeof(kDirsA[0]); i++) scan_dir(kDirsA[i], true, 1);
    for (unsigned i = 0; i < sizeof(kDirsB) / sizeof(kDirsB[0]); i++) scan_dir(kDirsB[i], false, 1);

    int bad = 0;
    for (int i = 0; i < s_file_count; i++) if (!s_files[i].ok) bad++;
    std::snprintf(s_sd_note, sizeof(s_sd_note), "%d file(s), %d bad", s_file_count, bad);
}

// --- pages ---------------------------------------------------------------------------------------
enum Page { P_ANALOG = 0, P_DIGITAL, P_MIDI, P_SD, P_AUDIO, P_OUT, P_SYS, P_COUNT };
static const char* kPageName[P_COUNT] = {"ANALOG", "DIGITAL", "MIDI", "SD", "AUDIO", "OUTPUTS", "SYSTEM"};

static bool     s_sdram_ok   = false;
static uint32_t s_gate_edges[2] = {0, 0};
static int      s_file_cursor   = 0;
static bool     s_cv_sweep      = false;
static bool     s_gate_out      = false;

static void bar(int x, int y, int w, int h, float v)
{
    if (v < 0.f) v = 0.f;
    if (v > 1.f) v = 1.f;
    board.ScreenRect(x, y, w, h, false);
    const int fill = static_cast<int>(v * static_cast<float>(w - 2) + 0.5f);
    if (fill > 0) board.ScreenRect(x + 1, y + 1, fill, h - 2, true);
}

static void draw(int page, const daisyapps::Controls& c, uint32_t now_ms)
{
    char line[64];
    board.ScreenClear();

    std::snprintf(line, sizeof(line), "%d/%d %s", page + 1, P_COUNT, kPageName[page]);
    board.ScreenText(0, 0, line, true);

    switch (page) {
        case P_ANALOG: {
            // Every analog input the board reports, as a bar plus its raw normalized value. On the
            // Patch these are knob+CV summed; on patch.init() 0-3 are pots and 4-7 are CV jacks.
            const int n = c.analog_count;
            for (int i = 0; i < n && i < 8; i++) {
                const int y = 12 + i * 6;
                std::snprintf(line, sizeof(line), "%d", i);
                board.ScreenText(0, y, line, true);
                bar(10, y, 70, 6, c.analog[i]);
                std::snprintf(line, sizeof(line), "%4d", static_cast<int>(c.analog[i] * 1000.f));
                board.ScreenText(84, y, line, true);
            }
            break;
        }

        case P_DIGITAL: {
            std::snprintf(line, sizeof(line), "enc %+ld %s", static_cast<long>(c.enc_inc),
                          c.enc_press ? "DOWN" : "up");
            board.ScreenText(0, 14, line, true);
            std::snprintf(line, sizeof(line), "btn %d %d (%d)",
                          c.button_count > 0 ? c.button[0] : 0,
                          c.button_count > 1 ? c.button[1] : 0, c.button_count);
            board.ScreenText(0, 24, line, true);
            std::snprintf(line, sizeof(line), "gate %d %d (%d)",
                          c.gate_count > 0 ? c.gate[0] : 0,
                          c.gate_count > 1 ? c.gate[1] : 0, c.gate_count);
            board.ScreenText(0, 34, line, true);
            std::snprintf(line, sizeof(line), "edges %lu / %lu",
                          static_cast<unsigned long>(s_gate_edges[0]),
                          static_cast<unsigned long>(s_gate_edges[1]));
            board.ScreenText(0, 44, line, true);
            board.ScreenText(0, 54, "click: reset edges", true);
            break;
        }

        case P_MIDI: {
            std::snprintf(line, sizeof(line), "msgs %lu", static_cast<unsigned long>(s_midi_count));
            board.ScreenText(0, 14, line, true);
            if (s_midi_count) {
                const uint8_t st = s_midi_last[0];
                std::snprintf(line, sizeof(line), "%s ch%d", midi_type_name(st),
                              (st < 0xF0) ? (st & 0x0F) + 1 : 0);
                board.ScreenText(0, 26, line, true);
                std::snprintf(line, sizeof(line), "%02X %3d %3d", st, s_midi_last[1], s_midi_last[2]);
                board.ScreenText(0, 36, line, true);
                if ((st & 0xF0) == 0x90 || (st & 0xF0) == 0x80) {
                    char nn[8];
                    note_name(s_midi_last[1], nn, sizeof(nn));
                    std::snprintf(line, sizeof(line), "note %s vel %d", nn, s_midi_last[2]);
                    board.ScreenText(0, 46, line, true);
                }
            } else {
                board.ScreenText(0, 26, "play something...", true);
            }
            break;
        }

        case P_SD: {
            if (s_sd_mounted) {
                std::snprintf(line, sizeof(line), "mounted (%d-bit)", card.width_bits());
                board.ScreenText(0, 14, line, true);
            } else {
                board.ScreenText(0, 14, "NOT MOUNTED", true);
            }
            board.ScreenText(0, 24, s_sd_note, true);
            if (!s_sd_mounted) {
                std::snprintf(line, sizeof(line), "%.20s", daisyapps::SdCard::fres_text(card.fres()));
                board.ScreenText(0, 32, line, true);
            }
            if (s_file_count) {
                if (s_file_cursor >= s_file_count) s_file_cursor = 0;
                const FileCheck& fc = s_files[s_file_cursor];
                std::snprintf(line, sizeof(line), "%d/%d %.20s", s_file_cursor + 1, s_file_count, fc.name);
                board.ScreenText(0, 38, line, true);
                std::snprintf(line, sizeof(line), "%s %.28s", fc.ok ? "OK " : "BAD", fc.verdict);
                board.ScreenText(0, 48, line, true);
            }
            board.ScreenText(0, 56, s_sd_mounted ? "click: next file" : "click: retry mount", true);
            break;
        }

        case P_AUDIO: {
            std::snprintf(line, sizeof(line), "blocks %lu", static_cast<unsigned long>(s_blocks));
            board.ScreenText(0, 14, line, true);
            board.ScreenText(0, 24, "in", true);
            bar(20, 24, 100, 6, s_in_peak[0]);
            bar(20, 32, 100, 6, s_in_peak[1]);
            board.ScreenText(0, 42, "out", true);
            bar(20, 42, 100, 6, s_out_peak[0]);
            std::snprintf(line, sizeof(line), "click: tone %s", s_tone_on ? "OFF" : "ON");
            board.ScreenText(0, 54, line, true);
            break;
        }

        case P_OUT: {
            std::snprintf(line, sizeof(line), "cv out %s", s_cv_sweep ? "SWEEPING" : "held 0V");
            board.ScreenText(0, 14, line, true);
            std::snprintf(line, sizeof(line), "gate out %s", s_gate_out ? "HIGH" : "low");
            board.ScreenText(0, 24, line, true);
            std::snprintf(line, sizeof(line), "cv outs on board: %d", daisyapps::Board::kCvOutCount);
            board.ScreenText(0, 36, line, true);
            board.ScreenText(0, 48, "meter the jacks", true);
            board.ScreenText(0, 56, "click: toggle sweep", true);
            break;
        }

        case P_SYS: {
            std::snprintf(line, sizeof(line), "sr %d blk %d",
                          static_cast<int>(board.SampleRate()), kBlock);
            board.ScreenText(0, 14, line, true);
            std::snprintf(line, sizeof(line), "sdram %s", s_sdram_ok ? "ok (1MB rw)" : "FAILED");
            board.ScreenText(0, 24, line, true);
            std::snprintf(line, sizeof(line), "analog %d gates %d",
                          daisyapps::Board::kAnalogCount, daisyapps::Board::kGateCount);
            board.ScreenText(0, 34, line, true);
            std::snprintf(line, sizeof(line), "screen %dx%d",
                          daisyapps::Board::kScreenWidth, daisyapps::Board::kScreenHeight);
            board.ScreenText(0, 44, line, true);
            std::snprintf(line, sizeof(line), "up %lus", static_cast<unsigned long>(now_ms / 1000));
            board.ScreenText(0, 54, line, true);
            break;
        }

        default: break;
    }

    board.ScreenUpdate();
}

int main(void)
{
    board.Init(kBlock);

    // SDRAM write/read before anything else leans on it.
    const uint32_t n = kSdramTestBytes / sizeof(uint32_t);
    for (uint32_t i = 0; i < n; i++) s_sdram_test[i] = i * 2654435761u;   // Knuth's multiplicative hash
    s_sdram_ok = true;
    for (uint32_t i = 0; i < n; i++) {
        if (s_sdram_test[i] != i * 2654435761u) { s_sdram_ok = false; break; }
    }

    s_sd_mounted = card.Mount();
    rescan_card();

    board.StartAudio(AudioCallback);
    board.StartMidi();

    daisyapps::Controls controls;
    bool     prev_gate[daisyapps::Controls::kMaxGates] = {false, false};
    bool     prev_press = false;
    int      page       = 0;
    uint32_t last_draw  = 0;

    while (1) {
        board.Poll(controls);
        const uint32_t now = System::GetNow();

        board.PollMidi([](uint8_t st, uint8_t d1, uint8_t d2) {
            s_midi_count++;
            s_midi_last[0] = st; s_midi_last[1] = d1; s_midi_last[2] = d2;
        });

        for (int g = 0; g < controls.gate_count && g < daisyapps::Controls::kMaxGates; g++) {
            if (controls.gate[g] && !prev_gate[g]) s_gate_edges[g]++;
            prev_gate[g] = controls.gate[g];
        }

        // Encoder: turn changes page, a press-release without a turn is the page's action.
        if (controls.enc_inc != 0) {
            page += controls.enc_inc;
            while (page < 0)        page += P_COUNT;
            while (page >= P_COUNT) page -= P_COUNT;
        }
        if (prev_press && !controls.enc_press) {
            switch (page) {
                case P_DIGITAL: s_gate_edges[0] = s_gate_edges[1] = 0; break;
                case P_SD:
                    // Nothing mounted -> retry the mount, so a card can be inserted and tested without
                    // a power cycle. Mounted -> step through the scanned files.
                    if (!s_sd_mounted) { s_sd_mounted = card.Mount(); rescan_card(); }
                    else if (s_file_count) s_file_cursor = (s_file_cursor + 1) % s_file_count;
                    break;
                case P_AUDIO:   s_tone_on = !s_tone_on; break;
                case P_OUT:     s_cv_sweep = !s_cv_sweep; break;
                case P_SYS:     rescan_card(); break;
                default: break;
            }
        }
        prev_press = controls.enc_press;

        // Outputs: a slow triangle on both CV outs and a 1 Hz gate, so a multimeter or a scope sees
        // something unambiguous. Held at mid-scale (0 V on a bipolar output) when the sweep is off.
        if (s_cv_sweep) {
            const float ph = static_cast<float>(now % 2000) / 2000.f;
            const float tri = ph < 0.5f ? (ph * 2.f) : (2.f - ph * 2.f);
            board.SetCvOut(0, tri);
            board.SetCvOut(1, 1.f - tri);
        } else {
            board.SetCvOut(0, 0.5f);
            board.SetCvOut(1, 0.5f);
        }
        s_gate_out = ((now / 500) & 1u) != 0;
        board.SetGateOut(s_gate_out);

        // The onboard LED is the fallback signal of life on a board with no screen (Pod, patch.init()),
        // where everything above still runs but nothing is visible.
        board.SetUserLed(((now / 250) & 1u) != 0);

        if (now - last_draw >= 50) {          // 20 Hz; the OLED blit is not free
            last_draw = now;
            draw(page, controls, now);
        }
    }
}
