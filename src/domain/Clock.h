#pragma once

#include <QtGlobal>

namespace OpenChat {

class Clock
{
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual qint64 nowMsUtc() const = 0;
};

} // namespace OpenChat
