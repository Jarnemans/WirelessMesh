#include "node.h"

Node::Node(const QString &address, const QString &uuid)
    : m_address(address), m_uuid(uuid) {}

QString Node::address() const {
    return m_address;
}

void Node::setAddress(const QString &address) {
    m_address = address;
}

QString Node::uuid() const {
    return m_uuid;
}

void Node::setUuid(const QString &uuid) {
    m_uuid = uuid;
}
