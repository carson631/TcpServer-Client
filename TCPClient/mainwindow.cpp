#include "mainwindow.h"
#include "ui_mainwindow.h"

// ---- 协议（行帧：每条以 \n 结尾，字段用 | 分隔）----
// 客户端 -> 服务器：
//   REG|用户名|密码        注册
//   LOG|用户名|密码        登录
//   OUT                    登出
//   MSG|内容               （已废弃的群发，保留兼容）
//   ADD|好友用户名         发好友请求（对方必须在线）
//   AGREE|好友用户名       同意好友请求
//   REFUSE|好友用户名      拒绝好友请求
//   PMSG|好友用户名|内容   给好友发私聊消息
// 服务器 -> 客户端：
//   REG_OK|用户名  REG_FAIL|原因  LOG_OK|用户名  LOG_FAIL|原因
//   OUT_OK  ERR|原因
//   FRIENDS|好友1,好友2,...        登录后同步好友列表
//   ADD_REQ|发起者                 有人请求加你为好友
//   ADD_OK|对方                    对方同意了你的请求（双方都成好友）
//   ADD_FAIL|原因                  好友请求被拒绝/失败
//   ADD_REJECT|对方                对方拒绝了你的请求
//   AGREE_OK|对方                  你同意请求的确认
//   REFUSE_OK|对方                 你拒绝请求的确认
//   PMSG|发送者|内容               收到私聊消息
//   PMSG_FAIL|原因                 私聊失败（不是好友/对方不在线）

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle(QStringLiteral("TCP客户端"));   // 未连接时的默认标题

    socket = new QTcpSocket(this);   // 挂对象树，窗口析构自动回收

    // 信号连接只做一次：放构造函数，绝不放按钮槽里（否则点几次挂几份，弹几个框）
    connect(socket, &QTcpSocket::connected, this, [this](){
        // 标题栏显示本机（客户端）的 ip:port —— localAddress/localPort 是本端，
        // peerAddress/peerPort 是服务器端，别搞反
        setWindowTitle(QStringLiteral("TCP客户端：%1:%2")
                           .arg(socket->localAddress().toString())
                           .arg(socket->localPort()));
        updateAuthUi();
        QMessageBox::information(this, QStringLiteral("连接提示"), QStringLiteral("连接成功"));
    });
    connect(socket, &QTcpSocket::disconnected, this, [this](){
        m_user.clear();   // 断线即掉登录态
        clearFriendUi();
        setWindowTitle(QStringLiteral("TCP客户端"));
        updateAuthUi();
        // 关窗口时的主动断开不弹框，只有手动点"断开"才提示
        if (!m_closing) {
            QMessageBox::information(this, QStringLiteral("连接提示"), QStringLiteral("连接断开"));
        }
    });
    // 连接失败没有这个信号就会安静失败，用户什么都看不到
    connect(socket, &QTcpSocket::errorOccurred, this, [this](){
        QMessageBox::warning(this, QStringLiteral("连接失败"), socket->errorString());
    });
    connect(socket, &QTcpSocket::readyRead, this, &MainWindow::onReadyRead);

    updateAuthUi();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 关窗口 = 主动断开：先立标志再断开，disconnected 弹框被跳过
    m_closing = true;
    if (socket->state() == QAbstractSocket::ConnectedState
        || socket->state() == QAbstractSocket::ConnectingState) {
        socket->disconnectFromHost();
    }
    event->accept();   // 窗口照常关闭，socket 挂对象树随窗口一起回收
}

void MainWindow::on_cancelButton_clicked()
{
    // 断开连接而非关窗口。disconnected 信号会弹"连接断开"提示
    if (socket->state() == QAbstractSocket::ConnectedState
        || socket->state() == QAbstractSocket::ConnectingState) {
        socket->disconnectFromHost();
    }
}

void MainWindow::on_connectButton_clicked()
{
    // 槽里只做一件事：发起连接。结果由 connected / errorOccurred 信号通知
    socket->connectToHost(QHostAddress(ui->ipEdit->text()),
                          ui->portEdit->text().toShort());
}

void MainWindow::on_registerButton_clicked()
{
    const QString user = ui->userEdit->text().trimmed();
    const QString pass = ui->passEdit->text();
    if (user.isEmpty() || pass.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("注册"), QStringLiteral("用户名和密码不能为空"));
        return;
    }
    sendLine(QStringLiteral("REG|%1|%2").arg(user, pass));
}

void MainWindow::on_loginButton_clicked()
{
    const QString user = ui->userEdit->text().trimmed();
    const QString pass = ui->passEdit->text();
    if (user.isEmpty() || pass.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("登录"), QStringLiteral("用户名和密码不能为空"));
        return;
    }
    sendLine(QStringLiteral("LOG|%1|%2").arg(user, pass));
}

void MainWindow::on_logoutButton_clicked()
{
    sendLine(QStringLiteral("OUT"));
}

void MainWindow::on_addFriendButton_clicked()
{
    const QString name = ui->addFriendEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("添加好友"), QStringLiteral("请输入好友用户名"));
        return;
    }
    if (name == m_user) {
        QMessageBox::warning(this, QStringLiteral("添加好友"), QStringLiteral("不能添加自己"));
        return;
    }
    sendLine(QStringLiteral("ADD|%1").arg(name));
    ui->addFriendEdit->clear();
}

void MainWindow::on_agreeButton_clicked()
{
    QListWidgetItem *item = ui->requestList->currentItem();
    if (!item) {
        QMessageBox::warning(this, QStringLiteral("同意"), QStringLiteral("请先在好友请求列表中选中一项"));
        return;
    }
    sendLine(QStringLiteral("AGREE|%1").arg(item->text()));
    delete item;   // 服务器会回 AGREE_OK 确认
}

void MainWindow::on_refuseButton_clicked()
{
    QListWidgetItem *item = ui->requestList->currentItem();
    if (!item) {
        QMessageBox::warning(this, QStringLiteral("拒绝"), QStringLiteral("请先在好友请求列表中选中一项"));
        return;
    }
    sendLine(QStringLiteral("REFUSE|%1").arg(item->text()));
    delete item;
}

void MainWindow::on_sendButton_clicked()
{
    // 未登录禁止发送，服务器也会再拦一次（客户端拦截只为体验，安全靠服务器）
    if (m_user.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("发送失败"), QStringLiteral("请先登录"));
        return;
    }
    if (socket->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, QStringLiteral("发送失败"), QStringLiteral("请先连接服务器"));
        return;
    }

    // 私聊必须先在好友列表里选中收件人
    QListWidgetItem *friendItem = ui->friendList->currentItem();
    if (!friendItem) {
        QMessageBox::warning(this, QStringLiteral("发送失败"), QStringLiteral("请先在好友列表中选中一个好友"));
        return;
    }

    QString msg = ui->msgEdit->text();
    if (msg.isEmpty()) {
        return;
    }

    const QString friendName = friendItem->text();
    sendLine(QStringLiteral("PMSG|%1|%2").arg(friendName, msg));
    ui->msgEdit->clear();   // 发完清空输入框，方便连续发送
    // 自己发的消息也回显，才知道发给了谁、发了什么
    ui->recvEdit->append(QStringLiteral("(我→%1：%2)").arg(friendName, msg));
}

void MainWindow::sendLine(const QString &line)
{
    socket->write((line + QLatin1Char('\n')).toUtf8());
}

void MainWindow::onReadyRead()
{
    // TCP 是字节流没有"消息"边界，必须自己按 \n 拆帧；
    // 不满一整行的数据留在 m_buffer，等下次 readyRead 拼上（半包处理）
    m_buffer += socket->readAll();
    int idx;
    while ((idx = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(idx);
        m_buffer.remove(0, idx + 1);
        handleLine(QString::fromUtf8(line));
    }
}

void MainWindow::handleLine(const QString &line)
{
    if (line.isEmpty()) {
        return;
    }
    const QStringList parts = line.split(QLatin1Char('|'));
    const QString cmd = parts.value(0);

    if (cmd == QLatin1String("REG_OK")) {
        ui->recvEdit->append(QStringLiteral("[系统] 注册成功：%1，请登录").arg(parts.value(1)));
    } else if (cmd == QLatin1String("REG_FAIL")) {
        QMessageBox::warning(this, QStringLiteral("注册失败"), parts.value(1));
    } else if (cmd == QLatin1String("LOG_OK")) {
        m_user = parts.value(1);
        ui->recvEdit->append(QStringLiteral("[系统] 登录成功，欢迎 %1").arg(m_user));
        updateAuthUi();
    } else if (cmd == QLatin1String("LOG_FAIL")) {
        QMessageBox::warning(this, QStringLiteral("登录失败"), parts.value(1));
    } else if (cmd == QLatin1String("OUT_OK")) {
        ui->recvEdit->append(QStringLiteral("[系统] 已登出：%1").arg(m_user));
        m_user.clear();
        clearFriendUi();
        updateAuthUi();
    } else if (cmd == QLatin1String("ERR")) {
        QMessageBox::warning(this, QStringLiteral("服务器提示"), parts.value(1));
    } else if (cmd == QLatin1String("MSG")) {
        // 旧版群发消息：MSG|用户名|内容
        ui->recvEdit->append(QStringLiteral("(%1：%2)").arg(parts.value(1), parts.value(2)));

    // ---- 好友相关 ----
    } else if (cmd == QLatin1String("FRIENDS")) {
        // 登录后服务器推下来的好友列表：FRIENDS|a,b,c
        ui->friendList->clear();
        const QStringList friends = parts.value(1).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &f : friends) {
            ui->friendList->addItem(f);
        }
    } else if (cmd == QLatin1String("ADD_REQ")) {
        // 有人请求加好友，进请求列表，选中后点同意/拒绝
        ui->requestList->addItem(parts.value(1));
        ui->recvEdit->append(QStringLiteral("[好友请求] %1 请求加你为好友，请在请求列表中选中后点同意/拒绝")
                                 .arg(parts.value(1)));
    } else if (cmd == QLatin1String("ADD_OK")) {
        ui->friendList->addItem(parts.value(1));   // 对方同意，成为好友
        ui->recvEdit->append(QStringLiteral("[系统] %1 已同意你的好友请求").arg(parts.value(1)));
    } else if (cmd == QLatin1String("ADD_FAIL")) {
        QMessageBox::warning(this, QStringLiteral("添加好友失败"), parts.value(1));
    } else if (cmd == QLatin1String("ADD_REJECT")) {
        ui->recvEdit->append(QStringLiteral("[系统] %1 拒绝了你的好友请求").arg(parts.value(1)));
    } else if (cmd == QLatin1String("AGREE_OK")) {
        ui->friendList->addItem(parts.value(1));
        ui->recvEdit->append(QStringLiteral("[系统] 你和 %1 已成为好友，选中后即可私聊").arg(parts.value(1)));
    } else if (cmd == QLatin1String("REFUSE_OK")) {
        ui->recvEdit->append(QStringLiteral("[系统] 已拒绝 %1 的好友请求").arg(parts.value(1)));

    // ---- 私聊 ----
    } else if (cmd == QLatin1String("PMSG")) {
        // PMSG|发送者|内容
        ui->recvEdit->append(QStringLiteral("(%1：%2)").arg(parts.value(1), parts.mid(2).join(QLatin1Char('|'))));
    } else if (cmd == QLatin1String("PMSG_FAIL")) {
        QMessageBox::warning(this, QStringLiteral("发送失败"), parts.value(1));
    } else {
        ui->recvEdit->append(QStringLiteral("[原始] %1").arg(line));
    }
}

void MainWindow::updateAuthUi()
{
    const bool logged = !m_user.isEmpty();
    const bool connected = (socket->state() == QAbstractSocket::ConnectedState);

    ui->userEdit->setEnabled(!logged);
    ui->passEdit->setEnabled(!logged);
    ui->registerButton->setEnabled(connected && !logged);
    ui->loginButton->setEnabled(connected && !logged);
    ui->logoutButton->setEnabled(logged);
    ui->sendButton->setEnabled(logged && connected);
    ui->addFriendButton->setEnabled(logged && connected);
    ui->addFriendEdit->setEnabled(logged && connected);
    ui->agreeButton->setEnabled(logged && connected);
    ui->refuseButton->setEnabled(logged && connected);

    if (logged) {
        setWindowTitle(QStringLiteral("TCP客户端 - %1").arg(m_user));
    }
}

void MainWindow::clearFriendUi()
{
    ui->friendList->clear();
    ui->requestList->clear();
    ui->addFriendEdit->clear();
}
