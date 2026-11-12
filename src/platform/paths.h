// Per-user storage locations. Extracted and cached game data lives here, never
// next to the executable and never inside the source tree.
#pragma once

#include <filesystem>

#include "core/result.h"

namespace bd::platform {

// Directory for user data owned by Bright Dawn:
//   Windows: %LOCALAPPDATA%\BrightDawn
//   macOS:   ~/Library/Application Support/BrightDawn
//   other:   $XDG_DATA_HOME/bright-dawn, or ~/.local/share/bright-dawn
// The directory is not created by this call.
Result<std::filesystem::path> user_data_dir();

}  // namespace bd::platform
