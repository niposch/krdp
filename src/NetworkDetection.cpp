// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// This file is roughly based on grd-rdp-network-autodetection.c from Gnome
// Remote Desktop which is:
//
// SPDX-FileCopyrightText: 2021 Pascal Nowack
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "NetworkDetection.h"

#include <ranges>

#include <atomic>

#include <QQueue>
#include <QTimer>

#include "PeerContext_p.h"
#include "RdpConnection.h"

#include "krdp_logging.h"

namespace KRdp
{

namespace clk = std::chrono;

constexpr auto rttUpdateInterval = clk::milliseconds(70);
constexpr auto rttAverageInterval = clk::milliseconds(500);
constexpr auto networkResultInterval = clk::seconds(1);
constexpr double bandwidthSmoothingWeight = 0.5;
// Measure real traffic over a scheduled window, on the peer thread only.
// Per-frame windows are too short to produce meaningful goodput estimates.
constexpr auto bandwidthMeasureDuration = clk::milliseconds(500);
constexpr auto bandwidthMeasureInterval = clk::seconds(2);
// A sample shorter than this or carrying fewer bytes than this is too noisy
// to trust (see onBandwidthMeasureResults()).
constexpr uint32_t minimumBandwidthSampleDuration = 100;
constexpr uint32_t minimumBandwidthSampleBytes = 4096;

BOOL rttMeasureResponse(rdpAutoDetect *rdpAutodetect, RDP_TRANSPORT_TYPE, uint16_t sequence)
{
    auto context = reinterpret_cast<PeerContext *>(rdpAutodetect->context);
    if (context->networkDetection->onRttMeasureResponse(sequence)) {
        return TRUE;
    }
    return FALSE;
}

BOOL bwMeasureResults(rdpAutoDetect *rdpAutodetect, RDP_TRANSPORT_TYPE, uint16_t sequence, uint16_t responseType, uint32_t timeDelta, uint32_t byteCount)
{
    auto context = reinterpret_cast<PeerContext *>(rdpAutodetect->context);
    if (context->networkDetection->onBandwidthMeasureResults(timeDelta, byteCount, sequence, responseType)) {
        return TRUE;
    }
    return FALSE;
}

FREERDP_AUTODETECT_STATE beginNetworkProbe(rdpAutoDetect *autodetect)
{
    return reinterpret_cast<PeerContext *>(autodetect->context)->networkDetection->beginConnectTimeProbe();
}

FREERDP_AUTODETECT_STATE progressNetworkProbe(rdpAutoDetect *autodetect)
{
    return reinterpret_cast<PeerContext *>(autodetect->context)->networkDetection->progressConnectTimeProbe();
}

BOOL netCharSync(rdpAutoDetect *autodetect, RDP_TRANSPORT_TYPE, uint16_t, uint32_t bandwidth, uint32_t rtt)
{
    return reinterpret_cast<PeerContext *>(autodetect->context)->networkDetection->onNetworkCharacteristicsSync(bandwidth, rtt);
}

struct RTTMeasurement {
    clk::system_clock::time_point measurementTime;
    clk::system_clock::duration roundTripTime;
};

class NetworkDetection::Private
{
public:
    uint16_t nextSequenceNumber();

    RdpConnection *session = nullptr;
    rdpAutoDetect *rdpAutodetect = nullptr;

    State state = State::None;
    enum class ProbeStage { None, Rtt, Bandwidth, Complete };
    ProbeStage probeStage = ProbeStage::None;
    clk::steady_clock::time_point probeStarted;
    uint32_t probeBytes = 0;
    uint16_t probeSequence = 0;
    bool probeRoundComplete = false;
    uint32_t probeTimeDelta = 0;
    uint32_t probeByteCount = 0;
    bool hasProbedBandwidth = false;

    uint16_t sequenceNumber = 0;

    // Highest observed goodput is a lower bound on available bandwidth. Idle
    // desktop traffic must not overwrite it with the application's low demand.
    uint32_t reportedBandwidthKbps = 0;

    double smoothedBandwidthBps = 0.0;
    bool hasSmoothedBandwidth = false;
    std::atomic<uint32_t> averageBandwidthBps{0};
    std::atomic<int> validSamples{0};

    // Scheduled measurement and request timeout bookkeeping.
    clk::system_clock::time_point bandwidthMeasureStartTime;
    clk::system_clock::time_point lastBandwidthMeasureStart;

    bool rttEnabled = false;
    clk::system_clock::time_point lastRttUpdate;
    QHash<uint32_t, clk::system_clock::time_point> rttRequests;
    std::vector<RTTMeasurement> rttMeasurements;

    clk::system_clock::duration minimumRtt;
    clk::system_clock::duration averageRtt;

    clk::system_clock::time_point lastNetworkResult;
};

NetworkDetection::NetworkDetection(RdpConnection *session)
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
    d->session = session;
}

NetworkDetection::~NetworkDetection() = default;

std::chrono::system_clock::duration NetworkDetection::minimumRTT() const
{
    return d->minimumRtt;
}

std::chrono::system_clock::duration NetworkDetection::averageRTT() const
{
    return d->averageRtt;
}

quint32 NetworkDetection::bandwidth() const
{
    // Compute in 64 bits before narrowing back to kbit/s: averageBandwidthBps
    // is bytes/s, and multiplying by 8 in 32 bits overflows above ~4.3 Gbit/s.
    return static_cast<quint32>(static_cast<quint64>(d->averageBandwidthBps.load()) * 8 / 1000);
}

int NetworkDetection::validBandwidthSamples() const
{
    return d->validSamples.load();
}

void NetworkDetection::initialize()
{
    d->rdpAutodetect = d->session->rdpPeerContext()->autodetect;
    d->rdpAutodetect->RTTMeasureResponse = rttMeasureResponse;
    d->rdpAutodetect->BandwidthMeasureResults = bwMeasureResults;
    d->rdpAutodetect->NetworkCharacteristicsSync = netCharSync;
    // Experimental until Windows interoperability is validated. Default builds
    // retain the working passive reporting path.
    if (qEnvironmentVariableIntValue("KRDP_BANDWIDTH_PROBE") == 1) {
        d->rdpAutodetect->OnConnectTimeAutoDetectBegin = beginNetworkProbe;
        d->rdpAutodetect->OnConnectTimeAutoDetectProgress = progressNetworkProbe;
    }
}

bool NetworkDetection::onNetworkCharacteristicsSync(uint32_t bandwidth, uint32_t rtt)
{
    // MS-RDPBCGR 3.3.5.14: cached reconnect metrics MUST stop every measurement.
    d->probeStage = Private::ProbeStage::Complete;
    d->state = State::None;
    d->rttRequests.clear();
    d->rttMeasurements.clear();
    d->rttMeasurements.push_back({clk::system_clock::now(), clk::milliseconds(rtt)});
    d->reportedBandwidthKbps = bandwidth;
    d->hasProbedBandwidth = false; // Passive observations may improve a stale cache.
    updateAverageRtt();
    qCInfo(KRDP) << "Client supplied cached network metrics:" << bandwidth << "kbit/s," << rtt << "ms; stopping probe";
    return true;
}

// Connection-time payload PDUs are supported by the protocol/FreeRDP only
// before activation. Probe 64 KiB first; on fast links grow to 256 KiB and
// at most 1 MiB for millisecond timer resolution (1.3125 MiB total maximum).
// No synthetic graphics, padding traffic during normal use, or speed floor.
FREERDP_AUTODETECT_STATE NetworkDetection::beginConnectTimeProbe()
{
    d->probeStage = Private::ProbeStage::Rtt;
    d->probeStarted = clk::steady_clock::now();
    auto sequence = d->nextSequenceNumber();
    d->rttRequests.insert(sequence, clk::system_clock::now());
    if (!d->rdpAutodetect->RTTMeasureRequest(d->rdpAutodetect, RDP_TRANSPORT_TCP, sequence)) {
        d->probeStage = Private::ProbeStage::Complete;
        return FREERDP_AUTODETECT_STATE_FAIL;
    }
    return FREERDP_AUTODETECT_STATE_REQUEST;
}

bool NetworkDetection::sendProbeRound(uint32_t bytes)
{
    // FreeRDP encodes MCS length with the two-octet PER form. Its decoder
    // accepts 16-KiB payloads, but their length crosses the 14-bit limit and
    // Windows interprets 0b11 as a fragmented determinant. Stay below it,
    // including the security and bandwidth headers.
    constexpr uint16_t chunkBytes = 8192;
    d->probeBytes = bytes;
    d->probeSequence = d->nextSequenceNumber();
    d->probeRoundComplete = false;
    d->probeStage = Private::ProbeStage::Bandwidth;
    auto *autodetect = d->rdpAutodetect;
    if (!autodetect->BandwidthMeasureStart(autodetect, RDP_TRANSPORT_TCP, d->probeSequence)) return false;
    for (uint32_t sent = chunkBytes; sent < bytes; sent += chunkBytes) {
        if (!autodetect->BandwidthMeasurePayload(autodetect, RDP_TRANSPORT_TCP, d->probeSequence, chunkBytes)) return false;
    }
    return autodetect->BandwidthMeasureStop(autodetect, RDP_TRANSPORT_TCP, d->probeSequence, chunkBytes);
}

FREERDP_AUTODETECT_STATE NetworkDetection::progressConnectTimeProbe()
{
    if (d->probeStage == Private::ProbeStage::Complete) return FREERDP_AUTODETECT_STATE_COMPLETE;
    if (d->probeStage == Private::ProbeStage::Rtt && !d->rttMeasurements.empty()) {
        return sendProbeRound(64 * 1024) ? FREERDP_AUTODETECT_STATE_REQUEST : FREERDP_AUTODETECT_STATE_FAIL;
    }
    if (d->probeStage != Private::ProbeStage::Bandwidth || !d->probeRoundComplete) return FREERDP_AUTODETECT_STATE_REQUEST;
    if (d->probeTimeDelta < 20 && d->probeBytes < 1024 * 1024) {
        return sendProbeRound(d->probeBytes * 4) ? FREERDP_AUTODETECT_STATE_REQUEST : FREERDP_AUTODETECT_STATE_FAIL;
    }
    if (d->probeTimeDelta > 0 && d->probeByteCount >= d->probeBytes) {
        d->reportedBandwidthKbps = static_cast<uint32_t>(static_cast<uint64_t>(d->probeByteCount) * 8 / d->probeTimeDelta);
        d->hasProbedBandwidth = d->reportedBandwidthKbps > 0;
        d->lastNetworkResult = {};
        updateAverageRtt();
        qCInfo(KRDP) << "Connection-time bandwidth probe:" << d->probeByteCount << "bytes in" << d->probeTimeDelta
                    << "ms ->" << d->reportedBandwidthKbps << "kbit/s";
    } else {
        qCWarning(KRDP) << "Connection-time bandwidth probe did not produce a usable sample; retaining passive fallback";
    }
    d->probeStage = Private::ProbeStage::Complete;
    return FREERDP_AUTODETECT_STATE_COMPLETE;
}

bool NetworkDetection::connectTimeProbeTimedOut() const
{
    const auto stage = d->probeStage;
    return (stage == Private::ProbeStage::Rtt || stage == Private::ProbeStage::Bandwidth)
        && clk::steady_clock::now() - d->probeStarted > clk::seconds(8);
}

void NetworkDetection::startBandwidthMeasure()
{
    if (d->state != State::None) {
        return;
    }

    d->state = State::PendingStop;
    d->bandwidthMeasureStartTime = clk::system_clock::now();
    if (!d->rdpAutodetect->BandwidthMeasureStart(d->rdpAutodetect, RDP_TRANSPORT_TCP, 0)) {
        d->state = State::None;
    }
}

void NetworkDetection::stopBandwidthMeasure()
{
    if (d->state != State::PendingStop) {
        return;
    }

    d->state = State::PendingResults;
    if (!d->rdpAutodetect->BandwidthMeasureStop(d->rdpAutodetect, RDP_TRANSPORT_TCP, 0, 0)) {
        d->state = State::None;
    }
}

void NetworkDetection::update()
{
    if (d->session->state() != RdpConnection::State::Streaming) {
        return;
    }

    auto now = clk::system_clock::now();

    // Do not let a missing bandwidth response stall measurement forever.
    if (d->state == State::PendingResults && now - d->bandwidthMeasureStartTime > clk::seconds(5)) {
        d->state = State::None;
    }

    // Goodput: run a bandwidthMeasureDuration-long window every
    // bandwidthMeasureInterval, independent of frame sends (a single frame is
    // much too short a sample; see the constants above).
    if (d->state == State::PendingStop && (now - d->bandwidthMeasureStartTime) >= bandwidthMeasureDuration) {
        stopBandwidthMeasure();
    } else if (d->state == State::None && (now - d->lastBandwidthMeasureStart) >= bandwidthMeasureInterval) {
        d->lastBandwidthMeasureStart = now;
        startBandwidthMeasure();
    }

    if ((now - d->lastRttUpdate) < rttUpdateInterval) {
        return;
    }

    d->lastRttUpdate = now;

    auto sequence = d->nextSequenceNumber();
    d->rttRequests.insert(sequence, now);
    d->rdpAutodetect->RTTMeasureRequest(d->rdpAutodetect, RDP_TRANSPORT_TCP, sequence);
}

bool NetworkDetection::onRttMeasureResponse(uint16_t sequence)
{
    if (!d->rttRequests.contains(sequence)) {
        return true;
    }

    RTTMeasurement rtt;
    rtt.measurementTime = clk::system_clock::now();
    rtt.roundTripTime = rtt.measurementTime - d->rttRequests.take(sequence);

    if (rtt.roundTripTime.count() <= 0) {
        return true;
    }

    d->rttMeasurements.push_back(std::move(rtt));

    updateAverageRtt();

    return true;
}

bool NetworkDetection::onBandwidthMeasureResults(uint32_t timeDelta, uint32_t byteCount, uint16_t sequence, uint16_t responseType)
{
    if (responseType == RDP_BW_RESULTS_RESPONSE_TYPE_CONNECTTIME) {
        if (d->probeStage == Private::ProbeStage::Bandwidth && sequence == d->probeSequence) {
            d->probeTimeDelta = timeDelta;
            d->probeByteCount = byteCount;
            d->probeRoundComplete = true;
        }
        return true;
    }
    if (d->state != State::PendingResults) {
        return true;
    }

    d->state = State::None;

    // A single ~500 ms window can still land on a nearly-idle stretch (a few
    // ACKs, no frame data) or, on a fast link, complete in well under a
    // millisecond; both produce a byteCount/timeDelta ratio that is noise,
    // not goodput (see research.md OPT-016 - a production sample of "10
    // bytes in 1 ms" read as 296 kbit/s and one of "416 bytes in 1 ms" read
    // as 1.7 Gbit/s). Reject those before they ever reach the smoothing
    // filter instead of letting them drag it around.
    if (timeDelta < minimumBandwidthSampleDuration || byteCount < minimumBandwidthSampleBytes) {
        qCDebug(KRDP) << "Ignoring bandwidth sample:" << byteCount << "bytes in" << timeDelta << "ms";
        return true;
    }

    const auto bytesPerSecond = static_cast<uint32_t>(std::min<uint64_t>(
        static_cast<uint64_t>(byteCount) * 1000ULL / timeDelta, std::numeric_limits<uint32_t>::max()));
    if (!d->hasSmoothedBandwidth) {
        d->hasSmoothedBandwidth = true;
        d->smoothedBandwidthBps = bytesPerSecond;
    } else {
        d->smoothedBandwidthBps = (1.0 - bandwidthSmoothingWeight) * d->smoothedBandwidthBps + bandwidthSmoothingWeight * bytesPerSecond;
    }
    d->averageBandwidthBps.store(static_cast<uint32_t>(d->smoothedBandwidthBps));
    d->validSamples.fetch_add(1);
    Q_EMIT bandwidthChanged();

    qCDebug(KRDP) << "Bandwidth measurement:" << byteCount << "bytes in" << timeDelta << "ms ->" << bandwidth() << "kbit/s";

    // FreeRDP 3.31 invokes our callback with byteCount/timeDelta; it does not
    // populate netCharBandwidth. The wire value is kilobits/s, not bytes/s.
    const auto measuredKbps = static_cast<uint32_t>(static_cast<uint64_t>(byteCount) * 8 / timeDelta);
    if (!d->hasProbedBandwidth && measuredKbps > static_cast<uint64_t>(d->reportedBandwidthKbps) * 5 / 4) {
        qCInfo(KRDP) << "Observed network throughput:" << measuredKbps << "kbit/s";
    }
    if (!d->hasProbedBandwidth) {
        d->reportedBandwidthKbps = std::max(d->reportedBandwidthKbps, measuredKbps);
    }

    updateAverageRtt();

    return true;
}

void NetworkDetection::updateAverageRtt()
{
    auto now = clk::system_clock::now();
    if (d->probeStage != Private::ProbeStage::Bandwidth) {
        d->rttMeasurements.erase(std::remove_if(d->rttMeasurements.begin(),
                                                d->rttMeasurements.end(),
                                                [now](const auto &measurement) {
                                                    return (now - measurement.measurementTime) > rttAverageInterval;
                                                }),
                                 d->rttMeasurements.end());
    }
    if (d->rttMeasurements.empty()) {
        return;
    }

    d->minimumRtt = std::numeric_limits<clk::system_clock::duration>::max();
    auto sum = clk::system_clock::duration(0);
    std::for_each(d->rttMeasurements.begin(), d->rttMeasurements.end(), [this, &sum](const auto &measurement) {
        d->minimumRtt = std::min(d->minimumRtt, measurement.roundTripTime);
        sum = sum + measurement.roundTripTime;
    });
    d->averageRtt = sum / d->rttMeasurements.size();

    Q_EMIT rttChanged();

    if (d->reportedBandwidthKbps == 0) {
        return;
    }

    if ((now - d->lastNetworkResult) < networkResultInterval) {
        return;
    }

    d->lastNetworkResult = now;

    rdpNetworkCharacteristicsResult result{};
    result.type = RDP_NETCHAR_RESULT_TYPE_BASE_RTT_BW_AVG_RTT;
    result.baseRTT = clk::duration_cast<clk::milliseconds>(d->minimumRtt).count();
    result.averageRTT = clk::duration_cast<clk::milliseconds>(d->averageRtt).count();
    result.bandwidth = d->reportedBandwidthKbps;
    qCDebug(KRDP) << "Reporting observed bandwidth:" << result.bandwidth << "kbit/s";
    d->rdpAutodetect->NetworkCharacteristicsResult(d->rdpAutodetect, RDP_TRANSPORT_TCP, d->nextSequenceNumber(), &result);
}

uint16_t NetworkDetection::Private::nextSequenceNumber()
{
    uint16_t sequence = sequenceNumber;
    while (sequence == 0 || rttRequests.contains(sequence)) {
        ++sequence;
    }
    sequenceNumber = static_cast<uint16_t>(sequence + 1);
    return sequence;
}

} // namespace KRdp

#include "moc_NetworkDetection.cpp"
