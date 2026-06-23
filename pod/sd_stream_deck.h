#pragma once

#include "daisy_seed.h"            // FatFs FIL / f_* + SdmmcHandler + FatFSInterface, via libDaisy
#include "engine/istreamdeck.h"

namespace daisyapps {

// Minimal SD-card IStreamDeck for the Pod harness. The Csound and ChucK patch banks call exactly two
// methods of the (large) IStreamDeck contract - exists() (an f_stat probe) and read_text() (f_open +
// f_read of a small text file) - so this implements only those over a FatFs-mounted card and stubs the
// streaming/recording half (tape/radio), which this synthesis sandbox never uses.
//
// Main-loop use only: FatFs is not ISR-safe. That holds here because the engine drives the patch paths
// (rescan_bank / orchestra read) from prepare(), which the harness calls in the main loop - never from
// the audio callback.
//
// Card layout the Csound engine expects (see src/engine/csound/csound_patch.h): a `csound/` folder at
// the card root holding numbered slots `csound/0.csd` .. `csound/7.csd`, each a full CSD document. The
// engine always boots its built-in orchestra; once a card is mounted it auto-loads the first present
// slot (~1 s after boot, via prepare()'s boot-retry) and the encoder selector scrolls the
// [built-in, present slots...] list. (ChucK is analogous: `chuck/0.ck` .. `chuck/7.ck`.)
class SdStreamDeck : public IStreamDeck {
public:
    // Bring up SDMMC (1-bit, MEDIUM_SLOW: the robust universal default) and mount the card's FatFs.
    // Returns false if no card is present or the mount fails; the engine then simply runs its built-in
    // orchestra, because every patch path tolerates a "file not found". The handler and interface are
    // members so the mounted filesystem stays alive for the whole session. Call once, at startup, from
    // the main thread - the card must be inserted at power-on (this harness does not hot-remount).
    bool Init()
    {
        daisy::SdmmcHandler::Config cfg;
        cfg.Defaults();
        cfg.speed = daisy::SdmmcHandler::Speed::MEDIUM_SLOW;
        cfg.width = daisy::SdmmcHandler::BusWidth::BITS_1;
        if (_sd.Init(cfg) != daisy::SdmmcHandler::Result::OK) return false;

        _fsi.Init(daisy::FatFSInterface::Config::MEDIA_SD);   // links libDaisy I/O to the FatFs driver
        return f_mount(&_fsi.GetSDFileSystem(), _fsi.GetSDPath(), 1) == FR_OK;
    }

    // --- the two methods the patch banks actually use --------------------------------------------
    // f_stat needs no FIL handle, so it is safe to call at any time.
    bool exists(const char* path) const override
    {
        FILINFO fno;
        return f_stat(path, &fno) == FR_OK;
    }

    // Read up to max-1 bytes of a small text file into buf and NUL-terminate; returns bytes read (0 if
    // missing/empty). Used for the .csd / .ck orchestra text; never for audio data.
    int read_text(const char* path, char* buf, int max) const override
    {
        if (!buf || max <= 0) return 0;
        FIL f;
        if (f_open(&f, path, FA_READ) != FR_OK) { buf[0] = '\0'; return 0; }
        UINT got = 0;
        f_read(&f, buf, static_cast<UINT>(max - 1), &got);
        f_close(&f);
        buf[got] = '\0';
        return static_cast<int>(got);
    }

    // --- unused streaming/recording half: stubbed (the harness never plays/records SD audio) -------
    uint32_t play_consume(DeckRef::Ref, uint8_t*, uint32_t)    override { return 0; }
    uint32_t record_produce(DeckRef::Ref, const uint8_t*, uint32_t) override { return 0; }
    bool     is_playing(DeckRef::Ref)   const override { return false; }
    bool     is_recording(DeckRef::Ref) const override { return false; }
    bool     start_play(DeckRef::Ref, const char*)   override { return false; }
    bool     start_record(DeckRef::Ref, const char*) override { return false; }
    void     stop(DeckRef::Ref)                       override {}
    void     set_loop(DeckRef::Ref, bool)             override {}
    uint32_t loop_frames(DeckRef::Ref) const          override { return 0; }

private:
    daisy::SdmmcHandler   _sd;
    daisy::FatFSInterface _fsi;
};

} // namespace daisyapps
