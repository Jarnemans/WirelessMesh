#include "dialog_sender.h"

#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QSpinBox>
#include <QDebug>
#include <QThread>
#include <QScrollArea>

#include <QScrollArea>

// Inside the DialogSender constructor
DialogSender::DialogSender(QWidget *parent) :
    QDialog(parent),
    m_transactionCount(0),
    m_serialPortLabel(new QLabel(tr("Serial port:"))),
    m_serialPortComboBox(new QComboBox),
    m_waitResponseLabel(new QLabel(tr("Wait response, msec:"))),
    m_waitResponseSpinBox(new QSpinBox),
    m_requestLabel(new QLabel(tr("Request:"))),
    m_requestLineEdit(new QLineEdit(tr("Who are you?"))),
    m_trafficLabel(new QLabel(tr("No traffic."))),
    m_statusLabel(new QLabel(tr("Status: Not running."))),
    m_runButton(new QPushButton(tr("Start"))),
    m_sendAdvertise(new QPushButton(tr("Initialize provisioner"))),
    m_addressListWidget(new QListWidget), // New list widget
    m_turnOnAllLedsButton(new QPushButton(tr("Turn On All LEDs"))),
    m_nodeDetailsTextBox(new QTextEdit)

{
    // Set up m_trafficLabel to support word wrapping
    m_nodeDetailsTextBox->setReadOnly(true); // Make it read-only
    m_trafficLabel->setWordWrap(true);

    // Create a QScrollArea for m_trafficLabel
    QScrollArea *trafficScrollArea = new QScrollArea;
    trafficScrollArea->setWidget(m_trafficLabel);
    trafficScrollArea->setWidgetResizable(true);
    trafficScrollArea->setMinimumHeight(100); // Adjust height as needed

    // Configure the rest of the UI
    const auto infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : infos) {
        m_serialPortComboBox->addItem(info.portName());
    }

    m_waitResponseSpinBox->setRange(0, 10000);
    m_waitResponseSpinBox->setValue(100);

    auto mainLayout = new QGridLayout;
    mainLayout->addWidget(m_serialPortLabel, 0, 0);
    mainLayout->addWidget(m_serialPortComboBox, 0, 1);
    mainLayout->addWidget(m_waitResponseLabel, 1, 0);
    mainLayout->addWidget(m_waitResponseSpinBox, 1, 1);
    mainLayout->addWidget(m_runButton, 0, 2, 2, 1);
    mainLayout->addWidget(m_requestLabel, 2, 0);
    mainLayout->addWidget(m_requestLineEdit, 2, 1, 1, 3);
    mainLayout->addWidget(new QLabel(tr("Traffic:")), 3, 0, 1, 5);
    mainLayout->addWidget(trafficScrollArea, 4, 0, 1, 5); // Replace m_trafficLabel with scrollable area
    mainLayout->addWidget(m_statusLabel, 5, 0, 1, 5);
    mainLayout->addWidget(m_sendAdvertise, 6, 0, 1, 5);
    mainLayout->addWidget(new QLabel(tr("Received Addresses:")), 7, 0, 1, 5); // Label for the list
    mainLayout->addWidget(m_addressListWidget, 8, 0, 1, 5); // List widget
    mainLayout->addWidget(m_turnOnAllLedsButton, 9, 0, 1, 5);
    mainLayout->addWidget(new QLabel(tr("Node Details:")), 10, 0, 1, 5);
    mainLayout->addWidget(m_nodeDetailsTextBox, 11, 0, 1, 5);


    setLayout(mainLayout);
    setWindowTitle(tr("Sender"));
    m_serialPortComboBox->setFocus();

    m_timer.setSingleShot(true);

    connect(m_runButton, &QPushButton::clicked, this, &DialogSender::sendRequest);
    connect(&m_serial, &QSerialPort::readyRead, this, &DialogSender::readResponse);
    connect(&m_timer, &QTimer::timeout, this, &DialogSender::processTimeout);
    connect(m_sendAdvertise, &QPushButton::clicked, this, &DialogSender::sendAdvertisement);
    connect(m_turnOnAllLedsButton, &QPushButton::clicked, this, &DialogSender::turnOnAllLeds);
    connect(m_addressListWidget, &QListWidget::itemDoubleClicked, this, &DialogSender::onAddressDoubleClicked);
    connect(m_serialPortComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &DialogSender::openSerialPort);

    initializeSerialPort();
}


void DialogSender::initializeSerialPort()
{
    if (m_serialPortComboBox->count() > 0) {
        QString portName = m_serialPortComboBox->currentText();
        m_serial.setPortName(portName);

        if (m_serial.open(QIODevice::ReadWrite)) {
            qDebug() << "Serial port opened successfully: " << m_serial.portName();

            m_serial.setDataTerminalReady(false);
            m_serial.setRequestToSend(false);
            QThread::msleep(200); // Wait for reset signal
            m_serial.setDataTerminalReady(true);
            m_serial.setRequestToSend(true);

            m_serial.setBaudRate(QSerialPort::Baud115200);
            m_serial.setDataBits(QSerialPort::Data8);
            m_serial.setParity(QSerialPort::NoParity);
            m_serial.setStopBits(QSerialPort::OneStop);
            m_serial.setFlowControl(QSerialPort::NoFlowControl);

            QThread::msleep(500); // Delay to ensure Zephyr is ready
            m_serial.write("\n");
            m_serial.waitForBytesWritten(100);

            m_statusLabel->setText(tr("Status: Initialized, connected to port %1.").arg(portName));
        } else {
            qDebug() << "Failed to open serial port: " << m_serial.portName();
            m_statusLabel->setText(tr("Status: Failed to initialize port %1.").arg(portName));
        }
    } else {
        qDebug() << "No available serial ports.";
    }
}

void DialogSender::sendAdvertisement()
{
    if (!m_serial.isOpen()) {
        m_statusLabel->setText(tr("Status: Serial port not open."));
        return;
    }

    QStringList commands = {
        "mesh init\n",
        "mesh reset-local\n",
        "mesh prov uuid deadbeaf\n",
        "mesh cdb create\n",
        "mesh prov local 0 0x0001\n",
        "mesh cdb app-key-add 0 0\n"
    };
    //  "mesh prov local 0 0x0001\n"

    for (const QString &command : commands) {
        m_serial.write(command.toUtf8());
        m_serial.waitForBytesWritten(100);
        QThread::msleep(100);
    }
    m_statusLabel->setText(tr("Status: Mesh commands sent."));
    qDebug() << "Mesh commands sent to dongle.";
}

void DialogSender::openSerialPort(int index)
{
    if (index == -1) return;

    QString portName = m_serialPortComboBox->itemText(index);
    if (m_serial.portName() != portName) {
        m_serial.close();
        m_serial.setPortName(portName);
        initializeSerialPort();
    }
}

void DialogSender::sendRequest()
{
    if (m_serial.portName() != m_serialPortComboBox->currentText()) {
        m_serial.close();
        m_serial.setPortName(m_serialPortComboBox->currentText());

        if (!m_serial.open(QIODevice::ReadWrite)) {
            processError(tr("Can't open %1, error code %2")
                             .arg(m_serial.portName()).arg(m_serial.error()));
            return;
        }
    }

    setControlsEnabled(false);
    m_statusLabel->setText(tr("Status: Running, connected to port %1.")
                               .arg(m_serialPortComboBox->currentText()));

    m_serial.write((m_requestLineEdit->text() + "\r\n").toUtf8());
    m_timer.start(m_waitResponseSpinBox->value());
}

void DialogSender::readResponse()
{
    // Wait until there's enough data to read
    if (!m_serial.waitForReadyRead(50)) {  // Wait up to 50ms for data
        qDebug() << "Timeout waiting for data.";
        return;
    }

    QByteArray newData = m_serial.readAll();
    qDebug() << "Data received:" << newData;

    // Add the new data to the existing response buffer
    m_response.append(newData);

    // Convert the byte array to a string for easier processing
    QString responseString = QString::fromUtf8(m_response);

    // Check if the response is non-empty
    if (!responseString.isEmpty()) {
        // Split the response into lines
        QStringList lines = responseString.split('\n', Qt::SkipEmptyParts);

        // Update the traffic label with the received traffic
        m_trafficLabel->setText(tr("Traffic (Live):\n%1").arg(responseString));

        // Process the lines for any addresses
        for (const QString &line : lines) {
            if (line.contains("Received message from") || line.contains("address") || line.contains("Addr:")) {
                // Extract address and value from the line
                QRegularExpression regex(R"((?:Received message from|address|Addr:)\s?0x[0-9A-Fa-f]+)");
                QRegularExpressionMatch match = regex.match(line);
                if (match.hasMatch()) {
                    QString address = match.captured(0); // Capture the full address (e.g., "0x12345")
                    qDebug() << "Address:" << address;

                    // Add the address to the list widget if it doesn't already exist
                    if (m_addressListWidget->findItems(address, Qt::MatchExactly).isEmpty()) {
                        m_addressListWidget->addItem(address);
                    }
                }
            }
        }
    }

    // Reset the buffer to collect the next response
    m_response.clear();
}



void DialogSender::setLedStatus(const QString &address, bool isOn)
{
    QString status = isOn ? "on" : "off";
    qDebug() << "Setting LED for address" << address << "to" << status;

    // Update the UI or internal state for the LED
    m_statusLabel->setText(tr("LED for %1 set to: %2").arg(address).arg(status));
}

void DialogSender::processTimeout()
{
    QString responseString = QString::fromUtf8(m_response);
    QStringList lines = responseString.split('\n', Qt::SkipEmptyParts);

    if (!lines.isEmpty() && !lines.last().endsWith('\r')) {
        lines.removeLast();
    }

    QString filteredResponse = lines.join('\n');

    setControlsEnabled(true);
    m_trafficLabel->setText(tr("Traffic, transaction #%1:""\n""\r-request: %2""\n""\r-response: %3")
                                .arg(++m_transactionCount)
                                .arg(m_requestLineEdit->text())
                                .arg(filteredResponse));

    qDebug() << "Processed response:" << filteredResponse;
    m_response.clear();
}

void DialogSender::processError(const QString &error)
{
    setControlsEnabled(true);
    m_statusLabel->setText(tr("Status: Not running, %1.").arg(error));
    m_trafficLabel->setText(tr("No traffic."));
}

void DialogSender::setControlsEnabled(bool enable)
{
    m_runButton->setEnabled(enable);
    m_serialPortComboBox->setEnabled(enable);
    m_waitResponseSpinBox->setEnabled(enable);
    m_requestLineEdit->setEnabled(enable);
}

void DialogSender::turnOnAllLeds()
{
    if (!m_serial.isOpen()) {
        m_statusLabel->setText(tr("Status: Serial port not open."));
        return;
    }

    // Placeholder-opdracht om alle LEDs aan te zetten
    QStringList commands = {
        "sendto (adress) leds 1\n",
    };

    int itemCount = m_addressListWidget->count(); // Aantal items in de lijst
    for (int i = 0; i < itemCount; ++i) {
        // Haal het adres op uit de lijst
        QListWidgetItem *item = m_addressListWidget->item(i);
        if (item) {
            QString address = item->text(); // Het adres uit de lijst

            // Voeg het adres in op de plek van "(adress)"
            for (const QString &command : commands) {
                QString commandWithAddress = command;
                commandWithAddress.replace("(adress)", address); // Vervang de placeholder
                m_serial.write(commandWithAddress.toUtf8());
                m_serial.waitForBytesWritten(100); // Wacht tot het commando is geschreven
            }
        }
    }
    m_statusLabel->setText(tr("Status: All LEDs turned on."));
    qDebug() << "Command sent to turn on all LEDs.";
}


void DialogSender::onAddressDoubleClicked(QListWidgetItem *item)
{
    if (!m_serial.isOpen()) {
        m_statusLabel->setText(tr("Status: Serial port not open."));
        return;
    }

    QString address = item->text(); // Get the selected address
    qDebug() << "Double-clicked on address:" << address;

    // Command to fetch UUID
    QString uuidCommand = "mesh prov uuid\n";

    // Clear previous node details
    m_nodeDetailsTextBox->clear();

    // Display address immediately
    m_nodeDetailsTextBox->append(tr("Address: %1").arg(address));

    // Send command to fetch UUID
    m_serial.write(uuidCommand.toUtf8());
    m_serial.waitForBytesWritten(100);

    // Handle UUID response
    connect(&m_serial, &QSerialPort::readyRead, this, [=]() {
        QByteArray data = m_serial.readAll();
        QString response = QString::fromUtf8(data).trimmed(); // Handle raw response
        qDebug() << "Received UUID response:" << response;

        // Extract UUID from the response
        QRegularExpression uuidRegex(R"(UUID:\s*([0-9a-fA-F\-]+))");
        QRegularExpressionMatch match = uuidRegex.match(response);
        if (match.hasMatch()) {
            QString uuid = match.captured(1); // Extract UUID
            m_nodeDetailsTextBox->append(tr("UUID: %1").arg(uuid));
        } else {
            m_nodeDetailsTextBox->append(tr("UUID: Not found in response."));
        }

        // Disconnect this temporary handler
        disconnect(&m_serial, &QSerialPort::readyRead, nullptr, nullptr);
    });

    m_statusLabel->setText(tr("Fetching UUID for address %1...").arg(address));
}


