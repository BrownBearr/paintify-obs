#pragma once
#include <string>
#include <vector>

// Native file and folder dialogs.
//
// Win32's IFileOpenDialog / IFileSaveDialog rather than the legacy
// GetOpenFileName: modern appearance, no MAX_PATH truncation, and folder
// selection is a flag rather than a separate shell-browse API. ole32 and
// shell32 are already in MSVC's default link set, so this costs no new
// dependency.
//
// Every entry point returns empty on cancel or on a non-Windows build, so the
// caller only ever has to check for empty.
namespace filedialog {

// Call once before any dialog. Safe to call repeatedly.
void init();

// `filter` is a display name and a semicolon-separated pattern list, e.g.
// {"Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"}. An empty list means all files.
struct Filter {
    const char* name;
    const char* patterns;
};

std::string openFile(const char* title, const std::vector<Filter>& filters);
std::vector<std::string> openFiles(const char* title, const std::vector<Filter>& filters);

// Directory chooser (FOS_PICKFOLDERS).
std::string pickFolder(const char* title);

// Save-as. `defaultName` seeds the filename box; `defaultExt` is appended when
// the user does not type one.
std::string saveFile(const char* title, const std::vector<Filter>& filters,
                     const char* defaultName, const char* defaultExt);

} // namespace filedialog
