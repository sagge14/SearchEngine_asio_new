#pragma once

#include <string>
#include <vector>

namespace file_extension_contract {

struct Selection {
    std::vector<std::string> indexedExtensions;
    bool includeExtensionlessFiles{false};
};

/// Validate the canonical administrator-facing selection. An empty result
/// means that the selection is valid.
std::vector<std::string> validateCanonicalSelection(
    const Selection& selection);

/// Convert the legacy extensions array to the canonical in-memory model.
/// The legacy empty string selects extensionless files, one leading dot is
/// accepted for compatibility, and case-insensitive duplicates are removed.
Selection canonicalizeLegacySelection(
    const std::vector<std::string>& legacyExtensions,
    const bool* explicitIncludeExtensionless = nullptr);

/// Match the exact final extension of a filename. The configured extensions
/// are UTF-8 strings without a leading dot. With no selected file type this
/// predicate fails closed.
bool matchesPath(
    const std::wstring& path,
    const Selection& selection);

}  // namespace file_extension_contract
