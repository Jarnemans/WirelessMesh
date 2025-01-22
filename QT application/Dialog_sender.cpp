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
#include <QRegularExpression>
#include <QRegularExpressionMatch>

#include <QScrollArea>

Node nodes(0,0);
bool Init = false;

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
    m_nodeDetailsTextBox(new QTextEdit),
    m_refreshButton(new QPushButton(tr("Refresh")))

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
    mainLayout->addWidget(m_refreshButton, 0, 3);


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
    connect(m_refreshButton, &QPushButton::clicked, this, &DialogSender::onRefreshClicked);

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

    if(!Init){
    if (!m_serial.isOpen()) {
        m_statusLabel->setText(tr("Status: Serial port not open."));
        return;
    }

    Node node;


    QStringList commands = {
        "mesh init\n",
        "mesh reset-local\n",
        "mesh prov uuid deadbeaf\n",
        "mesh cdb create\n",
        "mesh prov local 0 0x0001\n",
        "mesh cdb app-key-add 0 0\n"
    };


    node.setAddress("0x0001");
    node.setUuid("deadbeaf");
    m_addressListWidget->addItem(node.address());

    //  "mesh prov local 0 0x0001\n"
    m_nodeMap["0x0001"] = node;
    for (const QString &command : commands) {
        m_serial.write(command.toUtf8());
        m_serial.waitForBytesWritten(100);
        QThread::msleep(100);
    }

    m_statusLabel->setText(tr("Status: Mesh commands sent."));
    qDebug() << "Mesh commands sent to dongle.";
    Init = true;
    }
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
                // Extract the actual address (e.g., "0x0001")
                QRegularExpression regex(R"(0x[0-9A-Fa-f]+)");
                QRegularExpressionMatch match = regex.match(line);
                if (match.hasMatch()) {
                    QString address = match.captured(1); // Capture only the address (e.g., "0x0001")
                    qDebug() << "Address:" << address;

                    // Add the address to the list widget if it doesn't already exist
                  //  if (m_addressListWidget->findItems(address, Qt::MatchExactly).isEmpty()) {
                   //     m_addressListWidget->addItem(address);
                  //  }
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
    Node node;
    node = m_nodeMap[address];

    m_nodeDetailsTextBox->clear();
    // Display address immediately
    m_nodeDetailsTextBox->append(tr("Address: %1").arg(node.address()));
    m_nodeDetailsTextBox->append(tr("UUID: %1").arg(node.uuid()));

    m_statusLabel->setText(tr("Fetching UUID for address %1...").arg(address));
}

void DialogSender::onRefreshClicked()
{
    if (!m_serial.isOpen()) {
        m_statusLabel->setText(tr("Status: Serial port not open."));
        return;
    }

    // Send the "mesh prov beacon-listen on" command to discover nodes
    QString beaconListenCommand = "mesh prov beacon-listen on\n";
    m_serial.write(beaconListenCommand.toUtf8());
    m_serial.waitForBytesWritten(100);

    m_statusLabel->setText(tr("Refreshing and discovering nodes..."));
    // Wait for the response and parse it

    QTimer::singleShot(200, this, [this]() {
        connect(&m_serial, &QSerialPort::readyRead, this, &DialogSender::handleBeaconResponse, Qt::UniqueConnection);
    });
}


void DialogSender::handleBeaconResponse()
{
    QByteArray data = m_serial.readAll();
    QString response = QString::fromUtf8(data).trimmed();

    qDebug() << "Received beacon response:" << response;

    // Regular expression to extract UUID from the response
    QRegularExpression uuidRegex(R"(PB-GATT UUID\s([0-9a-fA-F]{32}))");
    QRegularExpressionMatch match = uuidRegex.match(response);

    if (match.hasMatch()) {
        QString uuid = match.captured(1);  // Extract the UUID
        qDebug() << "Found UUID:" << uuid;

        // Remove trailing zeros from the UUID
        uuid = uuid.trimmed();
        uuid.remove(QRegularExpression("0+$")); // Remove trailing zeros using QRegularExpression

        qDebug() << "UUID after trimming zeros:" << uuid;

        // Check if UUID is already provisioned
        if (m_provisionedUUIDs.contains(uuid)) {
            qDebug() << "UUID already provisioned. Skipping...";
            m_statusLabel->setText(tr("Node with UUID %1 is already provisioned.").arg(uuid));
            return; // Skip further processing for this UUID
        }

        // Assign the next available unicast address
        QString uniqueAddress = QString("0x%1").arg(m_nextUnicastAddress, 4, 16, QChar('0'));

        // Increment the address for the next node
        m_nextUnicastAddress++;

        // Provision the node using the remote GATT command
        QString provisionCommand = QString("mesh prov remote-gatt %1 0 %2 30\n").arg(uuid).arg(uniqueAddress);
        m_serial.write(provisionCommand.toUtf8());
        qDebug() << "Sending REMOTE GATT";
        m_serial.waitForBytesWritten(100);

        QTimer::singleShot(2000, this, [this, uniqueAddress]() {
        qDebug() << "RAAAAAAHHHHHHHHHHHHHHHHHHHHHH \n\n\n";
        QString Command1 = QString("mesh target dst %1\n").arg(uniqueAddress);
        m_serial.write(Command1.toUtf8());
        m_serial.waitForBytesWritten(100);
        m_serial.waitForReadyRead(50);

        m_serial.write("mesh models cfg appkey add 0 0\n");
        m_serial.waitForBytesWritten(100);
        m_serial.waitForReadyRead(50);

        QString Command2 = QString("mesh models cfg model app-bind %1 0 0x1001\n").arg(uniqueAddress);
        m_serial.write(Command2.toUtf8());
        m_serial.waitForBytesWritten(100);
        m_serial.waitForReadyRead(50);

        QString Command3 = QString("mesh models cfg model app-bind %1 0 0x1000\n").arg(uniqueAddress);
        m_serial.write(Command3.toUtf8());
        m_serial.waitForBytesWritten(100);
        m_serial.waitForReadyRead(50);
        });

        // Add the UUID to the provisioned set
        m_provisionedUUIDs.insert(uuid);

        // Check if the address is already in the map
        if (m_nodeMap.find(uniqueAddress) == m_nodeMap.end()) {
            // Create a new node and add it to the map
            Node node(uniqueAddress, uuid);
            m_nodeMap[uniqueAddress] = node;

            // Optionally add the address to the GUI list
            m_addressListWidget->addItem(uniqueAddress);
        } else {
            qDebug() << "Node with address" << uniqueAddress << "is already provisioned.";
        }

        m_statusLabel->setText(tr("Node provisioned with UUID %1 at address %2.").arg(uuid).arg(uniqueAddress));




    } else {
        m_statusLabel->setText(tr("No nodes discovered."));
    }

    // Optionally disable beacon listen after processing
    // m_serial.write("mesh prov beacon-listen off\n");
    // m_serial.waitForBytesWritten(100);

    // Disconnect the handler after processing the response
    // disconnect(&m_serial, &QSerialPort::readyRead, nullptr, nullptr);
}





