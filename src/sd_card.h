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
    // Bring up SDMMC (1-bit, MEDIUM_SLOW: the robust universal default) and mount the card's FatFs.
    // False if no card is present or the mount fails - which callers are expected to treat as
    // non-fatal, since every path that reads the card tolerates "file not found".
    bool Mount()
    {
        daisy::SdmmcHandler::Config cfg;
        cfg.Defaults();
        cfg.speed = daisy::SdmmcHandler::Speed::MEDIUM_SLOW;
        cfg.width = daisy::SdmmcHandler::BusWidth::BITS_1;
        if (_sd.Init(cfg) != daisy::SdmmcHandler::Result::OK) return false;

        _fsi.Init(daisy::FatFSInterface::Config::MEDIA_SD);   // links libDaisy I/O to the FatFs driver
        _mounted = f_mount(&_fsi.GetSDFileSystem(), _fsi.GetSDPath(), 1) == FR_OK;
        return _mounted;
    }

    bool mounted() const { return _mounted; }

private:
    daisy::SdmmcHandler   _sd;
    daisy::FatFSInterface _fsi;
    bool                  _mounted = false;
};

} // namespace daisyapps
