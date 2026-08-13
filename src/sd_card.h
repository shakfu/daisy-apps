#pragma once

// SD card bring-up: SDMMC + a mounted FatFs volume. Split out of sd_stream_deck.h so that the two
// IStreamDeck implementations in this repo - the lightweight `SdStreamDeck` (text/patch banks, used by
// the csound and chuck harnesses) and the full `StreamDeck` (hw/stream_deck.h, real audio streaming) -
// share one mount instead of each carrying a copy.
//
// The handler and interface must outlive every file operation, so hold this object for the whole
// session (a static/global in the harness). Call Mount() once at startup from the main thread; the card
// must be inserted at power-on, as nothing here hot-remounts.

#include "daisy_seed.h"   // SdmmcHandler + FatFSInterface + FatFs f_* , via libDaisy

namespace daisyapps {

class SdCard {
public:
    // Bring up SDMMC and mount the card's FatFs. False if no card is present or the mount fails -
    // which callers treat as non-fatal, since every path that reads the card tolerates "file not
    // found". The failure detail is kept (see init_ok / fres / width_bits) because "did not mount" on
    // its own is a miserable thing to debug from the far end: the diagnostic prints it.
    //
    // Several bus width / speed combinations are tried in order, most conservative first. A card that
    // will not talk at 4-bit FAST often works fine at 1-bit MEDIUM_SLOW, and the cost of trying is a
    // few milliseconds at boot, once.
    struct Attempt { daisy::SdmmcHandler::BusWidth width; daisy::SdmmcHandler::Speed speed; int bits; };
    static constexpr int kAttemptCount = 3;

    bool Mount()
    {
        static const Attempt kAttempts[kAttemptCount] = {
            { daisy::SdmmcHandler::BusWidth::BITS_1, daisy::SdmmcHandler::Speed::MEDIUM_SLOW, 1 },
            { daisy::SdmmcHandler::BusWidth::BITS_4, daisy::SdmmcHandler::Speed::MEDIUM_SLOW, 4 },
            { daisy::SdmmcHandler::BusWidth::BITS_4, daisy::SdmmcHandler::Speed::FAST,        4 },
        };

        _mounted = false;
        for (int i = 0; i < kAttemptCount; i++) {
            const Attempt& a = kAttempts[i];
            daisy::SdmmcHandler::Config cfg;
            cfg.Defaults();
            cfg.speed = a.speed;
            cfg.width = a.width;
            _width    = a.bits;

            _init_ok = _sd.Init(cfg) == daisy::SdmmcHandler::Result::OK;
            if (!_init_ok) { _attempt[i] = -1; continue; }        // -1: the peripheral itself refused

            _fsi.Init(daisy::FatFSInterface::Config::MEDIA_SD);  // links libDaisy I/O to the FatFs driver
            _fres       = f_mount(&_fsi.GetSDFileSystem(), _fsi.GetSDPath(), 1);
            _attempt[i] = static_cast<int>(_fres);
            if (_fres == FR_OK) { _mounted = true; return true; }
        }
        return false;
    }

    // Per-attempt result, for the diagnostic: keeping only the last one hides the interesting case
    // where a card talks at one bus width but not another.
    int attempt(int i) const { return (i >= 0 && i < kAttemptCount) ? _attempt[i] : -2; }

    bool mounted() const    { return _mounted; }
    bool init_ok() const    { return _init_ok; }   // did the SDMMC peripheral come up at all
    int  fres() const       { return static_cast<int>(_fres); }  // last f_mount FRESULT
    int  width_bits() const { return _width; }     // bus width of the successful (or last) attempt

    // The FatFs result codes worth telling a human apart. FR_NO_FILESYSTEM is the one that catches
    // people out: libDaisy builds FatFs with _FS_EXFAT 0, so an exFAT card - what most tools choose
    // by default above 32 GB - is rejected here and must be reformatted FAT32.
    static const char* fres_text(int fr)
    {
        switch (fr) {
            case FR_OK:             return "ok";
            case FR_DISK_ERR:       return "disk err";
            case FR_INT_ERR:        return "internal err";
            case FR_NOT_READY:      return "not ready/no card";
            case FR_NO_FILESYSTEM:  return "no FAT32 (exFAT?)";
            case FR_DENIED:         return "denied";
            case FR_WRITE_PROTECTED:return "write protected";
            case FR_INVALID_DRIVE:  return "bad drive";
            case FR_NOT_ENABLED:    return "not enabled";
            default:                return "error";
        }
    }

private:
    daisy::SdmmcHandler   _sd;
    daisy::FatFSInterface _fsi;
    bool                  _mounted = false;
    bool                  _init_ok = false;
    FRESULT               _fres    = FR_NOT_READY;
    int                   _width   = 0;
    int                   _attempt[kAttemptCount] = { -3, -3, -3 };   // -3: never tried
};

} // namespace daisyapps
