// The game's program code as the code translator reads it: the ARM9
// program decompressed, the ARM7 program, every ARM9 overlay decompressed,
// and manifest.txt, which says where each of them loads and where the
// programs start.
#pragma once

#include <filesystem>

#include "core/result.h"
#include "rom/rom.h"

namespace bd::rom {

// Writes the code into `folder` (made if it is not there). Returns how many
// files were written.
Result<std::size_t> write_program_code(const Rom& rom, const std::filesystem::path& folder);

}  // namespace bd::rom
