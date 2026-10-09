// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
// SPDX-FileCopyrightText: 2026 David Edmundson <kde@davidedmundson.co.uk>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "CaptureStream.h"
#include "krdp_logging.h"
#include <QRegion>
#include <pipewire/pipewire.h>
#include <unistd.h>
namespace KRdp
{
CaptureStream::CaptureStream(QObject* parent)
    : QObject(parent)
{
    makeEncoder();
}
CaptureStream::~CaptureStream()
{
    stop();
    if (m_fd >= 0)
        ::close(m_fd);
}
void CaptureStream::makeEncoder()
{
    m_encoded = std::make_unique<PipeWireEncodedStream>();
    m_encoded->setEncodingPreference(m_preference);
    m_encoded->setColorRange(m_range);
    m_encoded->setEncoder(m_encoder);
    m_encoded->setQuality(m_quality);
    m_encoded->setMaxFramerate(m_rate, 1);
    m_encoded->setMaxPendingFrames(m_pending);
    connect(m_encoded.get(), &PipeWireEncodedStream::activeChanged, this, &CaptureStream::activeChanged);
    connect(m_encoded.get(), &PipeWireEncodedStream::sizeChanged, this, &CaptureStream::sizeChanged);
    connect(m_encoded.get(), &PipeWireEncodedStream::cursorChanged, this, &CaptureStream::cursorChanged);
    connect(m_encoded.get(), &PipeWireEncodedStream::newPacket, this, &CaptureStream::newPacket);
    m_encoded->setNodeId(m_nodeId);
    if (m_fd >= 0)
        m_encoded->setFd(::dup(m_fd));
}
void CaptureStream::setProgressive(bool progressive)
{
    if (m_progressive == progressive)
        return;
    const bool requested = m_requested;
    stop();
    m_encoded.reset();
    m_progressive = progressive;
    if (!progressive)
        makeEncoder();
    if (requested)
        start();
}
void CaptureStream::setReady(bool ready)
{
    m_ready = ready;
    if (ready && m_requested)
        start();
}
void CaptureStream::setNodeId(uint id)
{
    m_nodeId = id;
    if (m_encoded)
        m_encoded->setNodeId(id);
}
uint CaptureStream::nodeId() const { return m_encoded ? m_encoded->nodeId() : m_source ? m_nodeId : 0; }
void CaptureStream::setFd(uint fd)
{
    if (m_fd >= 0)
        ::close(m_fd);
    m_fd = int(fd);
    if (m_encoded && m_fd >= 0)
        m_encoded->setFd(::dup(m_fd));
}
bool CaptureStream::isActive() const
{
    return m_encoded ? m_encoded->isActive() : m_source && m_source->state() == PW_STREAM_STATE_STREAMING;
}
void CaptureStream::start()
{
    m_requested = true;
    if (!m_nodeId || !m_ready)
        return;
    if (m_encoded) {
        m_encoded->start();
        return;
    }
    if (m_source) {
        m_source->setActive(true);
        return;
    }
    m_source = std::make_unique<PipeWireSourceStream>();
    m_source->setAllowDmaBuf(true);
    m_source->setDamageEnabled(true);
    m_source->setMaxFramerate({ m_rate, 1 });
    connect(m_source.get(), &PipeWireSourceStream::stateChanged, this,
        [this](pw_stream_state, pw_stream_state) { Q_EMIT activeChanged(isActive()); });
    connect(m_source.get(), &PipeWireSourceStream::streamParametersChanged, this, [this] { Q_EMIT sizeChanged(m_source->size()); });
    const auto generation = ++m_generation;
    connect(
        m_source.get(), &PipeWireSourceStream::frameReceived, this,
        [this, generation](const PipeWireFrame& frame) {
            if (generation != m_generation || !m_requested || !m_source)
                return;
            if (frame.cursor)
                Q_EMIT cursorChanged(*frame.cursor);
            QImage image;
            if (frame.dataFrame)
                image = frame.dataFrame->toImage().convertToFormat(QImage::Format_RGB32).copy();
            else if (frame.dmabuf) {
                QImage downloaded(QSize(frame.dmabuf->width, frame.dmabuf->height), QImage::Format_RGBA8888_Premultiplied);
                if (!m_dmaBuf.downloadFrame(downloaded, frame)) {
                    qCWarning(KRDP) << "Failed to download progressive DMA-BUF frame";
                    return;
                }
                image = downloaded.convertToFormat(QImage::Format_RGB32);
            }
            if (image.isNull())
                return;
            Q_EMIT sizeChanged(image.size());
            Q_EMIT rasterFrame(image, frame.damage.value_or(QRegion(QRect(QPoint(0, 0), image.size()))));
        },
        Qt::QueuedConnection);
    if (!m_source->createStream(m_nodeId, m_fd >= 0 ? ::dup(m_fd) : -1)) {
        qCWarning(KRDP) << "Could not create progressive source stream" << m_source->error();
        m_source.reset();
        return;
    }
    m_source->setActive(true);
}
void CaptureStream::stop()
{
    m_requested = false;
    ++m_generation;
    if (m_encoded)
        m_encoded->stop();
    if (m_source) {
        m_source->setActive(false);
        m_source.reset();
        Q_EMIT activeChanged(false);
    }
}
void CaptureStream::setQuality(quint8 q)
{
    m_quality = q;
    if (m_encoded)
        m_encoded->setQuality(q);
}
void CaptureStream::setMaxFramerate(const Fraction& r) { setMaxFramerate(r.numerator, r.denominator); }
void CaptureStream::setMaxFramerate(quint32 n, quint32 d)
{
    m_rate = std::max(1u, n / std::max(1u, d));
    if (m_encoded)
        m_encoded->setMaxFramerate(n, d);
    if (m_source)
        m_source->setMaxFramerate({ n, d });
}
void CaptureStream::setMaxPendingFrames(int n)
{
    m_pending = n;
    if (m_encoded)
        m_encoded->setMaxPendingFrames(n);
}
void CaptureStream::setColorRange(ColorRange r)
{
    m_range = r;
    if (m_encoded)
        m_encoded->setColorRange(r);
}
void CaptureStream::setEncoder(Encoder e)
{
    m_encoder = e;
    if (m_encoded)
        m_encoded->setEncoder(e);
}
void CaptureStream::setEncodingPreference(EncodingPreference p)
{
    m_preference = p;
    if (m_encoded)
        m_encoded->setEncodingPreference(p);
}
QList<CaptureStream::Encoder> CaptureStream::suggestedEncoders() const
{
    return m_encoded ? m_encoded->suggestedEncoders() : QList<Encoder> { };
}
}
#include "moc_CaptureStream.cpp"
