#pragma once
#include <QLoggingCategory>

namespace OpenChat {
Q_DECLARE_LOGGING_CATEGORY(relayLog)
Q_DECLARE_LOGGING_CATEGORY(contactsLog)
Q_DECLARE_LOGGING_CATEGORY(mlsLog)
// Install after the application/organization names are set.
void installFileLogging();
} // namespace OpenChat
