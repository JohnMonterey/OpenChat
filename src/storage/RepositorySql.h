#pragma once

#include "repositories/RepositoryError.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <sqlite3.h>

#include <limits>

namespace OpenChat::RepositorySql {

inline RepositoryError error(RepositoryErrorCode code, QString diagnostic)
{
    return RepositoryError{code, std::move(diagnostic)};
}

class Statement final
{
public:
    Statement(sqlite3 *database, const char *sql)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &m_statement, nullptr) != SQLITE_OK)
            m_statement = nullptr;
    }

    ~Statement()
    {
        if (m_statement)
            sqlite3_finalize(m_statement);
    }

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    [[nodiscard]] bool isValid() const noexcept { return m_statement != nullptr; }
    [[nodiscard]] sqlite3_stmt *get() const noexcept { return m_statement; }

    bool bindBlob(int index, QByteArrayView value)
    {
        if (value.size() > std::numeric_limits<int>::max())
            return false;
        static constexpr char emptyBlob = '\0';
        const char *data = value.isEmpty() ? &emptyBlob : value.data();
        return sqlite3_bind_blob(m_statement, index, data, static_cast<int>(value.size()),
                                 SQLITE_TRANSIENT)
               == SQLITE_OK;
    }

    bool bindText(int index, const QString &value)
    {
        const QByteArray utf8 = value.toUtf8();
        if (utf8.size() > std::numeric_limits<int>::max())
            return false;
        return sqlite3_bind_text(m_statement, index, utf8.constData(), utf8.size(), SQLITE_TRANSIENT)
               == SQLITE_OK;
    }

    bool bindInt(int index, int value)
    {
        return sqlite3_bind_int(m_statement, index, value) == SQLITE_OK;
    }

    bool bindInt64(int index, qint64 value)
    {
        return sqlite3_bind_int64(m_statement, index, value) == SQLITE_OK;
    }

    bool bindNull(int index) { return sqlite3_bind_null(m_statement, index) == SQLITE_OK; }

private:
    sqlite3_stmt *m_statement = nullptr;
};

inline bool execute(sqlite3 *database, const char *sql)
{
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

inline QByteArray blob(sqlite3_stmt *statement, int column)
{
    const auto *data = static_cast<const char *>(sqlite3_column_blob(statement, column));
    const int size = sqlite3_column_bytes(statement, column);
    return data && size > 0 ? QByteArray(data, size) : QByteArray();
}

inline QString text(sqlite3_stmt *statement, int column)
{
    const auto *data = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
    return data ? QString::fromUtf8(data, sqlite3_column_bytes(statement, column)) : QString();
}

} // namespace OpenChat::RepositorySql
