// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// Compile the production implementation with synthetic protocol callbacks.
#include "NetworkDetection.cpp"
#include <QTest>
Q_LOGGING_CATEGORY(KRDP, "krdp.network.test")
namespace KRdp {
// The fixture supplies autodetect directly; these unused peer accessors avoid
// linking the desktop/server stack into this protocol regression test.
rdpContext* RdpConnection::rdpPeerContext() const { return nullptr; }
RdpConnection::State RdpConnection::state() const { return State::Closed; }
static uint32_t reported = 0;
static int results = 0;
static BOOL sendResult(rdpAutoDetect*, RDP_TRANSPORT_TYPE, uint16_t, const rdpNetworkCharacteristicsResult* result)
{
    reported = result->bandwidth;
    ++results;
    return TRUE;
}
static uint32_t probeSent = 0;
static uint16_t probeMaxChunk = 0;
static BOOL probeRtt(rdpAutoDetect*, RDP_TRANSPORT_TYPE, uint16_t) { return TRUE; }
static BOOL probeStart(rdpAutoDetect*, RDP_TRANSPORT_TYPE, uint16_t) { return TRUE; }
static BOOL probePayload(rdpAutoDetect*, RDP_TRANSPORT_TYPE, uint16_t, uint16_t bytes)
{
    probeSent += bytes;
    probeMaxChunk = std::max(probeMaxChunk, bytes);
    return TRUE;
}
static BOOL probeStop(rdpAutoDetect*, RDP_TRANSPORT_TYPE, uint16_t, uint16_t bytes)
{
    probeSent += bytes;
    probeMaxChunk = std::max(probeMaxChunk, bytes);
    return TRUE;
}
class NetworkDetectionTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void reporting()
    {
        NetworkDetection network(nullptr);
        rdpAutoDetect autodetect { };
        autodetect.NetworkCharacteristicsResult = sendResult;
        network.d->rdpAutodetect = &autodetect;
        reported = 0;
        results = 0;
        auto refreshRtt = [&] {
            network.d->rttMeasurements.push_back({ clk::system_clock::now(), clk::milliseconds(4) });
            network.d->lastNetworkResult = { };
            network.updateAverageRtt();
        };
        auto sample = [&](uint32_t ms, uint32_t bytes) {
            network.d->state = NetworkDetection::State::PendingResults;
            QVERIFY(network.onBandwidthMeasureResults(ms, bytes));
        };
        refreshRtt();
        QCOMPARE(results, 0); // No uninitialized/zero report.
        sample(0, 5000000);
        sample(500, 0);
        sample(1, 416);
        refreshRtt();
        QCOMPARE(results, 0);
        sample(500, 5000000); // 10 MB/s = 80,000 kbit/s, with FreeRDP field zero.
        QCOMPARE(autodetect.netCharBandwidth, 0U);
        QCOMPARE(network.bandwidth(), 80000U);
        QCOMPARE(reported, 80000U);
        QVERIFY(results > 0);
        sample(500, 4096);
        refreshRtt();
        QCOMPARE(reported,
            80000U); // Low application demand cannot lower capacity estimate.
        sample(500, 7500000);
        refreshRtt();
        QCOMPARE(reported, 120000U);
        QCOMPARE(network.validBandwidthSamples(), 3);
        sample(100, std::numeric_limits<uint32_t>::max());
        refreshRtt();
        QCOMPARE(reported, 343597383U); // 64-bit conversion before narrowing.
    }
    void activeProbe()
    {
        NetworkDetection network(nullptr);
        rdpAutoDetect autodetect { };
        autodetect.RTTMeasureRequest = probeRtt;
        autodetect.BandwidthMeasureStart = probeStart;
        autodetect.BandwidthMeasurePayload = probePayload;
        autodetect.BandwidthMeasureStop = probeStop;
        autodetect.NetworkCharacteristicsResult = sendResult;
        network.d->rdpAutodetect = &autodetect;
        probeSent = 0;
        reported = 0;
        results = 0;
        QCOMPARE(network.beginConnectTimeProbe(), FREERDP_AUTODETECT_STATE_REQUEST);
        network.d->rttMeasurements.push_back({ clk::system_clock::now() - clk::seconds(1), clk::milliseconds(4) });
        QCOMPARE(network.progressConnectTimeProbe(), FREERDP_AUTODETECT_STATE_REQUEST);
        QCOMPARE(probeSent, 65536U);
        // Wrong sequence cannot advance the probe; zero-ms sample must grow.
        network.onBandwidthMeasureResults(0, 65536, network.d->probeSequence + 1, RDP_BW_RESULTS_RESPONSE_TYPE_CONNECTTIME);
        QCOMPARE(network.progressConnectTimeProbe(), FREERDP_AUTODETECT_STATE_REQUEST);
        QCOMPARE(probeSent, 65536U);
        network.onBandwidthMeasureResults(0, 65536, network.d->probeSequence, RDP_BW_RESULTS_RESPONSE_TYPE_CONNECTTIME);
        QCOMPARE(network.progressConnectTimeProbe(), FREERDP_AUTODETECT_STATE_REQUEST);
        QCOMPARE(probeSent, 65536U + 262144U);
        network.onBandwidthMeasureResults(1, 262144, network.d->probeSequence, RDP_BW_RESULTS_RESPONSE_TYPE_CONNECTTIME);
        QCOMPARE(network.progressConnectTimeProbe(), FREERDP_AUTODETECT_STATE_REQUEST);
        QCOMPARE(probeSent, 65536U + 262144U + 1048576U);
        QCOMPARE(probeMaxChunk, uint16_t(8192));
        // Independent PER rule: two-byte lengths must start with 0b10.
        QCOMPARE(((probeMaxChunk + 12U) | 0x8000U) & 0xC000U, 0x8000U);
        network.onBandwidthMeasureResults(40, 1048576, network.d->probeSequence, RDP_BW_RESULTS_RESPONSE_TYPE_CONNECTTIME);
        QCOMPARE(network.progressConnectTimeProbe(), FREERDP_AUTODETECT_STATE_COMPLETE);
        QCOMPARE(reported, 209715U);
        QVERIFY(network.d->hasProbedBandwidth);
        // Probe capacity and application goodput are separate estimates.
        network.d->state = NetworkDetection::State::PendingResults;
        network.onBandwidthMeasureResults(500, 4096);
        QCOMPARE(network.d->reportedBandwidthKbps, 209715U);
        QCOMPARE(network.bandwidth(), 65U);
        QVERIFY(!network.connectTimeProbeTimedOut());
        network.d->probeStage = NetworkDetection::Private::ProbeStage::Rtt;
        network.d->probeStarted = clk::steady_clock::now() - clk::seconds(9);
        QVERIFY(network.connectTimeProbeTimedOut());
    }
    void slowProbe()
    {
        NetworkDetection network(nullptr);
        rdpAutoDetect autodetect { };
        autodetect.RTTMeasureRequest = probeRtt;
        autodetect.BandwidthMeasureStart = probeStart;
        autodetect.BandwidthMeasurePayload = probePayload;
        autodetect.BandwidthMeasureStop = probeStop;
        autodetect.NetworkCharacteristicsResult = sendResult;
        network.d->rdpAutodetect = &autodetect;
        probeSent = 0;
        network.beginConnectTimeProbe();
        network.d->rttMeasurements.push_back({ clk::system_clock::now(), clk::milliseconds(100) });
        network.progressConnectTimeProbe();
        network.onBandwidthMeasureResults(500, 65536, network.d->probeSequence, RDP_BW_RESULTS_RESPONSE_TYPE_CONNECTTIME);
        QCOMPARE(network.progressConnectTimeProbe(), FREERDP_AUTODETECT_STATE_COMPLETE);
        QCOMPARE(probeSent, 65536U);
        QCOMPARE(network.d->reportedBandwidthKbps, 1048U);
    }
    void reconnectSyncStopsProbe()
    {
        NetworkDetection network(nullptr);
        rdpAutoDetect autodetect{};
        autodetect.NetworkCharacteristicsResult = sendResult;
        network.d->rdpAutodetect = &autodetect;
        network.d->probeStage = NetworkDetection::Private::ProbeStage::Bandwidth;
        network.d->state = NetworkDetection::State::PendingResults;
        network.d->rttRequests.insert(42, clk::system_clock::now());
        QVERIFY(network.onNetworkCharacteristicsSync(50000, 20));
        QCOMPARE(network.progressConnectTimeProbe(), FREERDP_AUTODETECT_STATE_COMPLETE);
        QCOMPARE(network.d->reportedBandwidthKbps, 50000U);
        QCOMPARE(network.averageRTT(), clk::system_clock::duration(clk::milliseconds(20)));
        QVERIFY(network.d->rttRequests.empty());
        QCOMPARE(network.d->state, NetworkDetection::State::None);
        QVERIFY(!network.connectTimeProbeTimedOut());
    }
    void sequenceWrap()
    {
        NetworkDetection network(nullptr);
        network.d->sequenceNumber = 65535;
        QCOMPARE(network.d->nextSequenceNumber(), uint16_t(65535));
        QCOMPARE(network.d->nextSequenceNumber(), uint16_t(1));
        network.d->rttRequests.insert(2, clk::system_clock::now());
        QCOMPARE(network.d->nextSequenceNumber(), uint16_t(3));
    }
};
} // namespace KRdp
QTEST_GUILESS_MAIN(KRdp::NetworkDetectionTest)
#include "NetworkDetectionTest.moc"
