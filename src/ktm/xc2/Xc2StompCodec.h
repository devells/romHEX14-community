#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>

#include "Xc2Models.h"

namespace ktm::xc2 {

struct Xc2StompFrame {
    QByteArray command;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
};

struct Xc2StompDecodeResult {
    QList<Xc2StompFrame> frames;
    QList<Xc2Error> errors;
    int heartbeats = 0;
};

class Xc2StompCodec final {
public:
    Xc2StompDecodeResult feed(const QByteArray &bytes);
    void reset();
    static QByteArray encode(const Xc2StompFrame &frame);

private:
    QByteArray m_buffer;
};

} // namespace ktm::xc2
