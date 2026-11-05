#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QElapsedTimer>

static void dump(const QString &path)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Text);
    QTextStream ts(&f);

    ts << "cwd: " << QDir::currentPath() << "\n";
    ts.flush();

    ts << "drivers: " << QSqlDatabase::drivers().join(',') << "\n";
    ts.flush();

    QElapsedTimer t;
    t.start();
    QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL");
    db.setHostName("192.168.5.50");
    db.setPort(3306);
    db.setUserName("chat");
    db.setPassword("chat123456");
    db.setDatabaseName("tcp_chat");
    ts << "connecting to 192.168.5.50:3306 ...\n";
    ts.flush();
    bool ok = db.open();
    ts << "open: " << (ok ? "OK" : QString("FAIL: %1").arg(db.lastError().text())) << "\n";
    ts << "elapsed ms: " << t.elapsed() << "\n";
}

int main(int argc, char **argv)
{
    QCoreApplication a(argc, argv);
    dump(argc > 1 ? QString::fromLocal8Bit(argv[1])
                  : QStringLiteral("dbsqltest_result.txt"));
    return 0;
}
