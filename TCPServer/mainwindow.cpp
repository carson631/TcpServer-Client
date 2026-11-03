#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QDir>

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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    setWindowTitle(QStringLiteral("TCP服务器"));

    loadUsers();
    loadFriends();
    refreshUserDbList();   // 管理界面的已注册用户列表

    server = new QTcpServer(this);

    if (!server->listen(QHostAddress::AnyIPv4, 6666)) {
        QMessageBox::critical(this, QStringLiteral("启动失败"),
                              QStringLiteral("监听 6666 端口失败：%1").arg(server->errorString()));
    }

    connect(server, &QTcpServer::newConnection, this, &MainWindow::newClientHandler);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::loadUsers()
{
    // 账号文件格式：用户名|密码的SHA-256十六进制
    QFile file(QStringLiteral("users.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;   // 第一次运行没有这个文件，正常
    }
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        const int sep = line.indexOf(QLatin1Char('|'));
        if (sep > 0) {
            m_users.insert(line.left(sep), line.mid(sep + 1));
        }
    }
}

void MainWindow::saveUser(const QString &user, const QString &hash)
{
    m_users.insert(user, hash);
    QFile file(QStringLiteral("users.txt"));
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out << user << QLatin1Char('|') << hash << QLatin1Char('\n');
    }
}

void MainWindow::saveUsers()
{
    // 有账号被删除时整体重写（users.txt 平时是追加写，删不掉旧记录）
    QFile file(QStringLiteral("users.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return;
    }
    QTextStream out(&file);
    for (auto it = m_users.constBegin(); it != m_users.constEnd(); ++it) {
        out << it.key() << QLatin1Char('|') << it.value() << QLatin1Char('\n');
    }
}

void MainWindow::loadFriends()
{
    // 好友文件格式：用户名|好友1,好友2,...
    QFile file(QStringLiteral("friends.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        const int sep = line.indexOf(QLatin1Char('|'));
        if (sep > 0) {
            m_friends.insert(line.left(sep),
                             line.mid(sep + 1).split(QLatin1Char(','), Qt::SkipEmptyParts));
        }
    }
}

void MainWindow::saveFriends()
{
    QFile file(QStringLiteral("friends.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return;
    }
    QTextStream out(&file);
    for (auto it = m_friends.constBegin(); it != m_friends.constEnd(); ++it) {
        if (!it.value().isEmpty()) {
            out << it.key() << QLatin1Char('|') << it.value().join(QLatin1Char(',')) << QLatin1Char('\n');
        }
    }
}

bool MainWindow::areFriends(const QString &a, const QString &b) const
{
    return m_friends.value(a).contains(b);
}

void MainWindow::addFriendPair(const QString &a, const QString &b)
{
    if (!m_friends[a].contains(b)) {
        m_friends[a].append(b);
    }
    if (!m_friends[b].contains(a)) {
        m_friends[b].append(a);
    }
}

QTcpSocket *MainWindow::socketByUser(const QString &user) const
{
    for (auto it = m_loggedIn.constBegin(); it != m_loggedIn.constEnd(); ++it) {
        if (it.value() == user) {
            return it.key();
        }
    }
    return nullptr;
}

QString MainWindow::hashPassword(const QString &password)
{
    // 不存明文：只存 SHA-256 摘要，文件泄露也拿不到原密码
    return QString::fromLatin1(
        QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex());
}

void MainWindow::newClientHandler()
{
    // 一次事件循环里可能积压了好几个连接，全部取出来
    while (server->hasPendingConnections()) {
        QTcpSocket *client = server->nextPendingConnection();
        m_clients.append(client);   // 记进在线列表

        // 每个客户端一个独立 socket，用 lambda 捕获自己那个，避免多客户端时串数据
        connect(client, &QTcpSocket::readyRead, this, [this, client](){
            m_buffers[client] += client->readAll();
            int idx;
            // TCP 是字节流没有消息边界，按 \n 拆帧；不完整的留在缓冲等下次
            while ((idx = m_buffers[client].indexOf('\n')) >= 0) {
                const QByteArray line = m_buffers[client].left(idx);
                m_buffers[client].remove(0, idx + 1);
                handleLine(client, QString::fromUtf8(line));
            }
        });

        // 客户端断开后从在线列表摘掉，并释放 socket（否则每来一个客户端就漏一个对象）
        connect(client, &QTcpSocket::disconnected, this, [this, client](){
            if (m_loggedIn.contains(client)) {
                m_loggedIn.remove(client);
                refreshUserList();
            }
            m_buffers.remove(client);
            m_pendingReq.remove(client);
            m_clients.removeOne(client);
            refreshClientList();
            client->deleteLater();
        });
    }

    refreshClientList();
}

void MainWindow::handleLine(QTcpSocket *client, const QString &line)
{
    if (line.isEmpty()) {
        return;
    }
    const QStringList parts = line.split(QLatin1Char('|'));
    const QString cmd = parts.value(0);

    // 除 REG / LOG 外的所有命令都要求先登录（LOG 本身就是登录动作，必须放行）
    if (cmd != QLatin1String("REG") && cmd != QLatin1String("LOG") && !m_loggedIn.contains(client)) {
        sendLine(client, QStringLiteral("ERR|请先登录"));
        return;
    }
    const QString me = m_loggedIn.value(client);

    if (cmd == QLatin1String("REG")) {
        const QString user = parts.value(1).trimmed();
        const QString pass = parts.value(2);
        if (user.isEmpty() || pass.isEmpty()) {
            sendLine(client, QStringLiteral("REG_FAIL|用户名和密码不能为空"));
        } else if (m_users.contains(user)) {
            sendLine(client, QStringLiteral("REG_FAIL|用户名已存在"));
        } else {
            saveUser(user, hashPassword(pass));
            refreshUserDbList();   // 管理界面同步出现新用户
            sendLine(client, QStringLiteral("REG_OK|%1").arg(user));
        }

    } else if (cmd == QLatin1String("LOG")) {
        const QString user = parts.value(1).trimmed();
        const QString pass = parts.value(2);
        if (!m_users.contains(user) || m_users.value(user) != hashPassword(pass)) {
            sendLine(client, QStringLiteral("LOG_FAIL|用户名或密码错误"));
        } else if (m_loggedIn.values().contains(user)) {
            sendLine(client, QStringLiteral("LOG_FAIL|该用户已在别处登录"));
        } else {
            m_loggedIn.insert(client, user);
            refreshUserList();
            sendLine(client, QStringLiteral("LOG_OK|%1").arg(user));
            // 登录后把已有好友列表同步下去
            sendLine(client, QStringLiteral("FRIENDS|%1").arg(m_friends.value(user).join(QLatin1Char(','))));
        }

    } else if (cmd == QLatin1String("OUT")) {
        if (m_loggedIn.contains(client)) {
            m_loggedIn.remove(client);
            refreshUserList();
        }
        sendLine(client, QStringLiteral("OUT_OK"));

    } else if (cmd == QLatin1String("ADD")) {
        const QString target = parts.value(1).trimmed();
        if (target.isEmpty() || target == me) {
            sendLine(client, QStringLiteral("ADD_FAIL|不能添加自己"));
        } else if (!m_users.contains(target)) {
            sendLine(client, QStringLiteral("ADD_FAIL|用户 %1 不存在").arg(target));
        } else if (areFriends(me, target)) {
            sendLine(client, QStringLiteral("ADD_FAIL|你们已经是好友"));
        } else {
            QTcpSocket *t = socketByUser(target);
            if (!t) {
                sendLine(client, QStringLiteral("ADD_FAIL|对方不在线，暂时无法发送好友请求"));
            } else if (m_pendingReq[t].contains(me)) {
                sendLine(client, QStringLiteral("ADD_FAIL|请求已发送，等待对方处理"));
            } else {
                m_pendingReq[t].append(me);
                sendLine(t, QStringLiteral("ADD_REQ|%1").arg(me));
                // 请求已送达对方，无需给发起者回执；结果由 AGREE/REFUSE 通知
            }
        }

    } else if (cmd == QLatin1String("AGREE")) {
        const QString requester = parts.value(1).trimmed();
        if (!m_pendingReq.value(client).contains(requester)) {
            sendLine(client, QStringLiteral("ADD_FAIL|没有来自 %1 的好友请求").arg(requester));
        } else {
            m_pendingReq[client].removeAll(requester);
            addFriendPair(me, requester);
            saveFriends();
            refreshUserList();
            sendLine(client, QStringLiteral("AGREE_OK|%1").arg(requester));
            sendLine(client, QStringLiteral("FRIENDS|%1").arg(m_friends.value(me).join(QLatin1Char(','))));
            QTcpSocket *req = socketByUser(requester);
            if (req) {
                sendLine(req, QStringLiteral("ADD_OK|%1").arg(me));
                // 对方还在线的话，把更新后的好友列表也推过去
                sendLine(req, QStringLiteral("FRIENDS|%1").arg(m_friends.value(requester).join(QLatin1Char(','))));
            }
        }

    } else if (cmd == QLatin1String("REFUSE")) {
        const QString requester = parts.value(1).trimmed();
        if (m_pendingReq.value(client).contains(requester)) {
            m_pendingReq[client].removeAll(requester);
            sendLine(client, QStringLiteral("REFUSE_OK|%1").arg(requester));
            QTcpSocket *req = socketByUser(requester);
            if (req) {
                sendLine(req, QStringLiteral("ADD_REJECT|%1").arg(me));
            }
        }

    } else if (cmd == QLatin1String("PMSG")) {
        const QString target = parts.value(1).trimmed();
        const QString text = parts.mid(2).join(QLatin1Char('|')).trimmed();
        if (!areFriends(me, target)) {
            sendLine(client, QStringLiteral("PMSG_FAIL|你和 %1 不是好友").arg(target));
        } else if (text.isEmpty()) {
            sendLine(client, QStringLiteral("PMSG_FAIL|消息内容为空"));
        } else {
            QTcpSocket *t = socketByUser(target);
            if (!t) {
                sendLine(client, QStringLiteral("PMSG_FAIL|%1 不在线").arg(target));
            } else {
                sendLine(t, QStringLiteral("PMSG|%1|%2").arg(me, text));
                // 服务器界面记录所有用户发出的消息，管理员可全量查看
                ui->msgLineEdit->append(QStringLiteral("(%1：%2)").arg(me, text));
            }
        }

    } else {
        sendLine(client, QStringLiteral("ERR|未知命令"));
    }
}

void MainWindow::sendLine(QTcpSocket *client, const QString &line)
{
    client->write((line + QLatin1Char('\n')).toUtf8());
}

void MainWindow::refreshClientList()
{
    ui->clientsTextEdit->clear();
    for (QTcpSocket *client : m_clients) {
        ui->clientsTextEdit->append(QStringLiteral("%1:%2")
                                        .arg(client->peerAddress().toString())
                                        .arg(client->peerPort()));
    }
}

void MainWindow::refreshUserList()
{
    ui->usersTextEdit->clear();
    for (auto it = m_loggedIn.constBegin(); it != m_loggedIn.constEnd(); ++it) {
        ui->usersTextEdit->append(QStringLiteral("%1 (%2:%3)")
                                      .arg(it.value())
                                      .arg(it.key()->peerAddress().toString())
                                      .arg(it.key()->peerPort()));
    }
}

void MainWindow::refreshUserDbList()
{
    ui->userList->clear();
    for (auto it = m_users.constBegin(); it != m_users.constEnd(); ++it) {
        ui->userList->addItem(it.key());
    }
}

void MainWindow::on_addUserButton_clicked()
{
    const QString user = ui->regUserEdit->text().trimmed();
    const QString pass = ui->regPassEdit->text();
    if (user.isEmpty() || pass.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("添加用户"),
                             QStringLiteral("用户名和密码不能为空"));
        return;
    }
    if (m_users.contains(user)) {
        QMessageBox::warning(this, QStringLiteral("添加用户"),
                             QStringLiteral("用户 %1 已存在").arg(user));
        return;
    }
    saveUser(user, hashPassword(pass));
    refreshUserDbList();
    ui->regUserEdit->clear();
    ui->regPassEdit->clear();
    ui->msgLineEdit->append(QStringLiteral("[管理] 用户 %1 已添加").arg(user));
}

void MainWindow::on_deleteUserButton_clicked()
{
    QListWidgetItem *item = ui->userList->currentItem();
    if (!item) {
        QMessageBox::warning(this, QStringLiteral("删除用户"),
                             QStringLiteral("请先在用户列表中选中一个用户"));
        return;
    }
    const QString user = item->text();
    if (QMessageBox::question(this, QStringLiteral("删除用户"),
                              QStringLiteral("确定删除用户 %1 吗？该操作会同时删除其好友关系，不可恢复。").arg(user))
        != QMessageBox::Yes) {
        return;
    }

    // 1. 在线就踢下线：先通知，再断开（disconnected 回调会自己清理 m_loggedIn 并刷新列表）
    if (QTcpSocket *t = socketByUser(user)) {
        sendLine(t, QStringLiteral("ERR|账号已被管理员删除"));
        t->disconnectFromHost();
    }

    // 2. 好友关系双向清除
    const QStringList friends = m_friends.value(user);
    for (const QString &f : friends) {
        m_friends[f].removeAll(user);
    }
    m_friends.remove(user);
    saveFriends();

    // 3. 挂在别人身上的待处理好友请求也清掉
    for (auto it = m_pendingReq.begin(); it != m_pendingReq.end(); ++it) {
        it.value().removeAll(user);
    }

    // 4. 删账号并重写 users.txt
    m_users.remove(user);
    saveUsers();
    refreshUserDbList();
    refreshUserList();   // 若被删用户在线，登录列表同步移除
    ui->msgLineEdit->append(QStringLiteral("[管理] 用户 %1 已删除").arg(user));
}
