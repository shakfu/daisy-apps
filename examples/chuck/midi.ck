// midi.ck - a polyphonic MIDI-playable example for the ChucK Pod harness (not a numbered bank slot).
//
// MIDI device UGens are compiled out of this bare-metal build, so the host owns the Pod's MIDI input and
// hands notes to the VM through globals. Per audio block it sets `notesA` (the note numbers of the
// NoteOns that arrived), `noteCountA` (how many), then broadcasts `noteOnA` once. MIDI channel 1 -> the
// deck-A globals, channel 2 -> the `B` versions. NoteOn-only: each note is a short, self-terminating
// voice.
//
// To try it, copy this over any numbered slot on the card, e.g.:
//     cp examples/chuck/midi.ck <card>/chuck/3.ck
// then select that slot and play - chords work (each NoteOn sporks its own voice). MIX sets the level.

global Event noteOnA;     // host broadcasts once per block that carries NoteOns (deck A)
global int   notesA[];    // the note numbers delivered this block (host-set, length == noteCountA)
global int   noteCountA;  // how many of notesA are valid
global float mixA;        // MIX knob -> level

// One self-contained, finite voice per note: triangle osc -> ADSR -> dac, sporked per NoteOn so notes
// overlap polyphonically. The UGens are reclaimed when the shred ends. Keep voices cheap - many
// concurrent shreds add up inside one audio block.
fun void voice(int note)
{
    Std.mtof(note) => float hz;
    TriOsc s => ADSR e => dac;
    hz => s.freq;
    Math.max(0.02, mixA) * 0.3 => s.gain;          // MIX -> level, with a small floor
    e.set(2::ms, 90::ms, 0.0, 60::ms);             // pluck-ish: fits within the note, self-terminates
    e.keyOn();
    140::ms => now;
    e.keyOff();
    70::ms => now;                                  // let the release finish before the shred exits
}

// On each broadcast, spork a voice for every note delivered this block (so a full chord sounds).
while (true)
{
    noteOnA => now;
    for (0 => int i; i < noteCountA; i++) spork ~ voice(notesA[i]);
}
