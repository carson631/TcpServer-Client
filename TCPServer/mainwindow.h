#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QStringList>

class MyTcpServer;
class MyThread;
class QSqlQueryModel;
class QTimer;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    // ---- 来自 MyThread 的自定义信号（跨线程，自动走队列连接）----
    void onNewClient(qintptr socketDescriptor);
    void onClientConnected(MyThread *thread, const QString &peer);
    void onClientDisconnected(MyThread *thread, const QString &peer);
    void onClientLine(MyThread *thread, const QString &line);

    // ---- 界面按钮 ----
    void on_addUserButton_clicked();      // 管理员添加用户
    void on_deleteUserButton_clicked();   // 管理员删除选中用户
    void on_queryUserButton_clicked();    // 查询用户 -> QTableView
    void on_connectDbButton_clicked();    // 连接 MySQL

private:
    void initDatabase();                  // 读配置并自动连接
    void applyDbState(bool ok, const QString &detail);
    void setupUserTable();                // QTableView + 查询模型
    void importLegacyFiles();             // users.txt / friends.txt 迁移进数据库

    void refreshClientList();
    void refreshOnlineList();
    void refreshUserTable(const QString &keyword = QString());
    void scheduleTableRefresh();          // 合并短时间内的多次刷新，避免每条消息都重查

    MyThread *threadByUser(const QString &user) const;
    static QString hashPassword(const QString &password);
    void handleLine(MyThread *client, const QString &line);   // 解析一条协议行
    void sendLine(MyThread *client, const QString &line);
    void logMessage(const QString &msg);
    void appendLogLine(const QString &line);   // 写界面 + 可选写文件
    bool dbReady();

    Ui::MainWindow *ui;
    MyTcpServer *m_server = nullptr;
    QList<MyThread *> m_clients;                 // 所有在线连接（含未登录）
    QHash<MyThread *, QString> m_loggedIn;       // 连接 -> 已登录用户名
    QHash<MyThread *, QString> m_peers;          // 连接 -> "ip:port"
    QHash<MyThread *, QStringList> m_pendingReq; // 连接 -> 待处理的好友请求发起者
    QSqlQueryModel *m_userModel = nullptr;
    QTimer *m_tableTimer = nullptr;
    QTimer *m_dbRetryTimer = nullptr;
    int m_dbRetry = 0;                    // 启动时连不上数据库的自动重试计数
};

#endif // MAINWINDOW_H
