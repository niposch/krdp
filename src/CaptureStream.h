// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-FileCopyrightText: 2026 David Edmundson <kde@davidedmundson.co.uk>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#pragma once
#include <DmaBufHandler>
#include <PipeWireEncodedStream>
#include <PipeWireSourceStream>
#include <QImage>
#include <QObject>
#include <QRegion>
#include <memory>
namespace KRdp
{
// One monitor's encoder/source, owned by VideoStream in server sessions.
// The facade keeps capture recovery and input readiness independent of codec.
class CaptureStream : public QObject
{
    Q_OBJECT
public:
    using ColorRange = PipeWireBaseEncodedStream::ColorRange;
    using Encoder = PipeWireBaseEncodedStream::Encoder;
    using EncodingPreference = PipeWireBaseEncodedStream::EncodingPreference;
    explicit CaptureStream(QObject* parent = nullptr);
    ~CaptureStream() override;
    void setProgressive(bool progressive);
    void setReady(bool ready);
    bool progressive() const { return m_progressive; }
    void setNodeId(uint id);
    uint nodeId() const;
    void setFd(uint fd);
    bool isActive() const;
    void start();
    void stop();
    void setQuality(quint8 quality);
    void setMaxFramerate(const Fraction& rate);
    void setMaxFramerate(quint32 numerator, quint32 denominator = 1);
    void setMaxPendingFrames(int count);
    void setColorRange(ColorRange range);
    void setEncoder(Encoder encoder);
    void setEncodingPreference(EncodingPreference preference);
    QList<Encoder> suggestedEncoders() const;
    Q_SIGNAL void activeChanged(bool active);
    Q_SIGNAL void sizeChanged(const QSize& size);
    Q_SIGNAL void cursorChanged(const PipeWireCursor& cursor);
    Q_SIGNAL void newPacket(const PipeWireEncodedStream::Packet& packet);
    Q_SIGNAL void rasterFrame(const QImage& image, const QRegion& damage);

private:
    void makeEncoder();
    std::unique_ptr<PipeWireEncodedStream> m_encoded;
    std::unique_ptr<PipeWireSourceStream> m_source;
    DmaBufHandler m_dmaBuf;
    uint m_nodeId = 0;
    int m_fd = -1;
    bool m_progressive = false;
    bool m_requested = false;
    bool m_ready = true; // Standalone capture examples do not negotiate RDP.
    quint64 m_generation = 0;
    quint8 m_quality = 80;
    quint32 m_rate = 60;
    int m_pending = 60;
    ColorRange m_range = ColorRange::Full;
    Encoder m_encoder = PipeWireEncodedStream::H264Baseline;
    EncodingPreference m_preference = PipeWireBaseEncodedStream::Speed;
};
}
