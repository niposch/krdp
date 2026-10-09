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
