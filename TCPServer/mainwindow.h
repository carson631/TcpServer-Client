#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QMessageBox>
#include <QList>
#include <QHash>
#include <QMap>

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
    void newClientHandler();
    void on_addUserButton_clicked();     // 管理员添加用户
    void on_deleteUserButton_clicked();  // 管理员删除选中用户

private:
    // 把 m_clients / m_loggedIn 里的内容重新刷到界面上（连接/断开/登录/登出时调用）
    void refreshClientList();
    void refreshUserList();
    void refreshUserDbList();               // 把 m_users 的账号名刷到 userList

    void loadUsers();                       // 启动时从 users.txt 加载账号
    void saveUser(const QString &user, const QString &hash);   // 注册成功追加写盘
    void saveUsers();                       // 用户被删除后整体重写 users.txt
    void loadFriends();                     // 启动时从 friends.txt 加载好友关系
    void saveFriends();                     // 好友关系变化后整体重写
    bool areFriends(const QString &a, const QString &b) const;
    void addFriendPair(const QString &a, const QString &b);    // 双向建立好友关系
    QTcpSocket *socketByUser(const QString &user) const;       // 用户名 -> 在线连接
    static QString hashPassword(const QString &password);      // SHA-256 hex

    void handleLine(QTcpSocket *client, const QString &line);  // 处理一条协议行
    void sendLine(QTcpSocket *client, const QString &line);    // 回一条协议行

    Ui::MainWindow *ui;
    QTcpServer *server;
    QList<QTcpSocket *> m_clients;                    // 当前所有在线连接（含未登录）
    QHash<QTcpSocket *, QString> m_loggedIn;          // 连接 -> 已登录用户名
    QHash<QTcpSocket *, QByteArray> m_buffers;        // 每个连接的半包缓冲
    QHash<QTcpSocket *, QStringList> m_pendingReq;    // 连接 -> 待处理的好友请求（发起者名）
    QMap<QString, QString> m_users;                   // 用户名 -> 密码哈希（users.txt）
    QMap<QString, QStringList> m_friends;             // 用户名 -> 好友列表（friends.txt）
};
#endif // MAINWINDOW_H
