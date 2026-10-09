// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include <QImage>
#include <QTest>
#include <freerdp/codec/progressive.h>
#include <memory>
class ProgressiveCodecTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void independentMonitorRoundTrips()
    {
        using Context = std::unique_ptr<PROGRESSIVE_CONTEXT, decltype(&progressive_context_free)>;
        Context decoder(progressive_context_new(FALSE), progressive_context_free);
        QVERIFY(decoder);
        const QSize sizes[] = { QSize(321, 241), QSize(267, 203) };
        const QColor colors[] = { QColor(30, 215, 96), QColor(215, 30, 96) };
        for (int monitor = 0; monitor < 2; ++monitor) {
            const QSize size = sizes[monitor];
            Context encoder(progressive_context_new(TRUE), progressive_context_free);
            QVERIFY(encoder);
            QVERIFY(progressive_create_surface_context(encoder.get(), monitor + 1, size.width(), size.height()) >= 0);
            QVERIFY(progressive_create_surface_context(decoder.get(), monitor + 1, size.width(), size.height()) >= 0);
            QImage source(size, QImage::Format_RGB32);
            source.fill(colors[monitor]);
            QImage output(size, QImage::Format_RGB32);
            output.fill(Qt::black);
            REGION16 damage { };
            region16_init(&damage);
            RECTANGLE_16 rect { 0, 0, UINT16(size.width()), UINT16(size.height()) };
            QVERIFY(region16_union_rect(&damage, &damage, &rect));
            for (int frame = 0; frame < 3; ++frame) {
                BYTE* bytes = nullptr;
                UINT32 length = 0;
                QVERIFY(progressive_compress(encoder.get(), source.constBits(), source.sizeInBytes(), PIXEL_FORMAT_BGRX32, size.width(),
                            size.height(), source.bytesPerLine(), &damage, &bytes, &length)
                    >= 0);
                QVERIFY(bytes && length);
                REGION16 decoded { };
                region16_init(&decoded);
                QVERIFY(progressive_decompress(decoder.get(), bytes, length, output.bits(), PIXEL_FORMAT_BGRX32, output.bytesPerLine(), 0,
                            0, &decoded, monitor + 1, frame)
                    >= 0);
                QVERIFY(!region16_is_empty(&decoded));
                region16_uninit(&decoded);
                const QColor result = output.pixelColor(size.width() - 1, size.height() - 1);
                QVERIFY(qAbs(result.red() - colors[monitor].red()) <= 6);
                QVERIFY(qAbs(result.green() - colors[monitor].green()) <= 6);
                QVERIFY(qAbs(result.blue() - colors[monitor].blue()) <= 6);
            }
            region16_uninit(&damage);
            progressive_delete_surface_context(decoder.get(), monitor + 1);
        }
    }
};
QTEST_GUILESS_MAIN(ProgressiveCodecTest)
#include "ProgressiveCodecTest.moc"
