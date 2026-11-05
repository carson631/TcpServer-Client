#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "database.h"
#include "mytcpserver.h"
#include "mythread.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QHostAddress>
#include <QMessageBox>
#include <QSettings>
#include <QSqlError>
#include <QSqlQueryModel>
#include <QTextStream>
#include <QTime>
#include <QTimer>

// ---- 协议（行帧：每条以 \n 结尾，字段用 | 分隔）----
// 客户端 -> 服务器：
//   REG|用户名|密码        注册
//   LOG|用户名|密码        登录
//   OUT                    登出
//   ADD|好友用户名         发好友请求（对方必须在线）
//   AGREE|好友用户名       同意好友请求
//   REFUSE|好友用户名      拒绝好友请求
//   PMSG|好友用户名|内容   给好友发私聊消息
// 服务器 -> 客户端：
//   REG_OK|用户名  REG_FAIL|原因  LOG_OK|用户名  LOG_FAIL|原因
//   OUT_OK  ERR|原因
//   FRIENDS|好友1,好友2,...        登录后同步好友列表
//   ADD_REQ|发起者  ADD_OK|对方  ADD_FAIL|原因  ADD_REJECT|对方
//   AGREE_OK|对方  REFUSE_OK|对方
//   PMSG|发送者|内容  PMSG_FAIL|原因

namespace {

// 用户表只做两件小事：数字列居中、在线/离线配色。
// 数据本身仍然全部来自 SQL 查询结果（QSqlQueryModel）。
class UserQueryModel : public QSqlQueryModel
{
public:
    explicit UserQueryModel(QObject *parent = nullptr)
        : QSqlQueryModel(parent)
    {
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (index.isValid()) {
            if (role == Qt::TextAlignmentRole
                && (index.column() == 0 || index.column() == 4 || index.column() == 5)) {
                return int(Qt::AlignCenter);
            }
            if (role == Qt::ForegroundRole && index.column() == 2) {
                const QString s = QSqlQueryModel::data(index, Qt::DisplayRole).toString();
                return s == QStringLiteral("在线") ? QColor(0x23, 0x8b, 0x4e)
                                                   : QColor(0x8c, 0x8c, 0x8c);
            }
        }
        return QSqlQueryModel::data(index, role);
    }
};

QString nowStamp()
{
    return QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    // 队列连接要把 MyThread* 当参数跨线程传，必须先注册这个类型
    qRegisterMetaType<MyThread *>("MyThread*");

    setWindowTitle(QStringLiteral("TCP服务器"));

    setupUserTable();

    m_tableTimer = new QTimer(this);
    m_tableTimer->setSingleShot(true);
    m_tableTimer->setInterval(400);
    connect(m_tableTimer, &QTimer::timeout, this, [this]() {
        refreshUserTable(ui->queryUserEdit->text());
    });

    // MySQL 没起来（虚拟机关机/服务还没 ready）时，自动重试几次，别让人干等
    m_dbRetryTimer = new QTimer(this);
    m_dbRetryTimer->setSingleShot(true);
    connect(m_dbRetryTimer, &QTimer::timeout, this, &MainWindow::on_connectDbButton_clicked);

    initDatabase();

    // ---- 多线程服务器 ----
    m_server = new MyTcpServer(this);
    connect(m_server, &MyTcpServer::newClientDescriptor, this, &MainWindow::onNewClient);

    if (!m_server->listen(QHostAddress::AnyIPv4, 6666)) {
        QMessageBox::critical(this, QStringLiteral("启动失败"),
                              QStringLiteral("监听 6666 端口失败：%1").arg(m_server->errorString()));
    } else {
        logMessage(QStringLiteral("服务器已启动：0.0.0.0:6666，每个连接一个工作线程"));
    }

    appendLogLine(QStringLiteral("协议：REG/LOG/OUT/ADD/AGREE/REFUSE/PMSG，行帧以 \\n 分隔"));
}

MainWindow::~MainWindow()
{
    // 先把线程停干净再销毁界面，否则 QThread 还在跑就被析构会直接崩
    const QList<MyThread *> threads = m_clients;
    for (MyThread *t : threads)
        t->stopThread();
    for (MyThread *t : threads)
        t->wait(1500);

    delete ui;
}

// ==================== 数据库 ====================

void MainWindow::setupUserTable()
{
    m_userModel = new UserQueryModel(this);
    ui->usersTable->setModel(m_userModel);
    ui->usersTable->setSortingEnabled(false);      // 排序交给 SQL 的 ORDER BY
    ui->usersTable->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->usersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->usersTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->usersTable->verticalHeader()->setVisible(false);
    ui->usersTable->verticalHeader()->setDefaultSectionSize(30);
    ui->usersTable->horizontalHeader()->setHighlightSections(false);
    ui->usersTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui->usersTable->horizontalHeader()->setStretchLastSection(true);
}

void MainWindow::initDatabase()
{
    QSettings s;
    const QString host = qEnvironmentVariable(
        "TCPS_DB_HOST", s.value(QStringLiteral("db/host"), QStringLiteral("192.168.5.50")).toString());
    const int port = qEnvironmentVariable(
                         "TCPS_DB_PORT", QString::number(s.value(QStringLiteral("db/port"), 3306).toInt()))
                         .toInt();
    const QString name = qEnvironmentVariable(
        "TCPS_DB_NAME", s.value(QStringLiteral("db/name"), QStringLiteral("tcp_chat")).toString());
    const QString user = qEnvironmentVariable(
        "TCPS_DB_USER", s.value(QStringLiteral("db/user"), QStringLiteral("chat")).toString());
    const QString pass = qEnvironmentVariable(
        "TCPS_DB_PASS", s.value(QStringLiteral("db/pass"), QStringLiteral("chat123456")).toString());

    ui->dbHostEdit->setText(host);
    ui->dbPortSpin->setValue(port > 0 ? port : 3306);
    ui->dbNameEdit->setText(name);
    ui->dbUserEdit->setText(user);
    ui->dbPassEdit->setText(pass);

    on_connectDbButton_clicked();   // 启动时自动连一次
}

void MainWindow::on_connectDbButton_clicked()
{
    const QString host = ui->dbHostEdit->text().trimmed();
    const int port = ui->dbPortSpin->value();
    const QString name = ui->dbNameEdit->text().trimmed();
    const QString user = ui->dbUserEdit->text().trimmed();
    const QString pass = ui->dbPassEdit->text();

    ui->dbConnectButton->setEnabled(false);
    ui->dbConnectButton->setText(QStringLiteral("连接中…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);

    QString err;
    const bool ok = Database::instance().open(host, port, name, user, pass, &err);

    QApplication::restoreOverrideCursor();
    ui->dbConnectButton->setEnabled(true);
    ui->dbConnectButton->setText(QStringLiteral("连接数据库"));

    if (ok) {
        QSettings s;
        s.setValue(QStringLiteral("db/host"), host);
        s.setValue(QStringLiteral("db/port"), port);
        s.setValue(QStringLiteral("db/name"), name);
        s.setValue(QStringLiteral("db/user"), user);
        s.setValue(QStringLiteral("db/pass"), pass);
        m_dbRetry = 0;
        importLegacyFiles();
    }
    applyDbState(ok, ok ? Database::instance().endpoint() : err);

    // 连接失败：最多自动重试 6 次（每次 5 秒），覆盖「虚拟机关着 / MySQL 还在启动」
    if (!ok && m_dbRetry < 6) {
        ++m_dbRetry;
        logMessage(QStringLiteral("[数据库] 5 秒后自动重试（%1/6）").arg(m_dbRetry));
        m_dbRetryTimer->start(5000);
    }
}

void MainWindow::applyDbState(bool ok, const QString &detail)
{
    if (ok) {
        ui->dbStatusLabel->setStyleSheet(QStringLiteral("color:#238b4e;font-weight:bold;"));
        ui->dbStatusLabel->setText(QStringLiteral("● 已连接 %1").arg(detail));
        // 重连时 initSchema 会把 online 全部清零，这里把当前真实在线的用户标回去
        for (const QString &user : m_loggedIn.values())
            Database::instance().setOnline(user, true);
        logMessage(QStringLiteral("[数据库] 已连接 %1（用户 %2 个，消息 %3 条）")
                       .arg(detail)
                       .arg(Database::instance().userCount())
                       .arg(Database::instance().messageCount()));
        refreshUserTable(ui->queryUserEdit->text());
    } else {
        ui->dbStatusLabel->setStyleSheet(QStringLiteral("color:#d4380d;font-weight:bold;"));
        ui->dbStatusLabel->setText(QStringLiteral("● 连接失败"));
        m_userModel->clear();
        ui->statsLabel->setText(QStringLiteral("数据库未连接"));
        logMessage(QStringLiteral("[数据库] 连接失败：%1").arg(detail));
    }
}

void MainWindow::importLegacyFiles()
{
    // 老版本把账号存在程序目录的 users.txt / friends.txt 里，第一次连上数据库时搬进去
    QString dir = QDir::currentPath();
    if (!QFile::exists(dir + QStringLiteral("/users.txt")))
        dir = QCoreApplication::applicationDirPath();

    const int users = Database::instance().importLegacyUsers(dir + QStringLiteral("/users.txt"));
    if (users > 0)
        logMessage(QStringLiteral("[迁移] 从 users.txt 导入了 %1 个账号").arg(users));

    const int friends = Database::instance().importLegacyFriends(dir + QStringLiteral("/friends.txt"));
    if (friends > 0)
        logMessage(QStringLiteral("[迁移] 从 friends.txt 导入了 %1 条好友关系").arg(friends));
}

void MainWindow::refreshUserTable(const QString &keyword)
{
    if (!Database::instance().isOpen()) {
        m_userModel->clear();
        ui->statsLabel->setText(QStringLiteral("数据库未连接"));
        return;
    }

    // 关键字自己拼进 LIKE，先把会破坏 SQL 的字符转义掉
    QString kw = keyword.trimmed();
    kw.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    kw.replace(QLatin1Char('\''), QLatin1String("''"));
    kw.replace(QLatin1Char('%'), QLatin1String("\\%"));
    kw.replace(QLatin1Char('_'), QLatin1String("\\_"));

    QString sql = QStringLiteral(
        "SELECT u.id AS `ID`, u.username AS `用户名`, "
        "CASE WHEN u.online = 1 THEN '在线' ELSE '离线' END AS `状态`, "
        "DATE_FORMAT(u.created_at, '%Y-%m-%d %H:%i') AS `注册时间`, "
        "(SELECT COUNT(*) FROM messages m WHERE m.sender = u.username) AS `发送`, "
        // friends 表双向各存一行（A|B 和 B|A），必须按「对方」去重，
        // 否则 N 个好友会被数成 2N
        "(SELECT COUNT(DISTINCT CASE WHEN f.user_a = u.username THEN f.user_b "
        "ELSE f.user_a END) FROM friends f "
        "WHERE f.user_a = u.username OR f.user_b = u.username) AS `好友` "
        "FROM users u ");
    if (!kw.isEmpty())
        sql += QStringLiteral("WHERE u.username LIKE '%%1%' ").arg(kw);
    sql += QStringLiteral("ORDER BY u.id");

    m_userModel->setQuery(sql, Database::instance().connection());
    if (m_userModel->lastError().isValid()) {
        logMessage(QStringLiteral("[数据库] 查询失败：%1").arg(m_userModel->lastError().text()));
    }

    ui->usersTable->resizeColumnsToContents();
    ui->statsLabel->setText(QStringLiteral("共 %1 个用户 · %2 条消息")
                                .arg(m_userModel->rowCount())
                                .arg(Database::instance().messageCount()));
}

void MainWindow::scheduleTableRefresh()
{
    if (m_tableTimer && !m_tableTimer->isActive())
        m_tableTimer->start();
}

void MainWindow::on_queryUserButton_clicked()
{
    if (!dbReady())
        return;
    refreshUserTable(ui->queryUserEdit->text());
    logMessage(QStringLiteral("[查询] 关键字「%1」→ %2 条结果")
                   .arg(ui->queryUserEdit->text().trimmed(),
                        QString::number(m_userModel->rowCount())));
}

// ==================== 线程与连接 ====================

void MainWindow::onNewClient(qintptr socketDescriptor)
{
    auto *thread = new MyThread(socketDescriptor, this);

    connect(thread, &MyThread::sigConnected, this, &MainWindow::onClientConnected);
    connect(thread, &MyThread::sigDisconnected, this, &MainWindow::onClientDisconnected);
    connect(thread, &MyThread::sigLine, this, &MainWindow::onClientLine);
    connect(thread, &MyThread::sigError, this, [this](MyThread *, const QString &msg) {
        logMessage(QStringLiteral("[线程] %1").arg(msg));
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    m_clients.append(thread);
    thread->start();
}

void MainWindow::onClientConnected(MyThread *thread, const QString &peer)
{
    m_peers.insert(thread, peer);
    refreshClientList();
    logMessage(QStringLiteral("[连接] %1 接入（新线程 #%2）")
                   .arg(peer)
                   .arg(reinterpret_cast<quintptr>(thread)));
}

void MainWindow::onClientDisconnected(MyThread *thread, const QString &peer)
{
    if (m_loggedIn.contains(thread)) {
        const QString user = m_loggedIn.take(thread);
        Database::instance().setOnline(user, false);
        refreshOnlineList();
        scheduleTableRefresh();
        logMessage(QStringLiteral("[下线] %1（%2）").arg(user, peer));
    } else {
        logMessage(QStringLiteral("[断开] %1").arg(peer));
    }

    m_pendingReq.remove(thread);
    m_peers.remove(thread);
    m_clients.removeOne(thread);
    refreshClientList();
}

void MainWindow::onClientLine(MyThread *thread, const QString &line)
{
    handleLine(thread, line);
}

// ==================== 协议处理（全部在主线程，可直接用数据库） ====================

void MainWindow::handleLine(MyThread *client, const QString &line)
{
    if (line.isEmpty())
        return;

    const QStringList parts = line.split(QLatin1Char('|'));
    const QString cmd = parts.value(0);

    // 除 REG / LOG 外的所有命令都要求先登录
    if (cmd != QLatin1String("REG") && cmd != QLatin1String("LOG")
        && !m_loggedIn.contains(client)) {
        sendLine(client, QStringLiteral("ERR|请先登录"));
        return;
    }

    const QString me = m_loggedIn.value(client);
    Database &db = Database::instance();

    if (cmd == QLatin1String("REG")) {
        const QString user = parts.value(1).trimmed();
        const QString pass = parts.value(2);
        if (user.isEmpty() || pass.isEmpty()) {
            sendLine(client, QStringLiteral("REG_FAIL|用户名和密码不能为空"));
            return;
        }
        if (!dbReady()) {
            sendLine(client, QStringLiteral("REG_FAIL|服务器数据库未连接"));
            return;
        }
        switch (db.lookupUser(user)) {
        case Database::Lookup::Found:
            sendLine(client, QStringLiteral("REG_FAIL|用户名已存在"));
            return;
        case Database::Lookup::Error:
            sendLine(client, QStringLiteral("REG_FAIL|数据库查询失败"));
            return;
        case Database::Lookup::NotFound:
            break;
        }

        QString err;
        if (!db.addUser(user, hashPassword(pass), &err)) {
            sendLine(client, QStringLiteral("REG_FAIL|写入失败：%1").arg(err));
            logMessage(QStringLiteral("[注册失败] %1：%2").arg(user, err));
            return;
        }
        refreshUserTable(ui->queryUserEdit->text());   // 注册即时出现在表格里
        logMessage(QStringLiteral("[注册] %1 已写入数据库").arg(user));
        sendLine(client, QStringLiteral("REG_OK|%1").arg(user));

    } else if (cmd == QLatin1String("LOG")) {
        const QString user = parts.value(1).trimmed();
        const QString pass = parts.value(2);
        if (!dbReady()) {
            sendLine(client, QStringLiteral("LOG_FAIL|服务器数据库未连接"));
            return;
        }
        QString hash;
        const Database::Lookup lk = db.lookupUser(user, &hash);
        if (lk == Database::Lookup::Error) {
            sendLine(client, QStringLiteral("LOG_FAIL|数据库查询失败"));
        } else if (lk == Database::Lookup::NotFound || hash != hashPassword(pass)) {
            sendLine(client, QStringLiteral("LOG_FAIL|用户名或密码错误"));
        } else if (m_loggedIn.values().contains(user)) {
            sendLine(client, QStringLiteral("LOG_FAIL|该用户已在别处登录"));
        } else {
            m_loggedIn.insert(client, user);
            db.setOnline(user, true);
            refreshOnlineList();
            refreshUserTable(ui->queryUserEdit->text());   // 状态列立刻变「在线」
            sendLine(client, QStringLiteral("LOG_OK|%1").arg(user));
            sendLine(client, QStringLiteral("FRIENDS|%1").arg(db.friendsOf(user).join(QLatin1Char(','))));
            logMessage(QStringLiteral("[登录] %1（%2）").arg(user, m_peers.value(client)));
        }

    } else if (cmd == QLatin1String("OUT")) {
        if (m_loggedIn.contains(client)) {
            const QString user = m_loggedIn.take(client);
            db.setOnline(user, false);
            refreshOnlineList();
            refreshUserTable(ui->queryUserEdit->text());
            logMessage(QStringLiteral("[登出] %1").arg(user));
        }
        sendLine(client, QStringLiteral("OUT_OK"));

    } else if (cmd == QLatin1String("ADD")) {
        const QString target = parts.value(1).trimmed();
        if (target.isEmpty() || target == me) {
            sendLine(client, QStringLiteral("ADD_FAIL|不能添加自己"));
            return;
        }
        if (!dbReady()) {
            sendLine(client, QStringLiteral("ADD_FAIL|服务器数据库未连接"));
            return;
        }
        const Database::Lookup lk = db.lookupUser(target);
        if (lk == Database::Lookup::NotFound) {
            sendLine(client, QStringLiteral("ADD_FAIL|用户 %1 不存在").arg(target));
        } else if (lk == Database::Lookup::Error) {
            sendLine(client, QStringLiteral("ADD_FAIL|数据库查询失败"));
        } else if (db.areFriends(me, target)) {
            sendLine(client, QStringLiteral("ADD_FAIL|你们已经是好友"));
        } else {
            MyThread *t = threadByUser(target);
            if (!t) {
                sendLine(client, QStringLiteral("ADD_FAIL|对方不在线，暂时无法发送好友请求"));
            } else if (m_pendingReq[t].contains(me)) {
                sendLine(client, QStringLiteral("ADD_FAIL|请求已发送，等待对方处理"));
            } else {
                m_pendingReq[t].append(me);
                sendLine(t, QStringLiteral("ADD_REQ|%1").arg(me));
                logMessage(QStringLiteral("[好友] %1 → %2 发送好友请求").arg(me, target));
            }
        }

    } else if (cmd == QLatin1String("AGREE")) {
        const QString requester = parts.value(1).trimmed();
        if (!m_pendingReq.value(client).contains(requester)) {
            sendLine(client, QStringLiteral("ADD_FAIL|没有来自 %1 的好友请求").arg(requester));
            return;
        }
        m_pendingReq[client].removeAll(requester);
        if (!dbReady()) {
            sendLine(client, QStringLiteral("ADD_FAIL|服务器数据库未连接"));
            return;
        }
        QString err;
        if (!db.addFriendPair(me, requester, &err)) {
            sendLine(client, QStringLiteral("ADD_FAIL|保存好友关系失败"));
            logMessage(QStringLiteral("[好友] 写入失败：%1").arg(err));
            return;
        }
        refreshUserTable(ui->queryUserEdit->text());
        logMessage(QStringLiteral("[好友] %1 与 %2 已成为好友").arg(me, requester));
        sendLine(client, QStringLiteral("AGREE_OK|%1").arg(requester));
        sendLine(client, QStringLiteral("FRIENDS|%1").arg(db.friendsOf(me).join(QLatin1Char(','))));
        if (MyThread *req = threadByUser(requester)) {
            sendLine(req, QStringLiteral("ADD_OK|%1").arg(me));
            sendLine(req, QStringLiteral("FRIENDS|%1").arg(db.friendsOf(requester).join(QLatin1Char(','))));
        }

    } else if (cmd == QLatin1String("REFUSE")) {
        const QString requester = parts.value(1).trimmed();
        if (m_pendingReq.value(client).contains(requester)) {
            m_pendingReq[client].removeAll(requester);
            sendLine(client, QStringLiteral("REFUSE_OK|%1").arg(requester));
            if (MyThread *req = threadByUser(requester))
                sendLine(req, QStringLiteral("ADD_REJECT|%1").arg(me));
        }

    } else if (cmd == QLatin1String("PMSG")) {
        const QString target = parts.value(1).trimmed();
        const QString text = parts.mid(2).join(QLatin1Char('|')).trimmed();
        if (!dbReady()) {
            sendLine(client, QStringLiteral("PMSG_FAIL|服务器数据库未连接"));
            return;
        }
        if (!db.areFriends(me, target)) {
            sendLine(client, QStringLiteral("PMSG_FAIL|你和 %1 不是好友").arg(target));
        } else if (text.isEmpty()) {
            sendLine(client, QStringLiteral("PMSG_FAIL|消息内容为空"));
        } else {
            MyThread *t = threadByUser(target);
            if (!t) {
                sendLine(client, QStringLiteral("PMSG_FAIL|%1 不在线").arg(target));
            } else {
                QString err;
                if (!db.saveMessage(me, target, text, &err))
                    logMessage(QStringLiteral("[数据库] 消息入库失败：%1").arg(err));
                sendLine(t, QStringLiteral("PMSG|%1|%2").arg(me, text));
                logMessage(QStringLiteral("(%1 → %2：%3)").arg(me, target, text));
                scheduleTableRefresh();   // 「发送」列的数会变，合并刷新
            }
        }

    } else {
        sendLine(client, QStringLiteral("ERR|未知命令"));
    }
}

void MainWindow::sendLine(MyThread *client, const QString &line)
{
    if (client)
        client->sendLine(line);
}

// ==================== 列表 / 日志 ====================

void MainWindow::refreshClientList()
{
    ui->clientsTextEdit->clear();
    for (MyThread *t : m_clients) {
        const QString peer = m_peers.value(t, QStringLiteral("(握手中)"));
        ui->clientsTextEdit->append(QStringLiteral("%1    #%2").arg(peer).arg(reinterpret_cast<quintptr>(t)));
    }
}

void MainWindow::refreshOnlineList()
{
    ui->usersTextEdit->clear();
    for (auto it = m_loggedIn.constBegin(); it != m_loggedIn.constEnd(); ++it) {
        ui->usersTextEdit->append(
            QStringLiteral("%1 (%2)").arg(it.value(), m_peers.value(it.key(), QStringLiteral("?"))));
    }
}

MyThread *MainWindow::threadByUser(const QString &user) const
{
    for (auto it = m_loggedIn.constBegin(); it != m_loggedIn.constEnd(); ++it) {
        if (it.value() == user)
            return it.key();
    }
    return nullptr;
}

QString MainWindow::hashPassword(const QString &password)
{
    // 不存明文：只存 SHA-256 摘要
    return QString::fromLatin1(
        QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex());
}

void MainWindow::logMessage(const QString &msg)
{
    appendLogLine(QStringLiteral("[%1] %2").arg(nowStamp(), msg));
}

void MainWindow::appendLogLine(const QString &line)
{
    if (ui && ui->msgLineEdit)
        ui->msgLineEdit->append(line);

    // 需要留档/无界面自测时，设了 TCPS_LOG 就同时写文件
    const QString path = qEnvironmentVariable("TCPS_LOG");
    if (!path.isEmpty()) {
        QFile f(path);
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream out(&f);
            out << line << QLatin1Char('\n');
        }
    }
}

bool MainWindow::dbReady()
{
    if (Database::instance().isOpen())
        return true;
    logMessage(QStringLiteral("[数据库] 未连接，操作被拒绝"));
    return false;
}

// ==================== 管理按钮 ====================

void MainWindow::on_addUserButton_clicked()
{
    const QString user = ui->regUserEdit->text().trimmed();
    const QString pass = ui->regPassEdit->text();
    if (user.isEmpty() || pass.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("添加用户"),
                             QStringLiteral("用户名和密码不能为空"));
        return;
    }
    if (!dbReady())
        return;

    Database &db = Database::instance();
    switch (db.lookupUser(user)) {
    case Database::Lookup::Found:
        QMessageBox::warning(this, QStringLiteral("添加用户"),
                             QStringLiteral("用户 %1 已存在").arg(user));
        return;
    case Database::Lookup::Error:
        QMessageBox::warning(this, QStringLiteral("添加用户"),
                             QStringLiteral("数据库查询失败：%1").arg(db.lastError()));
        return;
    case Database::Lookup::NotFound:
        break;
    }

    QString err;
    if (!db.addUser(user, hashPassword(pass), &err)) {
        QMessageBox::warning(this, QStringLiteral("添加用户"),
                             QStringLiteral("写入数据库失败：\n%1").arg(err));
        return;
    }

    refreshUserTable(ui->queryUserEdit->text());   // 立刻出现在 QTableView 里
    ui->regUserEdit->clear();
    ui->regPassEdit->clear();
    logMessage(QStringLiteral("[管理] 用户 %1 已写入数据库").arg(user));
}

void MainWindow::on_deleteUserButton_clicked()
{
    const QModelIndex cur = ui->usersTable->currentIndex();
    if (!cur.isValid()) {
        QMessageBox::warning(this, QStringLiteral("删除用户"),
                             QStringLiteral("请先在表格里选中一个用户"));
        return;
    }
    const QString user = m_userModel->index(cur.row(), 1).data().toString();
    if (user.isEmpty())
        return;

    if (QMessageBox::question(this, QStringLiteral("删除用户"),
                              QStringLiteral("确定删除用户 %1 吗？\n"
                                             "会同时删除其好友关系，消息记录保留。不可恢复。")
                                  .arg(user))
        != QMessageBox::Yes) {
        return;
    }
    if (!dbReady())
        return;

    // 1. 在线就先通知再断开（先入队，线程发完队列自己收工）
    if (MyThread *t = threadByUser(user)) {
        sendLine(t, QStringLiteral("ERR|账号已被管理员删除"));
        t->requestClose();
    }

    // 2. 删数据库（好友关系一起清）
    QString err;
    if (!Database::instance().removeUser(user, &err)) {
        QMessageBox::warning(this, QStringLiteral("删除用户"),
                             QStringLiteral("从数据库删除失败：\n%1").arg(err));
        return;
    }

    // 3. 挂在线程上的待处理好友请求也清掉
    for (auto it = m_pendingReq.begin(); it != m_pendingReq.end(); ++it)
        it.value().removeAll(user);

    refreshUserTable(ui->queryUserEdit->text());   // 立刻从 QTableView 里消失
    refreshOnlineList();
    logMessage(QStringLiteral("[管理] 用户 %1 已从数据库删除").arg(user));
}
