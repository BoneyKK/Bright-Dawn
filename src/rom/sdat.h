// SDAT: the sound archive of the Nintendo DS SDK. It holds the sequences
// (SSEQ, the music and most sound effects), the sequence archives (SSAR),
// the instrument banks (SBNK), the wave archives (SWAR) and the streams
// (STRM), with a table of what each is (INFO), where its file is (FAT) and,
// when the game was built with them, the names its makers gave each (SYMB).
//
// Only what the program needs is read: the sequences, with their names and
// their event data. Every offset is checked; a damaged archive is an error,
// not a crash.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/bytes.h"
#include "core/result.h"

namespace bd::rom {

struct SdatSequence {
  unsigned index = 0;    // The sequence's number in the archive.
  std::string name;      // Its name ("SEQ_MAP1"), or empty when the archive has none.
  ByteSpan data;         // Its events: what the sound program plays, as it sits in memory.
  std::uint16_t bank = 0;
  std::uint8_t volume = 127;
  std::uint8_t player = 0;
};

// The sequences in an archive, in their order; numbers without a sequence
// are left out. The result refers to `sdat` and must not outlive it.
Result<std::vector<SdatSequence>> sdat_sequences(ByteSpan sdat);

}  // namespace bd::rom
