// 排查用：打印某用户的好友关系原始行，以及修复前/后两种计数方式的差异
// 用法: friendcheck.exe <用户名> <输出文件>
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication a(argc, argv);
    const QString user = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("aaa");
    const QString out  = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("friendcheck.txt");

    QFile f(out);
    f.open(QIODevice::WriteOnly | QIODevice::Text);
    QTextStream ts(&f);

    QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL");
    db.setHostName("192.168.5.50");
    db.setPort(3306);
    db.setUserName("chat");
    db.setPassword("chat123456");
    db.setDatabaseName("tcp_chat");
    if (!db.open()) {
        ts << "open failed: " << db.lastError().text() << "\n";
        return 1;
    }

    ts << "== friends 表中涉及 " << user << " 的原始行 ==\n";
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT user_a, user_b FROM friends WHERE user_a = ? OR user_b = ? ORDER BY user_a"));
    q.addBindValue(user);
    q.addBindValue(user);
    q.exec();
    int rows = 0;
    while (q.next()) {
        ts << "  " << q.value(0).toString() << " | " << q.value(1).toString() << "\n";
        ++rows;
    }
    ts << "  原始行数 = " << rows << "\n\n";

    ts << "== 两种计数方式 ==\n";
    QSqlQuery c(db);
    c.prepare(QStringLiteral(
        "SELECT (SELECT COUNT(*) FROM friends f WHERE f.user_a = ? OR f.user_b = ?) AS old_cnt, "
        "       (SELECT COUNT(DISTINCT CASE WHEN f.user_a = ? THEN f.user_b ELSE f.user_a END) "
        "          FROM friends f WHERE f.user_a = ? OR f.user_b = ?) AS new_cnt"));
    c.addBindValue(user); c.addBindValue(user);
    c.addBindValue(user); c.addBindValue(user); c.addBindValue(user);
    c.exec();
    if (c.next())
        ts << "  旧(COUNT*)      = " << c.value(0).toInt() << "\n"
           << "  新(COUNT DISTINCT) = " << c.value(1).toInt() << "\n";
    else
        ts << "  查询失败: " << c.lastError().text() << "\n";

    return 0;
}
