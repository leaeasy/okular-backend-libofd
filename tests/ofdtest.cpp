/*
    SPDX-FileCopyrightText: 2026 OFD plugin contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "document.h"

#include <okular/core/generator.h>
#include <okular/core/page.h>

#include <KPluginFactory>
#include <KPluginMetaData>
#include <KZip>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMimeDatabase>
#include <QPixmap>
#include <QProcess>
#include <QRawFont>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace
{
using Entries = QMap<QString, QByteArray>;
QByteArray xml(const QByteArray &root, const QByteArray &body)
{
    return '<' + root + " xmlns=\"http://www.ofdspec.org/2016\">" + body + "</" + root + '>';
}
QByteArray png(Qt::GlobalColor color)
{
    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(color);
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return data;
}
Entries fixture()
{
    // Deliberately use arbitrary paths, reverse lexical page/resource order,
    // and reuse an image. A resource image is not a document page.
    return {
        {QStringLiteral("OFD.xml"), xml("OFD", "<DocBody><DocRoot>Book/Main.xml</DocRoot></DocBody>")},
        {QStringLiteral("Book/Main.xml"),
         xml("Document",
             "<CommonData><PageArea><PhysicalBox>0 0 100 200</PhysicalBox></PageArea><PublicRes>Assets/Res.xml</PublicRes></CommonData><Pages><Page ID=\"1\" "
             "BaseLoc=\"Z/Content.xml\"/><Page ID=\"2\" BaseLoc=\"A/Content.xml\"/></Pages>")},
        {QStringLiteral("Book/Assets/Res.xml"),
         xml("Res",
             "<MultiMedias><MultiMedia ID=\"9\" Type=\"Image\"><MediaFile>Z.png</MediaFile></MultiMedia><MultiMedia ID=\"8\" "
             "Type=\"Image\"><MediaFile>A.png</MediaFile></MultiMedia></MultiMedias>")},
        {QStringLiteral("Book/Assets/Z.png"), png(Qt::red)},
        {QStringLiteral("Book/Assets/A.png"), png(Qt::blue)},
        {QStringLiteral("Book/Z/Content.xml"),
         xml("Page",
             "<Content><Layer ID=\"1\"><ImageObject ID=\"2\" ResourceID=\"9\" Boundary=\"10 20 30 40\" CTM=\"30 0 0 40 0 0\"/><ImageObject ID=\"3\" "
             "ResourceID=\"8\" Boundary=\"60 100 20 30\" CTM=\"20 0 0 30 0 0\"/></Layer></Content>")},
        {QStringLiteral("Book/A/Content.xml"),
         xml("Page",
             "<Area><PhysicalBox>5 10 80 60</PhysicalBox></Area><Content><Layer ID=\"4\"><ImageObject ID=\"5\" ResourceID=\"8\" Boundary=\"5 10 80 60\" "
             "CTM=\"80 0 0 60 0 0\"/></Layer></Content>")},
    };
}
}

class OFDTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void pagesAndImages();
    void pathsAndText();
    void resourceLocations();
    void opacityAndClipping();
    void embeddedFonts();
    void drawingParameters();
    void templatesAndLayerOrder();
    void pageResourceScopes();
    void arcs_data();
    void arcs();
    void colors_data();
    void colors();
    void objectClipping();
    void clipTransforms();
    void clipFillRulesAndEmptyIntersection();
    void textAndStrokeClips();
    void malformed_data();
    void malformed();
    void unsupportedFeatures();
    void pluginIntegration();

private:
    QString write(const Entries &entries);
    QTemporaryDir m_dir;
    int m_serial = 0;
};

QString OFDTest::write(const Entries &entries)
{
    const auto path = m_dir.filePath(QString::number(++m_serial) + QStringLiteral(".ofd"));
    KZip archive(path);
    if (!archive.open(QIODevice::WriteOnly)) {
        return {};
    }
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        if (!archive.writeFile(it.key(), it.value())) {
            return {};
        }
    }
    return archive.close() ? path : QString();
}

void OFDTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
    qputenv("XDG_CONFIG_HOME", m_dir.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME", m_dir.filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_DATA_HOME", m_dir.filePath(QStringLiteral("data")).toUtf8());
    // A direct installation must register the MIME type without a manual
    // update-mime-database command. Exercise --prefix relocation as well.
    const QString mimeDir = m_dir.filePath(QStringLiteral("share/mime"));
    const QByteArray existing = qgetenv("XDG_DATA_DIRS");
    const QByteArray dataDirs =
        m_dir.filePath(QStringLiteral("share")).toUtf8() + ':' + (existing.isEmpty() ? QByteArray("/usr/local/share:/usr/share") : existing);
    qputenv("XDG_DATA_DIRS", dataDirs);
    QProcess installer;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("DESTDIR"));
    installer.setProcessEnvironment(environment);
    installer.start(QStringLiteral(OFD_CMAKE_COMMAND),
                    {QStringLiteral("--install"), QStringLiteral(OFD_BUILD_DIR), QStringLiteral("--prefix"), m_dir.path()});
    QVERIFY(installer.waitForFinished(60000));
    const QByteArray installOutput = installer.readAllStandardOutput() + installer.readAllStandardError();
    QVERIFY2(installer.exitStatus() == QProcess::NormalExit && installer.exitCode() == 0, installOutput.constData());
    QVERIFY2(QFileInfo::exists(mimeDir + QStringLiteral("/application/ofd.xml")), installOutput.constData());
    QVERIFY(QFileInfo::exists(mimeDir + QStringLiteral("/mime.cache")));
    QCoreApplication::addLibraryPath(QFileInfo(QStringLiteral(GENERATOR_PATH)).dir().absoluteFilePath(QStringLiteral("..")));
}

void OFDTest::pagesAndImages()
{
    OFD::Document document;
    const auto file = write(fixture());
    QVERIFY(document.open(file));
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.pageSize(0), QSizeF(100, 200));
    QCOMPARE(document.pageSize(1), QSizeF(80, 60));
    QVERIFY(!document.hasUnsupportedFeatures());
    auto image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(0, 0), QColor(Qt::white));
    QCOMPARE(image.pixelColor(15, 25), QColor(Qt::red));
    QCOMPARE(image.pixelColor(65, 105), QColor(Qt::blue));
    QCOMPARE(image.pixelColor(45, 65), QColor(Qt::white));
    image = document.render(0, QSize(200, 400));
    QCOMPARE(image.pixelColor(30, 50), QColor(Qt::red));
    QCOMPARE(image.pixelColor(130, 210), QColor(Qt::blue));
    image = document.render(1, QSize(80, 60));
    QCOMPARE(image.pixelColor(0, 0), QColor(Qt::blue));
    QCOMPARE(image.pixelColor(79, 59), QColor(Qt::blue));
    QVERIFY(document.render(-1, QSize(100, 200)).isNull());
    QVERIFY(document.render(2, QSize(100, 200)).isNull());
    QVERIFY(document.render(0, QSize(100000, 100000)).isNull());
    document.close();
    QCOMPARE(document.pageCount(), 0);
    QVERIFY(document.render(0, QSize(10, 10)).isNull());
    QVERIFY(document.open(file));
    QCOMPARE(document.pageCount(), 2);
}

void OFDTest::pathsAndText()
{
    auto entries = fixture();
    entries[QStringLiteral("Book/Z/Content.xml")] =
        xml("Page",
            "<Content><Layer ID=\"1\">"
            "<PathObject ID=\"2\" Boundary=\"10 20 30 40\" Fill=\"true\" Stroke=\"false\" CTM=\"2 0 0 2 0 0\"><FillColor Value=\"0 255 0\"/><AbbreviatedData>M "
            "0 0 L 10 0 L 10 10 L 0 10 C</AbbreviatedData></PathObject>"
            "<TextObject ID=\"3\" Boundary=\"0 70 100 30\" Size=\"15\"><TextCode X=\"5\" Y=\"20\" DeltaX=\"g 3 20\">TEST</TextCode></TextObject>"
            "<PathObject ID=\"4\" Boundary=\"0 120 100 50\" Fill=\"false\" LineWidth=\"2\"><AbbreviatedData>M 5 5 Q 10 30 20 5 B 30 0 40 20 50 "
            "5</AbbreviatedData></PathObject>"
            "</Layer></Content>");
    entries[QStringLiteral("Book/Assets/Res.xml")].replace("</Res>", "<Fonts><Font ID=\"12\" FamilyName=\"sans-serif\"/></Fonts></Res>");
    entries[QStringLiteral("Book/Z/Content.xml")].replace("<TextObject ", "<TextObject Font=\"12\" ");
    OFD::Document document;
    QVERIFY(document.open(write(entries)));
    QVERIFY(!document.hasUnsupportedFeatures());
    const auto image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(15, 25), QColor(Qt::green));
    QCOMPARE(image.pixelColor(35, 45), QColor(Qt::white));
    // Font-independent checks: each explicitly positioned letter produces ink.
    for (int letter = 0; letter < 4; ++letter) {
        int ink = 0;
        for (int y = 70; y < 100; ++y) {
            for (int x = 5 + letter * 20; x < 20 + letter * 20; ++x) {
                ink += image.pixelColor(x, y) != QColor(Qt::white);
            }
        }
        QVERIFY(ink > 0);
    }
    int curveInk = 0;
    for (int y = 120; y < 170; ++y) {
        for (int x = 0; x < 100; ++x) {
            curveInk += image.pixelColor(x, y) != QColor(Qt::white);
        }
    }
    QVERIFY(curveInk > 20);
}

void OFDTest::resourceLocations()
{
    auto entries = fixture();
    entries[QStringLiteral("Book/Assets/Res.xml")] =
        "<r:Res xmlns:r=\"http://www.ofdspec.org\" BaseLoc=\"../../Images\"><r:MultiMedias><r:MultiMedia ID=\"9\" "
        "Type=\"Image\"><r:MediaFile>Red.png</r:MediaFile></r:MultiMedia><r:MultiMedia ID=\"8\" "
        "Type=\"Image\"><r:MediaFile>/Images/Blue.png</r:MediaFile></r:MultiMedia></r:MultiMedias></r:Res>";
    entries[QStringLiteral("Images/Red.png")] = entries.take(QStringLiteral("Book/Assets/Z.png"));
    entries[QStringLiteral("Images/Blue.png")] = entries.take(QStringLiteral("Book/Assets/A.png"));
    OFD::Document document;
    QVERIFY2(document.open(write(entries)), qPrintable(document.errorString()));
    QCOMPARE(document.render(0, QSize(100, 200)).pixelColor(15, 25), QColor(Qt::red));
    QCOMPARE(document.render(1, QSize(80, 60)).pixelColor(1, 1), QColor(Qt::blue));
}

void OFDTest::opacityAndClipping()
{
    auto entries = fixture();
    entries[QStringLiteral("Book/Z/Content.xml")] = xml("Page",
                                                        "<Content><Layer>"
                                                        "<ImageObject ResourceID=\"9\" Boundary=\"10 20 20 20\" CTM=\"40 0 0 40 0 0\" Alpha=\"128\"/>"
                                                        "<ImageObject ResourceID=\"8\" Boundary=\"50 20 20 20\"/>"
                                                        "</Layer></Content>");
    OFD::Document document;
    QVERIFY(document.open(write(entries)));
    const auto image = document.render(0, QSize(100, 200));
    const auto pink = image.pixelColor(15, 25);
    QCOMPARE(pink.red(), 255);
    QVERIFY(pink.green() >= 126 && pink.green() <= 128);
    QCOMPARE(pink.green(), pink.blue());
    QCOMPARE(image.pixelColor(35, 25), QColor(Qt::white));
    QCOMPARE(image.pixelColor(55, 25), QColor(Qt::blue));
}

void OFDTest::embeddedFonts()
{
    QFile file(QStringLiteral(OFD_TEST_DATA_DIR "/OFDTest.ttf"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto fontData = file.readAll();
    const QRawFont raw(fontData, 1000, QFont::PreferNoHinting);
    QVERIFY(raw.isValid());
    QCOMPARE(raw.familyName(), QStringLiteral("OFDTest Shapes"));
    const auto families = QFontDatabase::families();
    auto entries = fixture();
    entries[QStringLiteral("Book/Assets/Res.xml")].replace(
        "</Res>",
        "<Fonts><Font ID=\"45\" FamilyName=\"monospace\"><FontFile>Shapes.ttf</FontFile></Font></Fonts></Res>");
    entries[QStringLiteral("Book/Assets/Shapes.ttf")] = fontData;
    entries[QStringLiteral("Book/Z/Content.xml")] = xml("Page",
                                                        "<Content><Layer><TextObject Font=\"45\" Size=\"20\" Boundary=\"0 0 100 200\"><TextCode X=\"10\" "
                                                        "Y=\"30\">AA</TextCode></TextObject></Layer></Content>");
    OFD::Document document;
    QVERIFY2(document.open(write(entries)), qPrintable(document.errorString()));
    QVERIFY(!document.hasUnsupportedFeatures());
    const auto image = document.render(0, QSize(100, 200));
    // The embedded A is a solid square, with a 1000-unit advance. Neither
    // its shape nor its spacing can be produced by substituting monospace A.
    for (const auto x : {12, 18, 24, 32, 38, 44}) {
        QCOMPARE(image.pixelColor(x, 20), QColor(Qt::black));
    }
    QCOMPARE(image.pixelColor(28, 20), QColor(Qt::white));
    QCOMPARE(image.pixelColor(48, 20), QColor(Qt::white));
    QCOMPARE(QFontDatabase::families(), families);
    // Unsupported font formats retain a visible fallback warning.
    entries[QStringLiteral("Book/Assets/Shapes.ttf")] = "unsupported font data";
    QVERIFY(document.open(write(entries)));
    QVERIFY(document.hasUnsupportedFeatures());
    // Closing/reopening cannot keep an embedded font from the previous file.
    entries[QStringLiteral("Book/Assets/Res.xml")].replace("<FontFile>Shapes.ttf</FontFile>", "");
    QVERIFY(document.open(write(entries)));
    QVERIFY(!document.hasUnsupportedFeatures());
    QVERIFY(document.render(0, QSize(100, 200)) != image);
}

void OFDTest::drawingParameters()
{
    auto entries = fixture();
    entries[QStringLiteral("Book/Assets/Res.xml")].replace(
        "</Res>",
        "<DrawParams><DrawParam ID=\"100\" LineWidth=\"4\"><FillColor Value=\"0 255 0\"/><StrokeColor Value=\"0 0 255\"/></DrawParam>"
        "<DrawParam ID=\"101\" Relative=\"100\" LineWidth=\"2\" DashPattern=\"6 4\" Cap=\"Butt\" Join=\"Round\"/>"
        "<DrawParam ID=\"102\" Relative=\"101\"><StrokeColor Value=\"255 0 0\"/></DrawParam></DrawParams></Res>");
    entries[QStringLiteral("Book/Z/Content.xml")] =
        xml("Page",
            "<Content><Layer DrawParam=\"100\">"
            "<PathObject Boundary=\"10 20 30 30\" Fill=\"true\" Stroke=\"false\"><AbbreviatedData>M 0 0 L 20 0 L 20 20 L 0 20 C</AbbreviatedData></PathObject>"
            "<PathObject Boundary=\"50 20 30 30\" Fill=\"true\" Stroke=\"false\"><FillColor Value=\"255 0 0\"/><AbbreviatedData>M 0 0 L 20 0 L 20 20 L 0 20 "
            "C</AbbreviatedData></PathObject>"
            "<PageBlock DrawParam=\"101\"><PathObject Boundary=\"0 80 100 20\"><AbbreviatedData>M 0 5 L 100 5</AbbreviatedData></PathObject></PageBlock>"
            "<PathObject DrawParam=\"102\" LineWidth=\"4\" Boundary=\"0 110 100 20\"><AbbreviatedData>M 0 5 L 100 5</AbbreviatedData></PathObject>"
            "</Layer></Content>");
    OFD::Document document;
    QVERIFY2(document.open(write(entries)), qPrintable(document.errorString()));
    QVERIFY(!document.hasUnsupportedFeatures());
    const auto image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(15, 25), QColor(Qt::green));
    QCOMPARE(image.pixelColor(55, 25), QColor(Qt::red));
    QCOMPARE(image.pixelColor(2, 85), QColor(Qt::blue));
    QCOMPARE(image.pixelColor(8, 85), QColor(Qt::white));
    QCOMPARE(image.pixelColor(12, 85), QColor(Qt::blue));
    QCOMPARE(image.pixelColor(2, 115), QColor(Qt::red));
    QCOMPARE(image.pixelColor(8, 115), QColor(Qt::white));
    QCOMPARE(image.pixelColor(2, 113), QColor(Qt::red));
}

void OFDTest::templatesAndLayerOrder()
{
    auto entries = fixture();
    entries[QStringLiteral("Book/Main.xml")].replace("</CommonData>",
                                                     "<TemplatePage ID=\"50\" BaseLoc=\"Templates/Background.xml\"/>"
                                                     "<TemplatePage ID=\"51\" BaseLoc=\"Templates/Foreground.xml\" ZOrder=\"Foreground\"/></CommonData>");
    entries[QStringLiteral("Book/Templates/Background.xml")] =
        xml("Page", "<Content><Layer><ImageObject ResourceID=\"9\" Boundary=\"0 0 100 200\"/></Layer></Content>");
    entries[QStringLiteral("Book/Templates/Foreground.xml")] =
        xml("Page",
            "<Content><Layer><PathObject Boundary=\"10 10 30 30\" Fill=\"true\" Stroke=\"false\"><FillColor Value=\"0 255 0\"/><AbbreviatedData>M 0 0 L 30 0 L "
            "30 30 L 0 30 C</AbbreviatedData></PathObject></Layer></Content>");
    entries[QStringLiteral("Book/Z/Content.xml")] =
        xml("Page",
            "<Template TemplateID=\"50\"/><Template TemplateID=\"51\"/>"
            "<Content><Layer Type=\"Foreground\"><ImageObject ResourceID=\"8\" Boundary=\"20 20 5 5\"/></Layer>"
            "<Layer><ImageObject ResourceID=\"8\" Boundary=\"10 10 50 50\"/></Layer>"
            "<Layer Type=\"Background\"><ImageObject ResourceID=\"9\" Boundary=\"0 0 100 200\"/></Layer></Content>");
    OFD::Document document;
    QVERIFY2(document.open(write(entries)), qPrintable(document.errorString()));
    QVERIFY(!document.hasUnsupportedFeatures());
    auto image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(5, 5), QColor(Qt::red));
    QCOMPARE(image.pixelColor(15, 15), QColor(Qt::green));
    QCOMPARE(image.pixelColor(22, 22), QColor(Qt::blue));
    QCOMPARE(image.pixelColor(45, 45), QColor(Qt::blue));
    // A page reference overrides the template definition's default ZOrder.
    entries[QStringLiteral("Book/Z/Content.xml")].replace("TemplateID=\"51\"", "TemplateID=\"51\" ZOrder=\"Background\"");
    QVERIFY(document.open(write(entries)));
    image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(15, 15), QColor(Qt::blue));
}

void OFDTest::pageResourceScopes()
{
    auto entries = fixture();
    entries[QStringLiteral("Book/Main.xml")].replace("</CommonData>", "<TemplatePage ID=\"50\" BaseLoc=\"Templates/Content.xml\"/></CommonData>");
    entries[QStringLiteral("Book/Templates/Content.xml")] =
        xml("Page", "<PageRes>Res.xml</PageRes><Content><Layer><ImageObject ResourceID=\"9\" Boundary=\"0 0 100 200\"/></Layer></Content>");
    entries[QStringLiteral("Book/Templates/Res.xml")] =
        xml("Res", "<MultiMedias><MultiMedia ID=\"9\" Type=\"Image\"><MediaFile>Green.png</MediaFile></MultiMedia></MultiMedias>");
    entries[QStringLiteral("Book/Templates/Green.png")] = png(Qt::green);
    entries[QStringLiteral("Book/Z/Res.xml")] =
        xml("Res", "<MultiMedias><MultiMedia ID=\"9\" Type=\"Image\"><MediaFile>Blue.png</MediaFile></MultiMedia></MultiMedias>");
    entries[QStringLiteral("Book/Z/Blue.png")] = png(Qt::blue);
    entries[QStringLiteral("Book/Z/Content.xml")] = xml(
        "Page",
        "<Template TemplateID=\"50\"/><PageRes>Res.xml</PageRes><Content><Layer><ImageObject ResourceID=\"9\" Boundary=\"20 20 20 20\"/></Layer></Content>");
    entries[QStringLiteral("Book/A/Content.xml")].replace("ResourceID=\"8\"", "ResourceID=\"9\"");
    OFD::Document document;
    QVERIFY2(document.open(write(entries)), qPrintable(document.errorString()));
    QVERIFY(!document.hasUnsupportedFeatures());
    auto image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(5, 5), QColor(Qt::green));
    QCOMPARE(image.pixelColor(25, 25), QColor(Qt::blue));
    image = document.render(1, QSize(80, 60));
    QCOMPARE(image.pixelColor(5, 5), QColor(Qt::red));
}

void OFDTest::arcs_data()
{
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<QPoint>("inside");
    QTest::addColumn<QPoint>("outside");
    QTest::newRow("clockwise") << QByteArray("M 10 30 A 20 20 0 0 1 50 30 C") << QPoint(30, 15) << QPoint(30, 45);
    QTest::newRow("counterclockwise") << QByteArray("M 10 30 A 20 20 0 0 0 50 30 C") << QPoint(30, 45) << QPoint(30, 15);
    QTest::newRow("small-arc") << QByteArray("M 40 20 A 20 20 0 0 1 20 40 L 20 20 C") << QPoint(30, 30) << QPoint(50, 40);
    QTest::newRow("large-arc") << QByteArray("M 40 20 A 20 20 0 1 1 20 40 L 20 20 C") << QPoint(50, 40) << QPoint(65, 40);
    QTest::newRow("rotated-ellipse") << QByteArray("M 20 0 A 20 10 90 0 1 20 40 A 20 10 90 0 1 20 0 C") << QPoint(20, 5) << QPoint(5, 20);
    QTest::newRow("radii-correction") << QByteArray("M 10 30 A 5 5 0 0 1 50 30 C") << QPoint(30, 15) << QPoint(30, 45);
    QTest::newRow("negative-radii") << QByteArray("M 10 30 A -20 -20 0 0 1 50 30 C") << QPoint(30, 15) << QPoint(30, 45);
    QTest::newRow("zero-radius") << QByteArray("M 10 10 A 0 20 0 0 1 50 10 L 50 30 L 10 30 C") << QPoint(30, 20) << QPoint(30, 40);
    QTest::newRow("same-endpoints") << QByteArray("M 10 10 A 20 20 0 0 1 10 10 L 50 10 L 50 30 L 10 30 C") << QPoint(30, 20) << QPoint(30, 40);
}

void OFDTest::arcs()
{
    QFETCH(QByteArray, data);
    QFETCH(QPoint, inside);
    QFETCH(QPoint, outside);
    auto entries = fixture();
    entries[QStringLiteral("Book/Z/Content.xml")] = xml("Page",
                                                        "<Content><Layer><PathObject Boundary=\"0 0 100 200\" Fill=\"true\" Stroke=\"false\"><AbbreviatedData>"
                                                            + data + "</AbbreviatedData></PathObject></Layer></Content>");
    OFD::Document document;
    QVERIFY2(document.open(write(entries)), qPrintable(document.errorString()));
    QVERIFY(!document.hasUnsupportedFeatures());
    const auto image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(inside), QColor(Qt::black));
    QCOMPARE(image.pixelColor(outside), QColor(Qt::white));
}

void OFDTest::colors_data()
{
    QTest::addColumn<QByteArray>("spaces");
    QTest::addColumn<QByteArray>("defaultSpace");
    QTest::addColumn<QByteArray>("attributes");
    QTest::addColumn<QColor>("expected");
    QTest::addColumn<bool>("warning");
    QTest::newRow("rgb-hex") << QByteArray() << QByteArray() << QByteArray("Value=\"#FF #00 #00\"") << QColor(Qt::red) << false;
    QTest::newRow("cmyk") << QByteArray("<ColorSpace ID=\"1\" Type=\"CMYK\"/>") << QByteArray() << QByteArray("ColorSpace=\"1\" Value=\"255 0 0 0\"")
                          << QColor(Qt::cyan) << false;
    QTest::newRow("default-cmyk") << QByteArray("<ColorSpace ID=\"1\" Type=\"CMYK\"/>") << QByteArray("<DefaultCS>1</DefaultCS>")
                                  << QByteArray("Value=\"0 0 255 0\"") << QColor(Qt::yellow) << false;
    QTest::newRow("rgb-palette") << QByteArray("<ColorSpace ID=\"1\" Type=\"RGB\"><Palette><CV>255 0 0</CV><CV>#00 #FF #00</CV></Palette></ColorSpace>")
                                 << QByteArray() << QByteArray("ColorSpace=\"1\" Index=\"1\"") << QColor(Qt::green) << false;
    QTest::newRow("cmyk-palette") << QByteArray("<ColorSpace ID=\"1\" Type=\"CMYK\"><Palette><CV>0 255 0 0</CV></Palette></ColorSpace>")
                                  << QByteArray("<DefaultCS>1</DefaultCS>") << QByteArray("Index=\"0\"") << QColor(Qt::magenta) << false;
    QTest::newRow("gray-4-bit") << QByteArray("<ColorSpace ID=\"1\" Type=\"GRAY\" BitsPerComponent=\"4\"/>") << QByteArray()
                                << QByteArray("ColorSpace=\"1\" Value=\"8\"") << QColor(136, 136, 136) << false;
    QTest::newRow("rgb-16-bit") << QByteArray("<ColorSpace ID=\"1\" Type=\"RGB\" BitsPerComponent=\"16\"/>") << QByteArray()
                                << QByteArray("ColorSpace=\"1\" Value=\"65535 0 #8000\"") << QColor(255, 0, 128) << false;
    QTest::newRow("value-before-index") << QByteArray() << QByteArray() << QByteArray("Value=\"0 0 255\" Index=\"999\"") << QColor(Qt::blue) << false;
    QTest::newRow("out-of-range-fallback") << QByteArray() << QByteArray() << QByteArray("Value=\"256 0 0\"") << QColor(Qt::black) << true;
    QTest::newRow("icc-warning") << QByteArray("<ColorSpace ID=\"1\" Type=\"RGB\"><Profile>Color.icc</Profile></ColorSpace>") << QByteArray()
                                 << QByteArray("ColorSpace=\"1\" Value=\"0 0 255\"") << QColor(Qt::blue) << true;
}

void OFDTest::colors()
{
    QFETCH(QByteArray, spaces);
    QFETCH(QByteArray, defaultSpace);
    QFETCH(QByteArray, attributes);
    QFETCH(QColor, expected);
    QFETCH(bool, warning);
    auto entries = fixture();
    const QByteArray resources = "<ColorSpaces>" + spaces + "</ColorSpaces></Res>";
    const QByteArray common = defaultSpace + "</CommonData>";
    entries[QStringLiteral("Book/Assets/Res.xml")].replace("</Res>", resources);
    entries[QStringLiteral("Book/Main.xml")].replace("</CommonData>", common);
    entries[QStringLiteral("Book/Z/Content.xml")] =
        xml("Page",
            "<Content><Layer><PathObject Boundary=\"0 0 100 200\" Fill=\"true\" Stroke=\"false\"><FillColor " + attributes
                + "/><AbbreviatedData>M 0 0 L 100 0 L 100 200 L 0 200 C</AbbreviatedData></PathObject></Layer></Content>");
    OFD::Document document;
    QVERIFY2(document.open(write(entries)), qPrintable(document.errorString()));
    QCOMPARE(document.hasUnsupportedFeatures(), warning);
    QCOMPARE(document.render(0, QSize(100, 200)).pixelColor(20, 20), expected);
}

void OFDTest::objectClipping()
{
    auto entries = fixture();
    entries[QStringLiteral("Book/Z/Content.xml")] =
        xml("Page",
            "<Content><Layer><ImageObject ResourceID=\"9\" Boundary=\"10 20 60 60\" CTM=\"60 0 0 60 0 0\"><Clips>"
            "<Clip><Area><Path Fill=\"true\" Stroke=\"false\"><AbbreviatedData>M 0 0 L 20 0 L 20 60 L 0 60 C</AbbreviatedData></Path></Area>"
            "<Area><Path Fill=\"true\" Stroke=\"false\"><AbbreviatedData>M 40 0 L 60 0 L 60 60 L 40 60 C</AbbreviatedData></Path></Area></Clip>"
            "<Clip><Area><Path Fill=\"true\" Stroke=\"false\"><AbbreviatedData>M 0 10 L 60 10 L 60 50 L 0 50 C</AbbreviatedData></Path></Area></Clip>"
            "</Clips></ImageObject></Layer></Content>");
    OFD::Document document;
    QVERIFY2(document.open(write(entries)), qPrintable(document.errorString()));
    QVERIFY(!document.hasUnsupportedFeatures());
    const auto image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(15, 35), QColor(Qt::red));
    QCOMPARE(image.pixelColor(35, 35), QColor(Qt::white));
    QCOMPARE(image.pixelColor(55, 35), QColor(Qt::red));
    QCOMPARE(image.pixelColor(15, 25), QColor(Qt::white));
    QCOMPARE(image.pixelColor(15, 75), QColor(Qt::white));
}

void OFDTest::clipTransforms()
{
    auto entries = fixture();
    entries[QStringLiteral("Book/Z/Content.xml")] = xml("Page",
                                                        "<Content><Layer><ImageObject ResourceID=\"9\" Boundary=\"10 20 60 60\" CTM=\"60 0 0 60 0 0\"><Clips "
                                                        "TransFlag=\"true\"><Clip><Area><Path Fill=\"true\" Stroke=\"false\"><AbbreviatedData>M 0 0 L 0.5 0 L "
                                                        "0.5 1 L 0 1 C</AbbreviatedData></Path></Area></Clip></Clips></ImageObject></Layer></Content>");
    OFD::Document document;
    QVERIFY(document.open(write(entries)));
    QVERIFY(!document.hasUnsupportedFeatures());
    auto image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(15, 25), QColor(Qt::red));
    QCOMPARE(image.pixelColor(55, 25), QColor(Qt::white));
    entries[QStringLiteral("Book/Z/Content.xml")] =
        xml("Page",
            "<Content><Layer><ImageObject ResourceID=\"9\" Boundary=\"10 20 60 60\"><Clips><Clip><Area CTM=\"1 0 0 1 10 5\"><Path Boundary=\"5 5 20 20\" "
            "CTM=\"2 0 0 2 0 0\" Fill=\"true\" Stroke=\"false\"><AbbreviatedData>M 0 0 L 10 0 L 10 10 L 0 10 "
            "C</AbbreviatedData></Path></Area></Clip></Clips></ImageObject></Layer></Content>");
    QVERIFY(document.open(write(entries)));
    QVERIFY(!document.hasUnsupportedFeatures());
    image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(30, 35), QColor(Qt::red));
    QCOMPARE(image.pixelColor(20, 35), QColor(Qt::white));
    QCOMPARE(image.pixelColor(30, 55), QColor(Qt::white));
}

void OFDTest::clipFillRulesAndEmptyIntersection()
{
    auto entries = fixture();
    entries[QStringLiteral("Book/Z/Content.xml")] =
        xml("Page",
            "<Content><Layer><ImageObject ResourceID=\"9\" Boundary=\"0 0 60 60\"><Clips><Clip><Area><Path Fill=\"true\" Stroke=\"false\" "
            "Rule=\"Even-Odd\"><AbbreviatedData>M 0 0 L 60 0 L 60 60 L 0 60 C M 20 20 L 40 20 L 40 40 L 20 40 "
            "C</AbbreviatedData></Path></Area></Clip></Clips></ImageObject></Layer></Content>");
    OFD::Document document;
    QVERIFY(document.open(write(entries)));
    QVERIFY(!document.hasUnsupportedFeatures());
    auto image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(10, 10), QColor(Qt::red));
    QCOMPARE(image.pixelColor(30, 30), QColor(Qt::white));
    entries[QStringLiteral("Book/Z/Content.xml")].replace(
        "</Clips>",
        "<Clip><Area><Path Fill=\"true\" Stroke=\"false\"><AbbreviatedData>M 80 80 L 90 80 L 90 90 C</AbbreviatedData></Path></Area></Clip></Clips>");
    QVERIFY(document.open(write(entries)));
    image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(10, 10), QColor(Qt::white));
    QCOMPARE(image.pixelColor(30, 30), QColor(Qt::white));
}

void OFDTest::textAndStrokeClips()
{
    auto entries = fixture();
    QFile font(QStringLiteral(OFD_TEST_DATA_DIR "/OFDTest.ttf"));
    QVERIFY(font.open(QIODevice::ReadOnly));
    entries[QStringLiteral("Book/Assets/Shapes.ttf")] = font.readAll();
    entries[QStringLiteral("Book/Assets/Res.xml")].replace("</Res>",
                                                           "<Fonts><Font ID=\"45\"><FontFile>Shapes.ttf</FontFile></Font></Fonts><DrawParams><DrawParam "
                                                           "ID=\"100\" LineWidth=\"6\" DashPattern=\"10 10\"/></DrawParams></Res>");
    entries[QStringLiteral("Book/Z/Content.xml")] =
        xml("Page",
            "<Content><Layer><ImageObject ResourceID=\"9\" Boundary=\"10 20 60 60\"><Clips><Clip><Area><Text Font=\"45\" Size=\"20\"><TextCode X=\"10\" "
            "Y=\"30\">A</TextCode></Text></Area></Clip></Clips></ImageObject></Layer></Content>");
    OFD::Document document;
    QVERIFY2(document.open(write(entries)), qPrintable(document.errorString()));
    QVERIFY(!document.hasUnsupportedFeatures());
    auto image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(25, 40), QColor(Qt::red));
    QCOMPARE(image.pixelColor(45, 40), QColor(Qt::white));
    entries[QStringLiteral("Book/Z/Content.xml")] =
        xml("Page",
            "<Content><Layer><ImageObject ResourceID=\"9\" Boundary=\"10 20 60 60\"><Clips><Clip><Area DrawParam=\"100\"><Path><AbbreviatedData>M 5 30 L 55 "
            "30</AbbreviatedData></Path></Area></Clip></Clips></ImageObject></Layer></Content>");
    QVERIFY(document.open(write(entries)));
    QVERIFY(!document.hasUnsupportedFeatures());
    image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(20, 50), QColor(Qt::red));
    QCOMPARE(image.pixelColor(30, 50), QColor(Qt::white));
    QCOMPARE(image.pixelColor(20, 55), QColor(Qt::white));
    entries[QStringLiteral("Book/Z/Content.xml")] = xml("Page",
                                                        "<Content><Layer><TextObject Font=\"45\" Size=\"40\" Boundary=\"10 20 60 60\"><TextCode X=\"0\" "
                                                        "Y=\"40\">A</TextCode><Clips><Clip><Area><Path Fill=\"true\" Stroke=\"false\"><AbbreviatedData>M 0 0 L "
                                                        "16 0 L 16 60 L 0 60 C</AbbreviatedData></Path></Area></Clip></Clips></TextObject></Layer></Content>");
    QVERIFY(document.open(write(entries)));
    QVERIFY(!document.hasUnsupportedFeatures());
    image = document.render(0, QSize(100, 200));
    QCOMPARE(image.pixelColor(20, 40), QColor(Qt::black));
    QCOMPARE(image.pixelColor(35, 40), QColor(Qt::white));
}

void OFDTest::malformed_data()
{
    QTest::addColumn<QString>("fault");
    for (const auto &fault : {"missing-index",
                              "invalid-xml",
                              "doctype",
                              "missing-page",
                              "missing-image",
                              "bad-image",
                              "escape-root",
                              "no-pages",
                              "bad-box",
                              "nan-box",
                              "bad-transform",
                              "bad-path",
                              "bad-text",
                              "bad-base",
                              "missing-resource-id",
                              "missing-font",
                              "deep-xml",
                              "not-zip",
                              "missing-param",
                              "param-cycle",
                              "param-depth",
                              "zero-dash",
                              "negative-dash",
                              "invalid-width",
                              "invalid-cap",
                              "invalid-join",
                              "missing-template",
                              "missing-template-file",
                              "invalid-template-order",
                              "missing-page-resource",
                              "missing-font-file",
                              "escaping-font-file",
                              "page-font-leak",
                              "arc-flag",
                              "arc-arguments",
                              "color-reference",
                              "palette-index",
                              "color-bits",
                              "palette-components",
                              "default-color-reference",
                              "empty-clip",
                              "clip-area-missing",
                              "clip-transform",
                              "clip-boundary"}) {
        QTest::newRow(fault) << QString::fromLatin1(fault);
    }
}

void OFDTest::malformed()
{
    QFETCH(QString, fault);
    auto entries = fixture();
    if (fault == QLatin1String("missing-index")) {
        entries.remove(QStringLiteral("OFD.xml"));
    } else if (fault == QLatin1String("invalid-xml")) {
        entries[QStringLiteral("OFD.xml")] = "<OFD>";
    } else if (fault == QLatin1String("doctype")) {
        entries[QStringLiteral("OFD.xml")].prepend("<!DOCTYPE OFD [<!ENTITY x 'entity'>]>");
    } else if (fault == QLatin1String("missing-page")) {
        entries.remove(QStringLiteral("Book/A/Content.xml"));
    } else if (fault == QLatin1String("missing-image")) {
        entries.remove(QStringLiteral("Book/Assets/Z.png"));
    } else if (fault == QLatin1String("bad-image")) {
        entries[QStringLiteral("Book/Assets/Z.png")] = "invalid image";
    } else if (fault == QLatin1String("escape-root")) {
        entries[QStringLiteral("OFD.xml")].replace("Book/Main.xml", "../../escape.xml");
    } else if (fault == QLatin1String("no-pages")) {
        entries[QStringLiteral("Book/Main.xml")] = xml("Document", "<Pages/>");
    } else if (fault == QLatin1String("bad-box")) {
        entries[QStringLiteral("Book/Main.xml")].replace("0 0 100 200", "0 0 -1 200");
    } else if (fault == QLatin1String("nan-box")) {
        entries[QStringLiteral("Book/Main.xml")].replace("0 0 100 200", "0 0 nan 200");
    } else if (fault == QLatin1String("bad-transform")) {
        entries[QStringLiteral("Book/A/Content.xml")].replace("80 0 0 60 0 0", "1 2 3");
    } else if (fault == QLatin1String("bad-path")) {
        entries[QStringLiteral("Book/A/Content.xml")] =
            xml("Page", "<Content><Layer><PathObject Boundary=\"0 0 5 5\"><AbbreviatedData>M 1</AbbreviatedData></PathObject></Layer></Content>");
    } else if (fault == QLatin1String("bad-text")) {
        entries[QStringLiteral("Book/A/Content.xml")] =
            xml("Page",
                "<Content><Layer><TextObject Boundary=\"0 0 5 5\" Size=\"5\"><TextCode DeltaX=\"g 999999 1\">AB</TextCode></TextObject></Layer></Content>");
    }
    if (fault == QLatin1String("bad-base")) {
        entries[QStringLiteral("Book/Assets/Res.xml")].replace("<Res ", "<Res BaseLoc=\"../../../Escape\" ");
    } else if (fault == QLatin1String("missing-resource-id")) {
        entries[QStringLiteral("Book/A/Content.xml")].replace("ResourceID=\"8\"", "ResourceID=\"999\"");
    } else if (fault == QLatin1String("missing-font")) {
        entries[QStringLiteral("Book/A/Content.xml")] =
            xml("Page", "<Content><Layer><TextObject Font=\"999\" Boundary=\"0 0 5 5\" Size=\"5\"><TextCode>AB</TextCode></TextObject></Layer></Content>");
    } else if (fault == QLatin1String("deep-xml")) {
        entries[QStringLiteral("OFD.xml")] = xml("OFD", QByteArray("<nested>").repeated(150) + QByteArray("</nested>").repeated(150));
    }
    if (fault == QLatin1String("missing-param") || fault == QLatin1String("param-cycle") || fault == QLatin1String("param-depth")) {
        entries[QStringLiteral("Book/A/Content.xml")] = xml("Page",
                                                            "<Content><Layer DrawParam=\"100\"><PathObject Boundary=\"0 0 20 20\"><AbbreviatedData>M 0 1 L 10 "
                                                            "1</AbbreviatedData></PathObject></Layer></Content>");
        if (fault == QLatin1String("param-cycle")) {
            entries[QStringLiteral("Book/Assets/Res.xml")].replace(
                "</Res>",
                "<DrawParams><DrawParam ID=\"100\" Relative=\"101\"/><DrawParam ID=\"101\" Relative=\"100\"/></DrawParams></Res>");
        } else if (fault == QLatin1String("param-depth")) {
            QByteArray params("<DrawParams>");
            for (int i = 100; i < 165; ++i) {
                params += "<DrawParam ID=\"" + QByteArray::number(i) + "\" Relative=\"" + QByteArray::number(i + 1) + "\"/>";
            }
            params += "</DrawParams></Res>";
            entries[QStringLiteral("Book/Assets/Res.xml")].replace("</Res>", params);
        }
    } else if (fault == QLatin1String("zero-dash") || fault == QLatin1String("negative-dash") || fault == QLatin1String("invalid-width")
               || fault == QLatin1String("invalid-cap") || fault == QLatin1String("invalid-join")) {
        const QByteArray attribute = fault == QLatin1String("zero-dash") ? "DashPattern=\"0 0\""
            : fault == QLatin1String("negative-dash")                    ? "DashPattern=\"5 -1\""
            : fault == QLatin1String("invalid-width")                    ? "LineWidth=\"invalid\""
            : fault == QLatin1String("invalid-cap")                      ? "Cap=\"unknown\""
                                                                         : "Join=\"unknown\"";
        entries[QStringLiteral("Book/A/Content.xml")] = xml("Page",
                                                            "<Content><Layer><PathObject Boundary=\"0 0 20 20\" " + attribute
                                                                + "><AbbreviatedData>M 0 1 L 10 1</AbbreviatedData></PathObject></Layer></Content>");
    } else if (fault == QLatin1String("missing-template")) {
        entries[QStringLiteral("Book/A/Content.xml")] = xml("Page", "<Template TemplateID=\"999\"/>");
    } else if (fault == QLatin1String("missing-template-file") || fault == QLatin1String("invalid-template-order")) {
        entries[QStringLiteral("Book/Main.xml")].replace("</CommonData>", "<TemplatePage ID=\"50\" BaseLoc=\"Missing.xml\"/></CommonData>");
        entries[QStringLiteral("Book/A/Content.xml")] = xml("Page", "<Template TemplateID=\"50\" ZOrder=\"unknown\"/>");
        if (fault == QLatin1String("invalid-template-order")) {
            entries[QStringLiteral("Book/Missing.xml")] = xml("Page", "<Content/>");
        }
    } else if (fault == QLatin1String("missing-page-resource")) {
        entries[QStringLiteral("Book/A/Content.xml")] = xml("Page", "<PageRes>Missing.xml</PageRes>");
    } else if (fault == QLatin1String("missing-font-file") || fault == QLatin1String("escaping-font-file")) {
        const QByteArray path = fault == QLatin1String("missing-font-file") ? "Missing.ttf" : "../../../Outside.ttf";
        const QByteArray fontXml = "<Fonts><Font ID=\"45\"><FontFile>" + path + "</FontFile></Font></Fonts></Res>";
        entries[QStringLiteral("Book/Assets/Res.xml")].replace("</Res>", fontXml);
    } else if (fault == QLatin1String("page-font-leak")) {
        entries[QStringLiteral("Book/Z/Content.xml")].replace("<Content>", "<PageRes>Fonts.xml</PageRes><Content>");
        entries[QStringLiteral("Book/Z/Fonts.xml")] = xml("Res", "<Fonts><Font ID=\"45\" FamilyName=\"sans-serif\"/></Fonts>");
        entries[QStringLiteral("Book/A/Content.xml")] = xml("Page",
                                                            "<Content><Layer><TextObject Font=\"45\" Size=\"20\" Boundary=\"0 0 40 40\"><TextCode X=\"0\" "
                                                            "Y=\"20\">AB</TextCode></TextObject></Layer></Content>");
    }
    if (fault == QLatin1String("arc-flag") || fault == QLatin1String("arc-arguments")) {
        const QByteArray data = fault == QLatin1String("arc-flag") ? "M 10 10 A 20 20 0 2 1 50 50" : "M 10 10 A 20 20 0";
        entries[QStringLiteral("Book/A/Content.xml")] =
            xml("Page", "<Content><Layer><PathObject Boundary=\"0 0 60 60\"><AbbreviatedData>" + data + "</AbbreviatedData></PathObject></Layer></Content>");
    } else if (fault == QLatin1String("color-reference") || fault == QLatin1String("palette-index")) {
        if (fault == QLatin1String("palette-index")) {
            entries[QStringLiteral("Book/Assets/Res.xml")].replace(
                "</Res>",
                "<ColorSpaces><ColorSpace ID=\"999\" Type=\"RGB\"><Palette><CV>255 0 0</CV></Palette></ColorSpace></ColorSpaces></Res>");
        }
        entries[QStringLiteral("Book/A/Content.xml")] =
            xml("Page",
                "<Content><Layer><PathObject Boundary=\"0 0 60 60\" Fill=\"true\" Stroke=\"false\"><FillColor ColorSpace=\"999\" "
                "Index=\"5\"/><AbbreviatedData>M 0 0 L 40 0 L 40 40 C</AbbreviatedData></PathObject></Layer></Content>");
    } else if (fault == QLatin1String("color-bits") || fault == QLatin1String("palette-components")) {
        const QByteArray space = fault == QLatin1String("color-bits") ? "<ColorSpace ID=\"1\" Type=\"RGB\" BitsPerComponent=\"3\"/>"
                                                                      : "<ColorSpace ID=\"1\" Type=\"CMYK\"><Palette><CV>0 0 255</CV></Palette></ColorSpace>";
        const QByteArray colorResources = "<ColorSpaces>" + space + "</ColorSpaces></Res>";
        entries[QStringLiteral("Book/Assets/Res.xml")].replace("</Res>", colorResources);
    } else if (fault == QLatin1String("default-color-reference")) {
        entries[QStringLiteral("Book/Main.xml")].replace("</CommonData>", "<DefaultCS>999</DefaultCS></CommonData>");
    } else if (fault == QLatin1String("empty-clip") || fault == QLatin1String("clip-area-missing") || fault == QLatin1String("clip-transform")
               || fault == QLatin1String("clip-boundary")) {
        QByteArray area;
        if (fault == QLatin1String("clip-area-missing")) {
            area = "<Area/>";
        } else if (fault == QLatin1String("clip-transform")) {
            area = "<Area CTM=\"1 2 3\"><Path Fill=\"true\" Stroke=\"false\"><AbbreviatedData>M 0 0 L 10 0 L 10 10 C</AbbreviatedData></Path></Area>";
        } else if (fault == QLatin1String("clip-boundary")) {
            area = "<Area><Path Boundary=\"0 0 -1 10\" Fill=\"true\" Stroke=\"false\"><AbbreviatedData>M 0 0 L 10 0 L 10 10 C</AbbreviatedData></Path></Area>";
        }
        entries[QStringLiteral("Book/A/Content.xml")] = xml("Page",
                                                            "<Content><Layer><ImageObject ResourceID=\"8\" Boundary=\"0 0 60 60\"><Clips><Clip>" + area
                                                                + "</Clip></Clips></ImageObject></Layer></Content>");
    }
    OFD::Document document;
    const auto good = write(fixture());
    QVERIFY(document.open(good));
    const QString broken = write(entries);
    if (fault == QLatin1String("not-zip")) {
        QFile invalid(broken);
        QVERIFY(invalid.open(QIODevice::WriteOnly | QIODevice::Truncate));
        invalid.write("not an archive");
        invalid.close();
    }
    QVERIFY(!document.open(broken));
    QVERIFY(!document.errorString().isEmpty());
    QCOMPARE(document.pageCount(), 0);
    QVERIFY(document.open(good));
    QCOMPARE(document.pageCount(), 2);
}

void OFDTest::unsupportedFeatures()
{
    auto entries = fixture();
    entries[QStringLiteral("Book/A/Content.xml")] = xml("Page", "<Content><Layer><CompositeObject Boundary=\"0 0 10 10\"/></Layer></Content>");
    OFD::Document document;
    const auto file = write(entries);
    QVERIFY(document.open(file));
    QVERIFY(document.hasUnsupportedFeatures());
    const auto result = KPluginFactory::instantiatePlugin<Okular::Generator>(KPluginMetaData(QStringLiteral(GENERATOR_PATH)));
    QVERIFY2(result, qPrintable(result.errorString));
    std::unique_ptr<Okular::Generator> generator(result.plugin);
    QSignalSpy warnings(generator.get(), &Okular::Generator::warning);
    QList<Okular::Page *> pages;
    QVERIFY(generator->loadDocument(file, pages));
    QCOMPARE(warnings.count(), 1);
    QCOMPARE(pages.size(), 2);
    QVERIFY(generator->closeDocument());
    qDeleteAll(pages);
}

void OFDTest::pluginIntegration()
{
    const auto file = write(fixture());
    const auto mime = QMimeDatabase().mimeTypeForFile(file);
    QCOMPARE(mime.name(), QStringLiteral("application/ofd"));
    const KPluginMetaData metadata(QStringLiteral(GENERATOR_PATH));
    QVERIFY(metadata.mimeTypes().contains(mime.name()));
    const auto result = KPluginFactory::instantiatePlugin<Okular::Generator>(metadata);
    QVERIFY2(result, qPrintable(result.errorString));
    std::unique_ptr<Okular::Generator> generator(result.plugin);
    for (int attempt = 0; attempt < 2; ++attempt) {
        QList<Okular::Page *> pages;
        QVERIFY(generator->loadDocument(file, pages));
        QCOMPARE(pages.size(), 2);
        QVERIFY(qAbs(pages[0]->width() - 100 * 72.0 / 25.4) < 0.01);
        QVERIFY(qAbs(pages[1]->height() - 60 * 72.0 / 25.4) < 0.01);
        QVERIFY(generator->closeDocument());
        qDeleteAll(pages);
    }
    QSignalSpy errors(generator.get(), &Okular::Generator::error);
    QList<Okular::Page *> pages;
    QVERIFY(!generator->loadDocument(m_dir.filePath(QStringLiteral("missing.ofd")), pages));
    QVERIFY(pages.isEmpty());
    QCOMPARE(errors.count(), 1);
}

QTEST_MAIN(OFDTest)
#include "ofdtest.moc"
