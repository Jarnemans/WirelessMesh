#ifndef DIALOG_SENDER_H
#define DIALOG_SENDER_H

#include <QDialog>
#include <QSerialPort>
#include <QTimer>
#include <qlabel.h>
#include <qlistwidget.h>
#include <qprocess.h>
#include <qpushbutton.h>
#include <qtextedit.h>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QComboBox;
QT_END_NAMESPACE

class DialogSender : public QDialog
{
    Q_OBJECT

public:
    explicit DialogSender(QWidget *parent = nullptr);

private slots:
    void sendRequest();
    void readResponse();
    void processTimeout();
    void initializeSerialPort();
    void sendAdvertisement();
    void openSerialPort(int);
    void turnOnAllLeds();
    void onAddressDoubleClicked(QListWidgetItem *item);
    void onRefreshClicked();
    void handleBeaconResponse();

private:
    void setControlsEnabled(bool enable);
    void processError(const QString &error);
    void setLedStatus(const QString&, bool);


private:
    int m_transactionCount = 0;
    QLabel *m_serialPortLabel = nullptr;
    QComboBox *m_serialPortComboBox = nullptr;
    QLabel *m_waitResponseLabel = nullptr;
    QSpinBox *m_waitResponseSpinBox = nullptr;
    QLabel *m_requestLabel = nullptr;
    QLineEdit *m_requestLineEdit = nullptr;
    QLabel *m_trafficLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_runButton = nullptr;
    QPushButton *m_sendAdvertise = nullptr;
    QLabel *m_led1Label = nullptr;
    QLabel *m_led2Label = nullptr;
    QLabel *m_led3Label = nullptr;
    QListWidget *m_addressListWidget;
    QPushButton *m_turnOnAllLedsButton;
    QTextEdit *m_nodeDetailsTextBox; // To display node details
    QByteArray m_currentResponse;
    QPushButton *m_refreshButton = nullptr;



    QSerialPort m_serial;
    QByteArray m_response;
    QTimer m_timer;
    QProcess *m_process;
};

#endif // DIALOG_SENDER_H
