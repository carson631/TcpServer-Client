#include "database.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QVariant>

namespace {

// 连接名：一个程序内唯一，方便用 QSqlDatabase::database() 取回
const char *const kConn = "tcp_chat_main";

QString queryError(const QSqlQuery &q)
{
    const QSqlError e = q.lastError();
    QString text = e.text().trimmed();
    if (text.isEmpty())
        text = e.driverText().trimmed();
    if (text.isEmpty())
        text = QStringLiteral("未知数据库错误");
    return text;
}

// 建/重连连接。dbName 为空表示只连服务器、不选库（用于 CREATE DATABASE）
bool connectOnce(const QString &dbName, const QString &host, int port,
                 const QString &user, const QString &password, QString *error)
{
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::contains(QLatin1String(kConn))
                              ? QSqlDatabase::database(QLatin1String(kConn), false)
                              : QSqlDatabase::addDatabase(QStringLiteral("QMYSQL"),
                                                          QLatin1String(kConn));
        db.setHostName(host);
        db.setPort(port);
        db.setDatabaseName(dbName);
        db.setUserName(user);
        db.setPassword(password);
        // 只设连接超时；MYSQL_OPT_RECONNECT 在 MySQL 8.0.34+ 已废弃，会往 stderr 打警告
        db.setConnectOptions(QStringLiteral("MYSQL_OPT_CONNECT_TIMEOUT=5"));
        ok = db.open();
        if (!ok) {
            if (error)
                *error = db.lastError().text().trimmed();
            db.close();
        }
    }
    return ok;
}

} // namespace

Database &Database::instance()
{
    static Database inst;
    return inst;
}

bool Database::open(const QString &host, int port, const QString &dbName,
                    const QString &user, const QString &password, QString *error)
{
    close();
    m_error.clear();

    auto fail = [this, error](const QString &msg) {
        m_error = msg;
        if (error)
            *error = msg;
        close();
        return false;
    };

    const QString safeDb = QString(dbName).replace(QLatin1Char('`'), QLatin1String("``"));

    QString err;
    if (!connectOnce(dbName, host, port, user, password, &err)) {
        // 1049 Unknown database —— 库还没建，先连服务器把它建出来再切过去
        if (!err.contains(QStringLiteral("Unknown database"), Qt::CaseInsensitive))
            return fail(err);

        if (!connectOnce(QString(), host, port, user, password, &err))
            return fail(err);

        {
            QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
            if (!q.exec(QStringLiteral("CREATE DATABASE IF NOT EXISTS `%1` "
                                       "DEFAULT CHARACTER SET utf8mb4 "
                                       "COLLATE utf8mb4_unicode_ci").arg(safeDb))) {
                return fail(queryError(q));
            }
        }
        // 同一条连接上换库，不走 removeDatabase，避免“connection still in use”警告
        {
            QSqlDatabase db = QSqlDatabase::database(QLatin1String(kConn), false);
            db.close();
            db.setDatabaseName(dbName);
            if (!db.open())
                return fail(db.lastError().text().trimmed());
        }
    }

    m_open = true;
    m_endpoint = QStringLiteral("%1:%2/%3").arg(host).arg(port).arg(dbName);

    QString schemaErr;
    if (!initSchema(&schemaErr))
        return fail(schemaErr);

    return true;
}

void Database::close()
{
    if (!QSqlDatabase::contains(QLatin1String(kConn))) {
        m_open = false;
        return;
    }
    {
        QSqlDatabase db = QSqlDatabase::database(QLatin1String(kConn), false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(QLatin1String(kConn));
    m_open = false;
}

bool Database::isOpen() const
{
    if (!m_open)
        return false;
    QSqlDatabase db = QSqlDatabase::database(QLatin1String(kConn), false);
    return db.isValid() && db.isOpen();
}

QSqlDatabase Database::connection() const
{
    return QSqlDatabase::database(QLatin1String(kConn), false);
}

bool Database::initSchema(QString *error)
{
    const QStringList ddl = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS users ("
            "  id         INT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "  username   VARCHAR(32)  NOT NULL,"
            "  pass_hash  CHAR(64)     NOT NULL,"
            "  online     TINYINT(1)   NOT NULL DEFAULT 0,"
            "  created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "  PRIMARY KEY (id),"
            "  UNIQUE KEY uk_username (username)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS messages ("
            "  id        BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "  sender    VARCHAR(32)   NOT NULL,"
            "  receiver  VARCHAR(32)   NOT NULL,"
            "  content   VARCHAR(2000) NOT NULL,"
            "  sent_at   DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "  PRIMARY KEY (id),"
            "  KEY idx_sender (sender),"
            "  KEY idx_receiver (receiver),"
            "  KEY idx_sent (sent_at)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS friends ("
            "  user_a     VARCHAR(32) NOT NULL,"
            "  user_b     VARCHAR(32) NOT NULL,"
            "  created_at DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "  PRIMARY KEY (user_a, user_b)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"),
    };

    for (const QString &sql : ddl) {
        QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
        if (!q.exec(sql)) {
            if (error)
                *error = queryError(q);
            return false;
        }
    }

    // 服务器重启/崩溃后，上一次留下的 online=1 是脏数据，一律清掉
    resetAllOnline();
    return true;
}

int Database::importLegacyUsers(const QString &txtPath)
{
    if (!isOpen() || userCount() > 0)
        return 0;

    QFile file(txtPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0;

    int imported = 0;
    QSqlDatabase db = QSqlDatabase::database(QLatin1String(kConn), false);
    db.transaction();
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        const int sep = line.indexOf(QLatin1Char('|'));
        if (sep <= 0)
            continue;
        QSqlQuery q(db);
        q.prepare(QStringLiteral("INSERT IGNORE INTO users (username, pass_hash) VALUES (?, ?)"));
        q.addBindValue(line.left(sep));
        q.addBindValue(line.mid(sep + 1));
        if (q.exec())
            ++imported;
    }
    db.commit();
    return imported;
}

int Database::importLegacyFriends(const QString &txtPath)
{
    if (!isOpen())
        return 0;
    QFile file(txtPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0;

    // friends.txt 是双向记录的（A|B 和 B|A 各一行），这里只处理 a<b 的组合，避免重复
    int imported = 0;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        const int sep = line.indexOf(QLatin1Char('|'));
        if (sep <= 0)
            continue;
        const QString a = line.left(sep);
        const QStringList friends = line.mid(sep + 1).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &b : friends) {
            if (b.isEmpty() || b == a)
                continue;
            if (!areFriends(a, b) && addFriendPair(a, b, nullptr))
                ++imported;
        }
    }
    return imported;
}

Database::Lookup Database::lookupUser(const QString &user, QString *hash) const{
    QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
    q.prepare(QStringLiteral("SELECT pass_hash FROM users WHERE username = ?"));
    q.addBindValue(user);
    if (!q.exec())
        return Lookup::Error;
    if (!q.next())
        return Lookup::NotFound;
    if (hash)
        *hash = q.value(0).toString();
    return Lookup::Found;
}

bool Database::addUser(const QString &user, const QString &hash, QString *error)
{
    QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
    q.prepare(QStringLiteral("INSERT INTO users (username, pass_hash, online) VALUES (?, ?, 0)"));
    q.addBindValue(user);
    q.addBindValue(hash);
    if (!q.exec()) {
        if (error)
            *error = queryError(q);
        return false;
    }
    return true;
}

bool Database::removeUser(const QString &user, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(QLatin1String(kConn), false);
    db.transaction();

    QSqlQuery q1(db);
    q1.prepare(QStringLiteral("DELETE FROM friends WHERE user_a = ? OR user_b = ?"));
    q1.addBindValue(user);
    q1.addBindValue(user);
    if (!q1.exec()) {
        if (error)
            *error = queryError(q1);
        db.rollback();
        return false;
    }

    QSqlQuery q2(db);
    q2.prepare(QStringLiteral("DELETE FROM users WHERE username = ?"));
    q2.addBindValue(user);
    if (!q2.exec()) {
        if (error)
            *error = queryError(q2);
        db.rollback();
        return false;
    }

    if (!db.commit()) {
        if (error)
            *error = db.lastError().text().trimmed();
        return false;
    }
    return true;
}

bool Database::setOnline(const QString &user, bool online)
{
    QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
    q.prepare(QStringLiteral("UPDATE users SET online = ? WHERE username = ?"));
    q.addBindValue(online ? 1 : 0);
    q.addBindValue(user);
    return q.exec();
}

void Database::resetAllOnline()
{
    QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
    q.exec(QStringLiteral("UPDATE users SET online = 0"));
}

int Database::userCount() const
{
    QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM users")) || !q.next())
        return 0;
    return q.value(0).toInt();
}

int Database::messageCount() const
{
    QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM messages")) || !q.next())
        return 0;
    return q.value(0).toInt();
}

QStringList Database::friendsOf(const QString &user) const
{
    QStringList list;
    QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
    q.prepare(QStringLiteral("SELECT user_b FROM friends WHERE user_a = ? "
                             "UNION SELECT user_a FROM friends WHERE user_b = ? "
                             "ORDER BY 1"));
    q.addBindValue(user);
    q.addBindValue(user);
    if (!q.exec())
        return list;
    while (q.next())
        list.append(q.value(0).toString());
    return list;
}

bool Database::areFriends(const QString &a, const QString &b) const
{
    QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
    q.prepare(QStringLiteral("SELECT 1 FROM friends "
                             "WHERE (user_a = ? AND user_b = ?) "
                             "   OR (user_a = ? AND user_b = ?) LIMIT 1"));
    q.addBindValue(a);
    q.addBindValue(b);
    q.addBindValue(b);
    q.addBindValue(a);
    if (!q.exec())
        return false;
    return q.next();
}

bool Database::addFriendPair(const QString &a, const QString &b, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(QLatin1String(kConn), false);
    db.transaction();
    const QPair<QString, QString> pairs[2] = { qMakePair(a, b), qMakePair(b, a) };
    for (const auto &p : pairs) {
        QSqlQuery q(db);
        // INSERT IGNORE：主键重复（已经是好友）不当作错误
        q.prepare(QStringLiteral("INSERT IGNORE INTO friends (user_a, user_b) VALUES (?, ?)"));
        q.addBindValue(p.first);
        q.addBindValue(p.second);
        if (!q.exec()) {
            if (error)
                *error = queryError(q);
            db.rollback();
            return false;
        }
    }
    if (!db.commit()) {
        if (error)
            *error = db.lastError().text().trimmed();
        return false;
    }
    return true;
}

bool Database::removeAllFriends(const QString &user, QString *error)
{
    QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
    q.prepare(QStringLiteral("DELETE FROM friends WHERE user_a = ? OR user_b = ?"));
    q.addBindValue(user);
    q.addBindValue(user);
    if (!q.exec()) {
        if (error)
            *error = queryError(q);
        return false;
    }
    return true;
}

bool Database::saveMessage(const QString &from, const QString &to,
                           const QString &text, QString *error)
{
    QSqlQuery q(QSqlDatabase::database(QLatin1String(kConn), false));
    q.prepare(QStringLiteral("INSERT INTO messages (sender, receiver, content) VALUES (?, ?, ?)"));
    q.addBindValue(from);
    q.addBindValue(to);
    q.addBindValue(text);
    if (!q.exec()) {
        if (error)
            *error = queryError(q);
        return false;
    }
    return true;
}
