/*
 * BoxedVN - explicit admission boundary for the optional FEX32 backend.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_FEX32_BACKEND_ADMISSION_H
#define BOXEDVN_FEX32_BACKEND_ADMISSION_H

#include "boxedvn/fex32_backend_descriptor.h"

#include <string>

namespace boxedvn {

enum class Fex32BackendSelectionStatus : std::uint8_t {
    NotRequested,
    InvalidDescriptor,
    Unavailable,
    Selected,
};

struct Fex32BackendSelectionResult final {
    Fex32BackendSelectionStatus status =
        Fex32BackendSelectionStatus::NotRequested;
    std::string reason;

    bool selected() const noexcept {
        return status == Fex32BackendSelectionStatus::Selected;
    }
};

// The caller must opt in explicitly. A descriptor is selected only when its
// full address/ABI contract is valid and it declares the backend available.
// No global or compile-time default can turn this optional boundary on.
Fex32BackendSelectionResult selectFex32Backend(
    const Fex32BackendDescriptor& descriptor, bool requested);

}  // namespace boxedvn

#endif  // BOXEDVN_FEX32_BACKEND_ADMISSION_H
