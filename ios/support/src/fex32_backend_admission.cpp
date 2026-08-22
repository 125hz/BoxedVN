/*
 * BoxedVN - explicit admission boundary for the optional FEX32 backend.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#include "boxedvn/fex32_backend_admission.h"

namespace boxedvn {

Fex32BackendSelectionResult selectFex32Backend(
    const Fex32BackendDescriptor& descriptor, bool requested) {
    if (!requested) {
        return {Fex32BackendSelectionStatus::NotRequested,
                "The optional FEX32 backend was not requested."};
    }

    switch (validateFex32BackendDescriptor(descriptor)) {
        case Fex32BackendStatus::Invalid:
            return {
                Fex32BackendSelectionStatus::InvalidDescriptor,
                "The FEX32 backend descriptor is invalid: its version, ABI, "
                "address mode, or direct mapping contract was rejected."};
        case Fex32BackendStatus::ValidUnavailable:
            return {
                Fex32BackendSelectionStatus::Unavailable,
                "The FEX32 backend descriptor is valid, but the translator "
                "is unavailable in this build."};
        case Fex32BackendStatus::ValidAvailable:
            return {Fex32BackendSelectionStatus::Selected,
                    "The FEX32 backend was explicitly admitted."};
    }

    return {Fex32BackendSelectionStatus::InvalidDescriptor,
            "The FEX32 backend descriptor returned an unknown validation "
            "status."};
}

}  // namespace boxedvn
