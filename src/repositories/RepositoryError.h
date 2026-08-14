#pragma once

#include <QString>

namespace OpenChat {

enum class RepositoryErrorCode {
    Locked,
    Unavailable,
    InvalidInput,
    NotFound,
    Conflict,
    IntegrityFailure,
    DiskFull,
    Internal,
};

struct RepositoryError final {
    RepositoryErrorCode code = RepositoryErrorCode::Internal;
    QString diagnosticCode;
};

} // namespace OpenChat
