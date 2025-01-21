#ifndef NODE_H
#define NODE_H

#include <QString>

class Node {
public:
    Node() = default;
    Node(const QString &address, const QString &uuid = "");

    QString address() const;
    void setAddress(const QString &address);

    QString uuid() const;
    void setUuid(const QString &uuid);

private:
    QString m_address;
    QString m_uuid;
};

#endif // NODE_H
