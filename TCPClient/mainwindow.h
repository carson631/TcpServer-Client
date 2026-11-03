#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTcpSocket>
#include <QHostAddress>
#include <QMessageBox>
#include <QCloseEvent>

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

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void on_cancelButton_clicked();
    void on_connectButton_clicked();
    void on_sendButton_clicked();
    void on_registerButton_clicked();
    void on_loginButton_clicked();
    void on_logoutButton_clicked();
    void on_addFriendButton_clicked();
    void on_agreeButton_clicked();
    void on_refuseButton_clicked();

private:
    void onReadyRead();                     // 收数据 + 拆行帧（处理半包）
    void handleLine(const QString &line);   // 处理一条完整的协议行
    void sendLine(const QString &line);     // 发一条协议行（自动补 \n）
    void updateAuthUi();                    // 按登录状态刷新按钮可用性
    void clearFriendUi();                   // 登出/断线时清空好友界面

    Ui::MainWindow *ui;
    QTcpSocket *socket;
    bool m_closing = false;
    QString m_user;        // 登录成功后保存用户名，空 = 未登录
    QByteArray m_buffer;   // TCP 半包缓冲
};
#endif // MAINWINDOW_H
