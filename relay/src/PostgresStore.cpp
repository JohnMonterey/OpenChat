#include "PostgresStore.h"

#include <QDateTime>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace OpenChat::Relay {

namespace {

// Splits a plain-DDL migration file into individual statements. Line comments
// (-- to end of line) are stripped first so that a semicolon inside a comment
// does not split a statement; the migrations contain no string literals holding
// '--', so this is safe. Dollar-quoted bodies are not used.
QStringList splitStatements(const QString &sql)
{
    QString stripped;
    stripped.reserve(sql.size());
    for (const QString &line : sql.split(QLatin1Char('\n'))) {
        const qsizetype comment = line.indexOf(QLatin1String("--"));
        stripped += (comment >= 0 ? line.left(comment) : line);
        stripped += QLatin1Char('\n');
    }

    QStringList statements;
    for (const QString &raw : stripped.split(QLatin1Char(';'))) {
        const QString trimmed = raw.trimmed();
        if (!trimmed.isEmpty())
            statements.append(trimmed);
    }
    return statements;
}

} // namespace

PostgresStore::PostgresStore(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

PostgresStore::~PostgresStore()
{
    if (m_database.isOpen())
        m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

std::unique_ptr<PostgresStore> PostgresStore::open(const Config &config,
                                                   const QString &connectionName, QString *error)
{
    std::unique_ptr<PostgresStore> store(new PostgresStore(connectionName));
    store->m_database = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), connectionName);
    store->m_database.setHostName(config.host);
    store->m_database.setPort(config.port);
    store->m_database.setUserName(config.user);
    if (!config.password.isEmpty())
        store->m_database.setPassword(config.password);
    store->m_database.setDatabaseName(config.database);
    if (!store->m_database.open()) {
        if (error)
            *error = store->m_database.lastError().text();
        store->m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
        return nullptr;
    }
    return store;
}

qint64 PostgresStore::nowMs() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

bool PostgresStore::applyMigrations(const QStringList &resourcePaths, QString *error)
{
    // Ensure the migrations table exists before consulting it.
    {
        QSqlQuery create(m_database);
        if (!create.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, "
                "applied_at_ms BIGINT NOT NULL)"))) {
            if (error)
                *error = create.lastError().text();
            return false;
        }
    }

    for (int index = 0; index < resourcePaths.size(); ++index) {
        const int version = index + 1;
        QSqlQuery check(m_database);
        check.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version = ?"));
        check.addBindValue(version);
        if (!check.exec()) {
            if (error)
                *error = check.lastError().text();
            return false;
        }
        if (check.next())
            continue; // already applied

        if (!applyOneMigration(version, resourcePaths.at(index), error))
            return false;
    }
    return true;
}

bool PostgresStore::applyOneMigration(int version, const QString &resourcePath, QString *error)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot open migration %1").arg(resourcePath);
        return false;
    }
    const QString sql = QString::fromUtf8(file.readAll());
    file.close();

    const QStringList statements = splitStatements(sql);

    if (!m_database.transaction()) {
        if (error)
            *error = m_database.lastError().text();
        return false;
    }
    for (const QString &statement : statements) {
        // schema_migrations is created up front; skip a redundant CREATE in 001.
        if (statement.contains(QLatin1String("CREATE TABLE schema_migrations")))
            continue;
        QSqlQuery query(m_database);
        if (!query.exec(statement)) {
            if (error)
                *error = query.lastError().text() + QStringLiteral(" [in ") + resourcePath
                    + QStringLiteral("]");
            m_database.rollback();
            return false;
        }
    }
    QSqlQuery record(m_database);
    record.prepare(QStringLiteral(
        "INSERT INTO schema_migrations (version, applied_at_ms) VALUES (?, ?)"));
    record.addBindValue(version);
    record.addBindValue(nowMs());
    if (!record.exec()) {
        if (error)
            *error = record.lastError().text();
        m_database.rollback();
        return false;
    }
    if (!m_database.commit()) {
        if (error)
            *error = m_database.lastError().text();
        m_database.rollback();
        return false;
    }
    return true;
}

} // namespace OpenChat::Relay
