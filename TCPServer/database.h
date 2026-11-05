#ifndef DATABASE_H
#define DATABASE_H

#include <QSqlDatabase>
#include <QString>
#include <QStringList>

// MySQL 访问层。
//
// 线程约定：整个程序只在**主线程**用这一个连接（QSqlDatabase 的连接不能跨线程共享）。
// 多线程只用在 socket 收发上，业务逻辑和 SQL 一律回到主线程执行，
// 这样既不用给每个线程开一条数据库连接，也不会有连接池和事务互相踩的问题。
class Database
{
public:
    enum class Lookup { Error, NotFound, Found };

    static Database &instance();

    // 连接；如果目标库还不存在会自动创建（需要账号有 CREATE 权限）
    bool open(const QString &host, int port, const QString &dbName,
              const QString &user, const QString &password, QString *error);
    void close();
    bool isOpen() const;
    QString lastError() const { return m_error; }
    QString endpoint() const { return m_endpoint; }
    // 给 QSqlQueryModel::setQuery(sql, db) 用——模型默认走默认连接，而我们用的是具名连接
    QSqlDatabase connection() const;

    // 建表（幂等），连上后自动调用
    bool initSchema(QString *error);
    // 把老的 users.txt（用户名|SHA256）/ friends.txt（用户名|好友1,好友2）导入数据库
    // 返回导入条数；数据库里已经有数据时跳过
    int importLegacyUsers(const QString &txtPath);
    int importLegacyFriends(const QString &txtPath);

    // --- 用户 ---
    Lookup lookupUser(const QString &user, QString *hash = nullptr) const;
    bool addUser(const QString &user, const QString &hash, QString *error);
    bool removeUser(const QString &user, QString *error);      // 连带清好友关系
    bool setOnline(const QString &user, bool online);
    void resetAllOnline();                                     // 启动时清残留的在线标记
    int userCount() const;
    int messageCount() const;

    // --- 好友 ---
    QStringList friendsOf(const QString &user) const;
    bool areFriends(const QString &a, const QString &b) const;
    bool addFriendPair(const QString &a, const QString &b, QString *error);
    bool removeAllFriends(const QString &user, QString *error);

    // --- 消息 ---
    bool saveMessage(const QString &from, const QString &to, const QString &text, QString *error);

private:
    Database() = default;
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool m_open = false;
    QString m_error;
    QString m_endpoint;
};

#endif // DATABASE_H
