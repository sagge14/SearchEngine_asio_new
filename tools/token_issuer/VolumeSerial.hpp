#pragma once

#include <string>
#include <vector>

namespace token_issuer {

struct RemovableVolume {
    std::string drive_letter; // "E:"
    std::string serial;       // UPPERCASE hardware/PNP serial
};

// Hardware/PNP serial via WMI (same semantics as client UsbHardwareSerial).
std::string NormalizeFlashSerial(std::string value);
std::string NormalizeDriveLetter(std::string drive);
std::string GetSerialForDriveLetter(const std::string& drive_letter);

// Removable volumes that have a usable hardware serial (not empty / not UNKNOWN).
std::vector<RemovableVolume> ListEligibleRemovableVolumes();

} // namespace token_issuer
