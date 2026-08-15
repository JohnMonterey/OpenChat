#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace OpenChat::Relay {

// Thin, parameterized wrapper over a single PostgreSQL connection. The relay is
// single-threaded (QHttpServer runs handlers on the main event loop), so one
// connection per store is sufficient; QSqlDatabase must never be shared across
// threads, so any future worker thread must own its own PostgresStore.
//
// Every query in the relay goes through prepared statements with bound values —
// never string interpolation. Multi-table state changes run inside a
// serializable transaction with bounded retry on serialization failures.
class PostgresStore final
{
public:
    struct Config final {
        QString host = QStringLiteral("127.0.0.1");
        int port = 5432;
        QString user;
        QString password;
        QString database;
    };

    // Opens a named connection. connectionName must be unique per process.
    [[nodiscard]] static std::unique_ptr<PostgresStore> open(const Config &config,
                                                             const QString &connectionName,
                                                             QString *error = nullptr);
    ~PostgresStore();

    PostgresStore(const PostgresStore &) = delete;
    PostgresStore &operator=(const PostgresStore &) = delete;

    [[nodiscard]] QSqlDatabase &database() { return m_database; }

    // Applies the given migration resource files (in order), each split into
    // individual statements. Idempotent: already-applied versions are skipped
    // using the schema_migrations table.
    [[nodiscard]] bool applyMigrations(const QStringList &resourcePaths, QString *error = nullptr);

    // Injectable clock (UTC ms). Defaults to the system clock. Tests override it
    // to exercise challenge/token expiry deterministically.
    void setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }
    [[nodiscard]] qint64 nowMs() const;

private:
    explicit PostgresStore(QString connectionName);

    [[nodiscard]] bool applyOneMigration(int version, const QString &resourcePath, QString *error);

    QString m_connectionName;
    QSqlDatabase m_database;
    std::function<qint64()> m_clock;
};

} // namespace OpenChat::Relay
