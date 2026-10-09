// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Loopback integration fixture: never captures a desktop or uses system login.
#include "RdpConnection.h"
#include "Server.h"
#include "VideoStream.h"
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPointer>
#include <QTimer>
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 4 && argc != 7)
        return 2;
    QList<QByteArray> encoded;
    if (argc == 7) {
        for (int i = 5; i < 7; ++i) {
            QFile file(QString::fromLocal8Bit(argv[i]));
            if (!file.open(QIODevice::ReadOnly))
                return 4;
            encoded.append(file.readAll());
        }
    }
    KRdp::Server server;
    const auto address = qEnvironmentVariable("KRDP_SYNTHETIC_ADDRESS");
    server.setAddress(address.isEmpty() ? QHostAddress(QHostAddress::LocalHost) : QHostAddress(address));
    server.setPort(QString::fromLocal8Bit(argv[1]).toUShort());
    server.setTlsCertificate(argv[2]);
    server.setTlsCertificateKey(argv[3]);
    server.setUsers({ { QStringLiteral("synthetic"), QStringLiteral("synthetic-test-only") } });
    server.setUsePAMAuthentication(false);
    QObject::connect(&server, &KRdp::Server::newConnectionCreated, &app, [&](KRdp::RdpConnection* connection) {
        auto* video = connection->videoStream();
        video->setCodecPreference(encoded.isEmpty() ? KRdp::CodecPreference::Progressive : KRdp::CodecPreference::Avc420);
        video->setMonitorLayout({ { QRect(0, 0, 256, 256), true }, { QRect(256, 0, 256, 256), false } });
        auto* timer = new QTimer(connection);
        timer->setInterval(50);
        QObject::connect(timer, &QTimer::timeout, video, [video, &encoded] {
            for (int index = 0; index < 2; ++index) {
                KRdp::VideoFrame frame;
                frame.size = QSize(256, 256);
                frame.monitorIndex = index;
                if (encoded.isEmpty()) {
                    frame.image = QImage(frame.size, QImage::Format_RGB32);
                    frame.image.fill(index == 0 ? QColor(30, 215, 96) : QColor(215, 30, 96));
                } else
                    frame.data = encoded[index];
                frame.damage = QRect(0, 0, 16, 16);
                frame.isKeyFrame = true;
                video->queueFrame(frame);
            }
        });
        timer->start();
    });
    if (!server.start())
        return 3;
    const int seconds = qBound(9, qEnvironmentVariableIntValue("KRDP_SYNTHETIC_SECONDS"), 300);
    QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
    return app.exec();
}
