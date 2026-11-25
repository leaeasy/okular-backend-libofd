/*
    SPDX-FileCopyrightText: 2026 OFD plugin contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "document.h"
#include "geometry.h"

#include <KArchiveDirectory>
#include <KArchiveFile>
#include <KZip>

#include <QBuffer>
#include <QDir>
#include <QDomDocument>
#include <QFont>
#include <QFontMetricsF>
#include <QImageReader>
#include <QPainter>
#include <QPainterPathStroker>
#include <QRawFont>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <memory>

namespace OFD
{
namespace
{
constexpr qint64 maxEntrySize = 32 * 1024 * 1024;
constexpr qint64 maxDocumentSize = 256 * 1024 * 1024;
constexpr qint64 maxImagePixels = 32 * 1024 * 1024;
constexpr int maxObjects = 100000;

QString name(const QDomElement &element)
{
    return element.localName();
}

QDomElement child(const QDomElement &parent, const QString &localName)
{
    for (auto e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        if (name(e) == localName) {
            return e;
        }
    }
    return {};
}

bool numbers(const QString &text, QList<double> &values)
{
    values.clear();
    const auto parts = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const auto &part : parts) {
        bool ok;
        const double value = part.toDouble(&ok);
        if (!ok || !std::isfinite(value) || std::abs(value) > 1e9) {
            return false;
        }
        values.append(value);
    }
    return true;
}

bool box(const QString &text, QRectF &rect)
{
    QList<double> values;
    if (!numbers(text, values) || values.size() != 4 || values[2] <= 0 || values[3] <= 0 || values[2] > 100000 || values[3] > 100000) {
        return false;
    }
    rect = QRectF(values[0], values[1], values[2], values[3]);
    return true;
}

// Locations are archive paths, never filesystem paths. Absolute OFD locations
// start at the archive root; relative locations start beside the referring XML.
QString location(const QString &baseFile, const QString &reference)
{
    if (reference.isEmpty() || reference.contains(QLatin1Char('\\')) || reference.contains(QLatin1Char(':'))) {
        return {};
    }
    const QString path =
        QDir::cleanPath(reference.startsWith(QLatin1Char('/')) ? reference.mid(1) : baseFile.left(baseFile.lastIndexOf(QLatin1Char('/')) + 1) + reference);
    if (path == QLatin1String("..") || path.startsWith(QLatin1String("../")) || path.startsWith(QLatin1Char('/')) || path == QLatin1String(".")) {
        return {};
    }
    return path;
}

bool enabled(const QDomElement &element, const QString &attribute, bool defaultValue)
{
    return element.hasAttribute(attribute) ? element.attribute(attribute) == QLatin1String("true") || element.attribute(attribute) == QLatin1String("1")
                                           : defaultValue;
}
}

class Document::Parser
{
public:
    explicit Parser(Document &document, const QString &fileName)
        : doc(document)
        , archive(fileName)
    {
    }
    bool parse();

private:
    QByteArray read(const QString &path);
    QDomElement xml(const QString &path, const QString &rootName);
    struct Font {
        QString family;
        QRawFont embedded;
    };
    struct ColorSpace {
        QString type = QStringLiteral("RGB");
        int bits = 8;
        QList<QList<double>> palette;
    };
    struct Resources {
        QHash<QString, QImage> images;
        QHash<QString, Font> fonts;
        QHash<QString, ColorSpace> colors;
        QString defaultColorSpace;
        QHash<QString, QDomElement> drawParams;
    } res;
    struct Style {
        QHash<QString, QString> attributes;
        QDomElement fill;
        QDomElement stroke;
    };
    bool resources(const QString &path);
    void overlay(const QDomElement &element, Style &style);
    bool drawParameters(const QString &id, Style &style);
    bool styled(const QDomElement &element, const Style &inherited, Style &style);
    bool stroke(const Style &style, QPen &pen);
    bool content(const QDomElement &parent, Page &page, const Style &inherited, int depth = 0);
    bool object(const QDomElement &element, Page &page, const Style &inherited);
    bool pageResources(const QDomElement &root, const QString &pagePath);
    bool pageContent(const QDomElement &root, Page &page);
    int order(const QString &type);
    bool path(const QString &data, QPainterPath &result);
    bool text(const QDomElement &element, QPainterPath &result);
    QColor color(const QDomElement &element);
    bool colorValues(const QString &data, const ColorSpace &space, QList<double> &values);
    bool colorSpace(const QDomElement &element);
    bool clips(const QDomElement &element, Object &object, const Style &style);
    bool clipArea(const QDomElement &area, QPainterPath &result, const Style &inherited);
    bool transform(const QDomElement &element, QTransform &matrix);
    bool fail(const QString &message);

    Document &doc;
    KZip archive;

    qint64 bytesRead = 0;
    qint64 imagePixels = 0;
    int objectCount = 0;
    qsizetype pathTokens = 0;
    qsizetype glyphCount = 0;
    int clipAreas = 0;
};

bool Document::Parser::fail(const QString &message)
{
    if (doc.m_error.isEmpty()) {
        doc.m_error = message;
    }
    return false;
}

QByteArray Document::Parser::read(const QString &path)
{
    const auto *entry = path.isEmpty() ? nullptr : archive.directory()->entry(path);
    if (!entry || !entry->isFile() || !entry->symLinkTarget().isEmpty()) {
        fail(QStringLiteral("Missing or invalid OFD entry: %1").arg(path));
        return {};
    }
    const auto *file = static_cast<const KArchiveFile *>(entry);
    if (file->size() <= 0 || file->size() > maxEntrySize || bytesRead + file->size() > maxDocumentSize) {
        fail(QStringLiteral("OFD entry exceeds the supported size limit: %1").arg(path));
        return {};
    }
    std::unique_ptr<QIODevice> device(file->createDevice());
    const QByteArray data = device ? device->read(maxEntrySize + 1) : QByteArray();
    if (data.size() != file->size()) {
        fail(QStringLiteral("Cannot read OFD entry: %1").arg(path));
        return {};
    }
    bytesRead += data.size();
    return data;
}

QDomElement Document::Parser::xml(const QString &path, const QString &rootName)
{
    const auto data = read(path);
    // Reject DTDs and entities before building a DOM; OFD has no need for them.
    QXmlStreamReader stream(data);
    int depth = 0;
    int tokens = 0;
    while (!stream.atEnd()) {
        const auto token = stream.readNext();
        if (++tokens > 1000000 || (token == QXmlStreamReader::StartElement && ++depth > 128)) {
            fail(QStringLiteral("OFD XML exceeds the supported complexity limit"));
            return {};
        }
        if (token == QXmlStreamReader::EndElement) {
            --depth;
        }
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
            fail(QStringLiteral("Unsupported XML entity or DTD in %1").arg(path));
            return {};
        }
    }
    QDomDocument dom;
    if (stream.hasError() || !dom.setContent(data, QDomDocument::ParseOption::UseNamespaceProcessing)) {
        fail(QStringLiteral("Invalid OFD XML: %1").arg(path));
        return {};
    }
    const auto root = dom.documentElement();
    if (name(root) != rootName
        || (root.namespaceURI() != QLatin1String("http://www.ofdspec.org/2016") && root.namespaceURI() != QLatin1String("http://www.ofdspec.org"))) {
        fail(QStringLiteral("Unexpected OFD XML root in %1").arg(path));
        return {};
    }
    return root;
}

bool Document::Parser::resources(const QString &path)
{
    const auto root = xml(path, QStringLiteral("Res"));
    if (root.isNull()) {
        return false;
    }
    const QString base = location(path, root.attribute(QStringLiteral("BaseLoc"), QStringLiteral(".")) + QStringLiteral("/placeholder"));
    if (base.isEmpty()) {
        return fail(QStringLiteral("Invalid OFD resource base location"));
    }
    for (auto group = root.firstChildElement(); !group.isNull(); group = group.nextSiblingElement()) {
        for (auto resource = group.firstChildElement(); !resource.isNull(); resource = resource.nextSiblingElement()) {
            const QString id = resource.attribute(QStringLiteral("ID"));
            if (name(resource) == QLatin1String("MultiMedia") && resource.attribute(QStringLiteral("Type")) == QLatin1String("Image")) {
                const auto data = read(location(base, child(resource, QStringLiteral("MediaFile")).text().trimmed()));
                if (!doc.m_error.isEmpty()) {
                    return false;
                }
                QBuffer buffer;
                buffer.setData(data);
                buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer);
                const auto size = reader.size();
                const qint64 pixels = qint64(size.width()) * size.height();
                if (!size.isValid() || pixels > maxImagePixels || imagePixels + pixels > maxImagePixels * 2) {
                    return fail(QStringLiteral("OFD image exceeds the supported size limit"));
                }
                const auto image = reader.read();
                if (image.isNull()) {
                    return fail(QStringLiteral("Cannot decode OFD image resource %1").arg(id));
                }
                imagePixels += pixels;
                res.images.insert(id, image);
            } else if (name(resource) == QLatin1String("Font")) {
                Font font;
                font.family = resource.attribute(QStringLiteral("FamilyName"), resource.attribute(QStringLiteral("FontName")));
                const auto file = child(resource, QStringLiteral("FontFile"));
                if (!file.isNull()) {
                    const auto data = read(location(base, file.text().trimmed()));
                    if (!doc.m_error.isEmpty()) {
                        return false;
                    }
                    font.embedded.loadFromData(data, 1000, QFont::PreferNoHinting);
                    if (!font.embedded.isValid()) {
                        // OFD may embed fonts in formats unsupported by Qt. Keep
                        // the system-font fallback, but never hide the substitution.
                        doc.m_unsupported = true;
                    }
                }
                res.fonts.insert(id, font);
            } else if (name(resource) == QLatin1String("DrawParam")) {
                res.drawParams.insert(id, resource);
            } else if (name(resource) == QLatin1String("ColorSpace")) {
                if (!colorSpace(resource)) {
                    return false;
                }
            } else {
                doc.m_unsupported = true;
            }
        }
    }
    return doc.m_error.isEmpty();
}

bool Document::Parser::colorValues(const QString &data, const ColorSpace &space, QList<double> &values)
{
    values.clear();
    const auto parts = data.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const int channels = space.type == QLatin1String("CMYK") ? 4 : space.type == QLatin1String("GRAY") ? 1 : 3;
    if (parts.size() != channels) {
        return false;
    }
    const uint maximum = (1u << space.bits) - 1;
    for (const auto &part : parts) {
        bool ok;
        const uint value = part.startsWith(QLatin1Char('#')) ? part.mid(1).toUInt(&ok, 16) : part.toUInt(&ok, 10);
        if (!ok || value > maximum) {
            return false;
        }
        values.append(double(value) / maximum);
    }
    return true;
}

bool Document::Parser::colorSpace(const QDomElement &element)
{
    ColorSpace space;
    space.type = element.attribute(QStringLiteral("Type"));
    bool ok;
    space.bits = element.attribute(QStringLiteral("BitsPerComponent"), QStringLiteral("8")).toInt(&ok);
    if (!ok || (space.bits != 1 && space.bits != 2 && space.bits != 4 && space.bits != 8 && space.bits != 16)
        || (space.type != QLatin1String("RGB") && space.type != QLatin1String("GRAY") && space.type != QLatin1String("CMYK"))) {
        return fail(QStringLiteral("Invalid OFD color space"));
    }
    const auto palette = child(element, QStringLiteral("Palette"));
    for (auto entry = palette.firstChildElement(); !entry.isNull(); entry = entry.nextSiblingElement()) {
        QList<double> values;
        if (name(entry) != QLatin1String("CV") || !colorValues(entry.text(), space, values) || space.palette.size() >= 65536) {
            return fail(QStringLiteral("Invalid OFD color palette"));
        }
        space.palette.append(values);
    }
    if (!child(element, QStringLiteral("Profile")).isNull()) {
        doc.m_unsupported = true; // Device colors are rendered without ICC conversion.
    }
    res.colors.insert(element.attribute(QStringLiteral("ID")), space);
    return true;
}

QColor Document::Parser::color(const QDomElement &element)
{
    if (element.isNull()) {
        return Qt::black;
    }
    const QString id = element.attribute(QStringLiteral("ColorSpace"), res.defaultColorSpace);
    if (!id.isEmpty() && !res.colors.contains(id)) {
        fail(QStringLiteral("Missing OFD color space %1").arg(id));
        return Qt::black;
    }
    ColorSpace space = res.colors.value(id);
    // Preserve implicit gray values used by existing image-only producers.
    if (id.isEmpty() && element.attribute(QStringLiteral("Value")).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size() == 1) {
        space.type = QStringLiteral("GRAY");
    }
    QList<double> values;
    if (element.hasAttribute(QStringLiteral("Value"))) {
        if (!colorValues(element.attribute(QStringLiteral("Value")), space, values)) {
            doc.m_unsupported = true;
            values.clear();
        }
    } else if (element.hasAttribute(QStringLiteral("Index"))) {
        bool ok;
        const uint index = element.attribute(QStringLiteral("Index")).toUInt(&ok);
        if (!ok || index >= uint(space.palette.size())) {
            fail(QStringLiteral("Invalid OFD palette index"));
            return Qt::black;
        }
        values = space.palette[index];
    }
    if (values.isEmpty()) {
        values.fill(0, space.type == QLatin1String("CMYK") ? 4 : space.type == QLatin1String("GRAY") ? 1 : 3);
    }
    QColor result = space.type == QLatin1String("CMYK") ? QColor::fromCmykF(values[0], values[1], values[2], values[3])
        : space.type == QLatin1String("GRAY")           ? QColor::fromRgbF(values[0], values[0], values[0])
                                                        : QColor::fromRgbF(values[0], values[1], values[2]);
    bool ok;
    const int alpha = element.attribute(QStringLiteral("Alpha"), QStringLiteral("255")).toInt(&ok);
    if (!ok || alpha < 0 || alpha > 255) {
        doc.m_unsupported = true;
    } else {
        result.setAlpha(alpha);
    }
    if (!element.firstChildElement().isNull()) {
        doc.m_unsupported = true;
    }
    return result;
}

bool Document::Parser::path(const QString &data, QPainterPath &result)
{
    const auto tokens = data.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    pathTokens += tokens.size();
    if (pathTokens > 1000000) {
        return fail(QStringLiteral("OFD paths exceed the supported complexity limit"));
    }
    int index = 0;
    while (index < tokens.size()) {
        const QString op = tokens[index++];
        const int count = op == QLatin1String("M") || op == QLatin1String("S") || op == QLatin1String("L") ? 2
            : op == QLatin1String("Q")                                                                     ? 4
            : op == QLatin1String("B")                                                                     ? 6
            : op == QLatin1String("A")                                                                     ? 7
            : op == QLatin1String("C")                                                                     ? 0
                                                                                                           : -1;
        if (count < 0) {
            doc.m_unsupported = true;
            result = {};
            return true;
        }
        QList<double> v;
        if (index + count > tokens.size() || !numbers(tokens.mid(index, count).join(QLatin1Char(' ')), v)) {
            return fail(QStringLiteral("Invalid OFD path data"));
        }
        index += count;
        if (op == QLatin1String("M") || op == QLatin1String("S")) {
            result.moveTo(v[0], v[1]);
        } else if (op == QLatin1String("L")) {
            result.lineTo(v[0], v[1]);
        } else if (op == QLatin1String("Q")) {
            result.quadTo(v[0], v[1], v[2], v[3]);
        } else if (op == QLatin1String("B")) {
            result.cubicTo(v[0], v[1], v[2], v[3], v[4], v[5]);
        } else if (op == QLatin1String("A")) {
            if (!appendArc(result, v)) {
                return fail(QStringLiteral("Invalid OFD elliptical arc"));
            }
        } else {
            result.closeSubpath();
        }
    }
    return true;
}

bool Document::Parser::text(const QDomElement &element, QPainterPath &result)
{
    bool ok;
    const double size = element.attribute(QStringLiteral("Size")).toDouble(&ok);
    if (!ok || !std::isfinite(size) || size <= 0 || size > 1000) {
        return fail(QStringLiteral("Invalid OFD text size"));
    }
    const QString fontId = element.attribute(QStringLiteral("Font"));
    if (!fontId.isEmpty() && !res.fonts.contains(fontId)) {
        return fail(QStringLiteral("Missing OFD font resource %1").arg(fontId));
    }
    const auto fontResource = res.fonts.value(fontId);
    QFont font(fontResource.family);
    font.setPixelSize(1000);
    font.setItalic(enabled(element, QStringLiteral("Italic"), false));
    font.setBold(element.attribute(QStringLiteral("Weight"), QStringLiteral("400")).toInt() >= 700);
    if (fontResource.embedded.isValid()
        && ((font.italic() && fontResource.embedded.style() == QFont::StyleNormal) || (font.bold() && fontResource.embedded.weight() < QFont::Bold))) {
        doc.m_unsupported = true; // Synthetic styling is not applied to raw outlines.
    }
    const double scale = size / 1000;
    const QFontMetricsF metrics(font);
    double x = 0;
    double y = 0;
    for (auto code = element.firstChildElement(); !code.isNull(); code = code.nextSiblingElement()) {
        if (name(code) != QLatin1String("TextCode")) {
            if (name(code) != QLatin1String("FillColor") && name(code) != QLatin1String("StrokeColor") && name(code) != QLatin1String("Clips")) {
                doc.m_unsupported = true;
            }
            continue;
        }
        if (code.hasAttribute(QStringLiteral("X"))) {
            x = code.attribute(QStringLiteral("X")).toDouble(&ok);
            if (!ok || !std::isfinite(x)) {
                return fail(QStringLiteral("Invalid OFD text position"));
            }
        }
        if (code.hasAttribute(QStringLiteral("Y"))) {
            y = code.attribute(QStringLiteral("Y")).toDouble(&ok);
            if (!ok || !std::isfinite(y)) {
                return fail(QStringLiteral("Invalid OFD text position"));
            }
        }
        QList<double> dx, dy;
        // 'g count value' repeats an advance; cap expansion by the text length.
        const auto unicode = code.text().toUcs4();
        glyphCount += unicode.size();
        if (glyphCount > 100000) {
            return fail(QStringLiteral("OFD text exceeds the supported size limit"));
        }
        auto deltas = [&](const QString &attribute, QList<double> &out) {
            const auto parts = code.attribute(attribute).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            for (int i = 0; i < parts.size(); ++i) {
                int repeat = 1;
                if (parts[i] == QLatin1String("g")) {
                    if (i + 2 >= parts.size()) {
                        return false;
                    }
                    repeat = parts[++i].toInt(&ok);
                    if (!ok || repeat < 1 || repeat > unicode.size()) {
                        return false;
                    }
                    ++i;
                }
                QList<double> value;
                if (!numbers(parts[i], value) || value.size() != 1 || out.size() + repeat > unicode.size()) {
                    return false;
                }
                for (int n = 0; n < repeat; ++n) {
                    out.append(value[0]);
                }
            }
            return true;
        };
        if (!deltas(QStringLiteral("DeltaX"), dx) || !deltas(QStringLiteral("DeltaY"), dy)) {
            return fail(QStringLiteral("Invalid OFD text advances"));
        }
        for (int i = 0; i < unicode.size(); ++i) {
            const char32_t codepoint = unicode[i];
            const QString glyph = QString::fromUcs4(&codepoint, 1);
            QPainterPath glyphPath;
            double advance = metrics.horizontalAdvance(glyph);
            if (fontResource.embedded.isValid()) {
                const auto indexes = fontResource.embedded.glyphIndexesForString(glyph);
                if (indexes.size() != 1 || indexes[0] == 0) {
                    doc.m_unsupported = true;
                    glyphPath.addText(QPointF(), font, glyph);
                } else {
                    glyphPath = fontResource.embedded.pathForGlyph(indexes[0]);
                    const auto advances = fontResource.embedded.advancesForGlyphIndexes(indexes);
                    if (advances.size() != 1) {
                        return fail(QStringLiteral("Invalid OFD embedded font metrics"));
                    }
                    advance = advances[0].x();
                }
            } else {
                glyphPath.addText(QPointF(), font, glyph);
            }
            result.addPath(QTransform(scale, 0, 0, scale, x, y).map(glyphPath));
            x += dx.isEmpty() ? advance * scale : dx[qMin(i, int(dx.size()) - 1)];
            y += dy.isEmpty() ? 0 : dy[qMin(i, int(dy.size()) - 1)];
        }
    }
    return true;
}

void Document::Parser::overlay(const QDomElement &element, Style &style)
{
    for (const auto &attribute : {"LineWidth", "Cap", "Join", "MiterLimit", "DashPattern", "DashOffset"}) {
        const auto key = QString::fromLatin1(attribute);
        if (element.hasAttribute(key)) {
            style.attributes.insert(key, element.attribute(key));
        }
    }
    const auto fill = child(element, QStringLiteral("FillColor"));
    const auto stroke = child(element, QStringLiteral("StrokeColor"));
    if (!fill.isNull()) {
        style.fill = fill;
    }
    if (!stroke.isNull()) {
        style.stroke = stroke;
    }
}

bool Document::Parser::drawParameters(const QString &id, Style &style)
{
    QList<QDomElement> chain;
    QSet<QString> seen;
    QString current = id;
    while (!current.isEmpty()) {
        if (seen.contains(current) || seen.size() >= 64) {
            return fail(QStringLiteral("Cyclic or excessive OFD drawing parameter inheritance"));
        }
        const auto parameter = res.drawParams.value(current);
        if (parameter.isNull()) {
            return fail(QStringLiteral("Missing OFD drawing parameter %1").arg(current));
        }
        seen.insert(current);
        chain.prepend(parameter);
        current = parameter.attribute(QStringLiteral("Relative"));
    }
    for (const auto &parameter : chain) {
        overlay(parameter, style);
    }
    return true;
}

bool Document::Parser::styled(const QDomElement &element, const Style &inherited, Style &style)
{
    style = inherited;
    if (element.hasAttribute(QStringLiteral("DrawParam")) && !drawParameters(element.attribute(QStringLiteral("DrawParam")), style)) {
        return false;
    }
    overlay(element, style);
    return true;
}

bool Document::Parser::stroke(const Style &style, QPen &pen)
{
    const auto scalar = [&](const QString &attribute, const QString &defaultValue, double &value) {
        QList<double> values;
        if (!numbers(style.attributes.value(attribute, defaultValue), values) || values.size() != 1) {
            return false;
        }
        value = values[0];
        return true;
    };
    double width, limit, offset;
    if (!scalar(QStringLiteral("LineWidth"), QStringLiteral("0.353"), width) || width < 0 || width > 100000
        || !scalar(QStringLiteral("MiterLimit"), QStringLiteral("3.528"), limit) || limit <= 0
        || !scalar(QStringLiteral("DashOffset"), QStringLiteral("0"), offset)) {
        return fail(QStringLiteral("Invalid OFD stroke dimensions"));
    }
    pen = QPen(color(style.stroke), width);
    const auto cap = style.attributes.value(QStringLiteral("Cap"), QStringLiteral("Butt"));
    const auto join = style.attributes.value(QStringLiteral("Join"), QStringLiteral("Miter"));
    if (cap != QLatin1String("Butt") && cap != QLatin1String("Round") && cap != QLatin1String("Square")) {
        return fail(QStringLiteral("Invalid OFD line cap"));
    }
    if (join != QLatin1String("Miter") && join != QLatin1String("Round") && join != QLatin1String("Bevel")) {
        return fail(QStringLiteral("Invalid OFD line join"));
    }
    pen.setCapStyle(cap == QLatin1String("Round") ? Qt::RoundCap : cap == QLatin1String("Square") ? Qt::SquareCap : Qt::FlatCap);
    pen.setJoinStyle(join == QLatin1String("Round") ? Qt::RoundJoin : join == QLatin1String("Bevel") ? Qt::BevelJoin : Qt::MiterJoin);
    // OFD's miter length is in millimetres; QPen uses multiples of pen width.
    pen.setMiterLimit(width > 0 ? limit / width : limit);
    if (style.attributes.contains(QStringLiteral("DashPattern"))) {
        QList<double> values;
        if (!numbers(style.attributes.value(QStringLiteral("DashPattern")), values) || values.isEmpty() || values.size() > 128) {
            return fail(QStringLiteral("Invalid OFD dash pattern"));
        }
        double total = 0;
        for (const auto value : values) {
            if (value < 0) {
                return fail(QStringLiteral("Invalid OFD dash length"));
            }
            total += value;
        }
        if (total <= 0) {
            return fail(QStringLiteral("Empty OFD dash pattern"));
        }
        if (width == 0) {
            doc.m_unsupported = true; // Device-dependent hairline dash lengths.
        } else {
            QList<qreal> pattern;
            for (const auto value : values) {
                pattern.append(value / width);
            }
            if (pattern.size() % 2) {
                pattern.append(pattern);
            }
            pen.setDashPattern(pattern);
            pen.setDashOffset(offset / width);
        }
    }
    return true;
}

int Document::Parser::order(const QString &type)
{
    if (type == QLatin1String("Background")) {
        return 1;
    }
    if (type == QLatin1String("Body")) {
        return 3;
    }
    if (type == QLatin1String("Foreground")) {
        return 5;
    }
    fail(QStringLiteral("Invalid OFD layer order"));
    return -1;
}

bool Document::Parser::pageResources(const QDomElement &root, const QString &pagePath)
{
    for (auto entry = root.firstChildElement(); !entry.isNull(); entry = entry.nextSiblingElement()) {
        if (name(entry) == QLatin1String("PageRes") && !resources(location(pagePath, entry.text().trimmed()))) {
            return false;
        }
    }
    return true;
}

bool Document::Parser::pageContent(const QDomElement &root, Page &page)
{
    struct Layer {
        int order;
        QDomElement element;
    };
    QList<Layer> layers;
    const auto contents = child(root, QStringLiteral("Content"));
    for (auto layer = contents.firstChildElement(); !layer.isNull(); layer = layer.nextSiblingElement()) {
        if (name(layer) != QLatin1String("Layer")) {
            doc.m_unsupported = true;
            continue;
        }
        const int rank = order(layer.attribute(QStringLiteral("Type"), QStringLiteral("Body")));
        if (rank < 0) {
            return false;
        }
        layers.append({rank, layer});
    }
    std::stable_sort(layers.begin(), layers.end(), [](const Layer &a, const Layer &b) {
        return a.order < b.order;
    });
    for (const auto &layer : layers) {
        Style style;
        if (!styled(layer.element, Style(), style) || !content(layer.element, page, style)) {
            return false;
        }
        if (layer.element.hasAttribute(QStringLiteral("CTM")) || layer.element.hasAttribute(QStringLiteral("Boundary"))) {
            doc.m_unsupported = true;
        }
    }
    // A template cannot recursively apply other templates.
    if (!child(root, QStringLiteral("Template")).isNull()) {
        doc.m_unsupported = true;
    }
    return true;
}

bool Document::Parser::transform(const QDomElement &element, QTransform &matrix)
{
    matrix = QTransform();
    if (!element.hasAttribute(QStringLiteral("CTM"))) {
        return true;
    }
    QList<double> values;
    if (!numbers(element.attribute(QStringLiteral("CTM")), values) || values.size() != 6) {
        return fail(QStringLiteral("Invalid OFD transform"));
    }
    matrix = QTransform(values[0], values[1], values[2], values[3], values[4], values[5]);
    return true;
}

bool Document::Parser::clipArea(const QDomElement &area, QPainterPath &result, const Style &inherited)
{
    if (++clipAreas > 4096) {
        return fail(QStringLiteral("OFD clipping exceeds the supported complexity limit"));
    }
    const auto shape = area.firstChildElement();
    if (shape.isNull() || !shape.nextSiblingElement().isNull()) {
        return fail(QStringLiteral("Invalid OFD clip area"));
    }
    const bool isText = name(shape) == QLatin1String("Text");
    if (!isText && name(shape) != QLatin1String("Path")) {
        doc.m_unsupported = true;
        return true;
    }
    QPainterPath geometry;
    if (isText ? !text(shape, geometry) : !path(child(shape, QStringLiteral("AbbreviatedData")).text(), geometry)) {
        return false;
    }
    geometry.setFillRule(shape.attribute(QStringLiteral("Rule")) == QLatin1String("Even-Odd") ? Qt::OddEvenFill : Qt::WindingFill);
    Style areaStyle, style;
    if (!styled(area, inherited, areaStyle) || !styled(shape, areaStyle, style)) {
        return false;
    }
    if (enabled(shape, QStringLiteral("Fill"), isText)) {
        result = geometry;
    }
    if (enabled(shape, QStringLiteral("Stroke"), !isText)) {
        QPen pen;
        if (!stroke(style, pen)) {
            return false;
        }
        if (pen.widthF() == 0) {
            doc.m_unsupported = true; // A device hairline has no fixed geometric area.
        } else {
            QPainterPathStroker stroker(pen);
            result = result.united(stroker.createStroke(geometry));
        }
    }
    QTransform shapeMatrix, areaMatrix;
    if (!transform(shape, shapeMatrix) || !transform(area, areaMatrix)) {
        return false;
    }
    result = shapeMatrix.map(result);
    if (shape.hasAttribute(QStringLiteral("Boundary"))) {
        QRectF boundary;
        if (!box(shape.attribute(QStringLiteral("Boundary")), boundary)) {
            return fail(QStringLiteral("Invalid OFD clip boundary"));
        }
        result.translate(boundary.topLeft());
        QPainterPath bounds;
        bounds.addRect(boundary);
        result = result.intersected(bounds);
    }
    result = areaMatrix.map(result);
    if (!child(shape, QStringLiteral("Clips")).isNull()) {
        doc.m_unsupported = true;
    }
    return true;
}

bool Document::Parser::clips(const QDomElement &element, Object &object, const Style &style)
{
    if (element.isNull()) {
        return true;
    }
    object.transformClips = enabled(element, QStringLiteral("TransFlag"), false);
    for (auto clip = element.firstChildElement(); !clip.isNull(); clip = clip.nextSiblingElement()) {
        if (name(clip) != QLatin1String("Clip") || clip.firstChildElement().isNull() || object.clips.size() >= 256) {
            return fail(QStringLiteral("Invalid OFD clip sequence"));
        }
        QPainterPath combined;
        for (auto area = clip.firstChildElement(); !area.isNull(); area = area.nextSiblingElement()) {
            QPainterPath shape;
            if (name(area) != QLatin1String("Area") || !clipArea(area, shape, style)) {
                return fail(QStringLiteral("Invalid OFD clip area"));
            }
            combined = combined.united(shape);
        }
        object.clips.append(combined);
    }
    return true;
}

bool Document::Parser::object(const QDomElement &element, Page &page, const Style &inherited)
{
    if (++objectCount > maxObjects) {
        return fail(QStringLiteral("Too many OFD objects"));
    }
    Object object;
    if (!box(element.attribute(QStringLiteral("Boundary")), object.boundary)) {
        return fail(QStringLiteral("Invalid OFD object boundary"));
    }
    if (!transform(element, object.transform)) {
        return false;
    }
    object.opacity = qBound(0.0, element.attribute(QStringLiteral("Alpha"), QStringLiteral("255")).toDouble() / 255, 1.0);
    Style style;
    if (!styled(element, inherited, style)) {
        return false;
    }
    object.fill = enabled(element, QStringLiteral("Fill"), name(element) == QLatin1String("TextObject")) ? color(style.fill) : QColor();
    if (enabled(element, QStringLiteral("Stroke"), name(element) == QLatin1String("PathObject")) && !stroke(style, object.pen)) {
        return false;
    }
    if (element.hasAttribute(QStringLiteral("HScale")) || element.hasAttribute(QStringLiteral("ReadDirection"))
        || element.hasAttribute(QStringLiteral("CharDirection")) || !child(element, QStringLiteral("Actions")).isNull()) {
        doc.m_unsupported = true;
    }
    if (name(element) == QLatin1String("ImageObject")) {
        const QString id = element.attribute(QStringLiteral("ResourceID"));
        if (!res.images.contains(id)) {
            return fail(QStringLiteral("Missing OFD image resource %1").arg(id));
        }
        object.image = res.images.value(id);
        if (!element.hasAttribute(QStringLiteral("CTM"))) {
            object.transform.scale(object.boundary.width(), object.boundary.height());
        }
        for (auto entry = element.firstChildElement(); !entry.isNull(); entry = entry.nextSiblingElement()) {
            if (name(entry) != QLatin1String("Clips")) {
                doc.m_unsupported = true;
            }
        }
        if (element.hasAttribute(QStringLiteral("Mask")) || element.hasAttribute(QStringLiteral("Substitution"))) {
            doc.m_unsupported = true;
        }
    } else if (name(element) == QLatin1String("PathObject")) {
        if (!path(child(element, QStringLiteral("AbbreviatedData")).text(), object.path)) {
            return false;
        }
        object.path.setFillRule(element.attribute(QStringLiteral("Rule")) == QLatin1String("Even-Odd") ? Qt::OddEvenFill : Qt::WindingFill);
    } else if (!text(element, object.path)) {
        return false;
    }
    if (!clips(child(element, QStringLiteral("Clips")), object, style) || !doc.m_error.isEmpty()) {
        return false;
    }
    page.objects.append(object);
    return true;
}

bool Document::Parser::content(const QDomElement &parent, Page &page, const Style &inherited, int depth)
{
    if (depth > 64) {
        return fail(QStringLiteral("OFD page nesting exceeds the supported limit"));
    }
    for (auto element = parent.firstChildElement(); !element.isNull(); element = element.nextSiblingElement()) {
        const QString tag = name(element);
        if (tag == QLatin1String("Layer") || tag == QLatin1String("PageBlock")) {
            if (element.hasAttribute(QStringLiteral("CTM")) || element.hasAttribute(QStringLiteral("Boundary"))) {
                doc.m_unsupported = true;
            }
            Style style;
            if (!styled(element, inherited, style) || !content(element, page, style, depth + 1)) {
                return false;
            }
        } else if (tag == QLatin1String("ImageObject") || tag == QLatin1String("PathObject") || tag == QLatin1String("TextObject")) {
            if (!object(element, page, inherited)) {
                return false;
            }
        } else {
            doc.m_unsupported = true;
        }
    }
    return true;
}

bool Document::Parser::parse()
{
    if (!archive.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("Cannot open OFD archive"));
    }
    const auto ofd = xml(QStringLiteral("OFD.xml"), QStringLiteral("OFD"));
    const auto body = child(ofd, QStringLiteral("DocBody"));
    const QString documentPath = location(QString(), child(body, QStringLiteral("DocRoot")).text().trimmed());
    if (ofd.isNull() || documentPath.isEmpty()) {
        return fail(QStringLiteral("Missing OFD document root"));
    }
    if (!body.nextSiblingElement().isNull() || !child(body, QStringLiteral("Signatures")).isNull()) {
        doc.m_unsupported = true;
    }
    const auto document = xml(documentPath, QStringLiteral("Document"));
    if (document.isNull()) {
        return false;
    }
    const auto common = child(document, QStringLiteral("CommonData"));
    QRectF defaultBox;
    const QString physicalBox = child(child(common, QStringLiteral("PageArea")), QStringLiteral("PhysicalBox")).text();
    if (!physicalBox.isEmpty() && !box(physicalBox, defaultBox)) {
        return fail(QStringLiteral("Invalid OFD default page size"));
    }
    QHash<QString, QDomElement> templates;
    for (auto element = common.firstChildElement(); !element.isNull(); element = element.nextSiblingElement()) {
        const QString tag = name(element);
        if (tag == QLatin1String("PublicRes") || tag == QLatin1String("DocumentRes")) {
            if (!resources(location(documentPath, element.text().trimmed()))) {
                return false;
            }
        } else if (tag == QLatin1String("TemplatePage")) {
            const QString id = element.attribute(QStringLiteral("ID"));
            if (id.isEmpty() || templates.contains(id)) {
                return fail(QStringLiteral("Invalid OFD template ID"));
            }
            templates.insert(id, element);
        }
    }
    if (!child(document, QStringLiteral("Annotations")).isNull() || !child(document, QStringLiteral("Attachments")).isNull()) {
        doc.m_unsupported = true;
    }
    res.defaultColorSpace = child(common, QStringLiteral("DefaultCS")).text().trimmed();
    if (!res.defaultColorSpace.isEmpty() && !res.colors.contains(res.defaultColorSpace)) {
        return fail(QStringLiteral("Missing OFD default color space"));
    }
    const Resources documentResources = res;
    const auto pages = child(document, QStringLiteral("Pages"));
    for (auto entry = pages.firstChildElement(); !entry.isNull(); entry = entry.nextSiblingElement()) {
        if (name(entry) != QLatin1String("Page") || doc.m_pages.size() >= 10000) {
            return fail(QStringLiteral("Invalid OFD page list"));
        }
        const QString pagePath = location(documentPath, entry.attribute(QStringLiteral("BaseLoc")));
        const auto root = xml(pagePath, QStringLiteral("Page"));
        if (root.isNull()) {
            return false;
        }
        Page page;
        page.box = defaultBox;
        const auto pageBox = child(child(root, QStringLiteral("Area")), QStringLiteral("PhysicalBox"));
        if ((!pageBox.isNull() && !box(pageBox.text(), page.box)) || !page.box.isValid()) {
            return fail(QStringLiteral("Invalid OFD page size"));
        }
        // Template scopes must not leak into the ordinary page or later pages.
        struct Part {
            int order;
            QList<Object> objects;
        };
        QList<Part> parts;
        for (auto reference = root.firstChildElement(); !reference.isNull(); reference = reference.nextSiblingElement()) {
            if (name(reference) != QLatin1String("Template")) {
                continue;
            }
            const auto definition = templates.value(reference.attribute(QStringLiteral("TemplateID")));
            if (definition.isNull()) {
                return fail(QStringLiteral("Missing OFD template"));
            }
            const QString templatePath = location(documentPath, definition.attribute(QStringLiteral("BaseLoc")));
            const auto templateRoot = xml(templatePath, QStringLiteral("Page"));
            res = documentResources;
            Page templatePage;
            if (templateRoot.isNull() || !pageResources(templateRoot, templatePath) || !pageContent(templateRoot, templatePage)) {
                return false;
            }
            const int rank = order(reference.attribute(QStringLiteral("ZOrder"), definition.attribute(QStringLiteral("ZOrder"), QStringLiteral("Background"))));
            if (rank < 0) {
                return false;
            }
            parts.append({rank - 1, templatePage.objects});
        }
        res = documentResources;
        if (!pageResources(root, pagePath)) {
            return false;
        }
        const auto contents = child(root, QStringLiteral("Content"));
        for (auto layer = contents.firstChildElement(); !layer.isNull(); layer = layer.nextSiblingElement()) {
            Page layerPage;
            if (name(layer) != QLatin1String("Layer")) {
                doc.m_unsupported = true;
                continue;
            }
            const int rank = order(layer.attribute(QStringLiteral("Type"), QStringLiteral("Body")));
            Style style;
            if (rank < 0 || !styled(layer, Style(), style) || !content(layer, layerPage, style)) {
                return false;
            }
            if (layer.hasAttribute(QStringLiteral("CTM")) || layer.hasAttribute(QStringLiteral("Boundary"))) {
                doc.m_unsupported = true;
            }
            parts.append({rank, layerPage.objects});
        }
        std::stable_sort(parts.begin(), parts.end(), [](const Part &a, const Part &b) {
            return a.order < b.order;
        });
        for (const auto &part : parts) {
            page.objects.append(part.objects);
        }
        doc.m_pages.append(page);
    }
    return !doc.m_pages.isEmpty() || fail(QStringLiteral("OFD document has no pages"));
}

bool Document::open(const QString &fileName)
{
    close();
    Parser parser(*this, fileName);
    if (!parser.parse()) {
        m_pages.clear();
        return false;
    }
    return true;
}

void Document::close()
{
    m_pages.clear();
    m_error.clear();
    m_unsupported = false;
}

int Document::pageCount() const
{
    return m_pages.size();
}

QSizeF Document::pageSize(int page) const
{
    return page >= 0 && page < m_pages.size() ? m_pages[page].box.size() : QSizeF();
}

QString Document::errorString() const
{
    return m_error;
}

bool Document::hasUnsupportedFeatures() const
{
    return m_unsupported;
}

QImage Document::render(int pageNumber, const QSize &size) const
{
    if (pageNumber < 0 || pageNumber >= m_pages.size() || !size.isValid() || qint64(size.width()) * size.height() > maxImagePixels) {
        return {};
    }
    QImage image(size, QImage::Format_RGB32);
    if (image.isNull()) {
        return image;
    }
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    const auto &page = m_pages[pageNumber];
    painter.scale(size.width() / page.box.width(), size.height() / page.box.height());
    painter.translate(-page.box.topLeft());
    for (const auto &object : page.objects) {
        painter.save();
        painter.setClipRect(object.boundary, Qt::IntersectClip);
        painter.translate(object.boundary.topLeft());
        if (!object.transformClips) {
            for (const auto &clip : object.clips) {
                painter.setClipPath(clip, Qt::IntersectClip);
            }
        }
        painter.setWorldTransform(object.transform, true);
        if (object.transformClips) {
            for (const auto &clip : object.clips) {
                painter.setClipPath(clip, Qt::IntersectClip);
            }
        }
        painter.setOpacity(object.opacity);
        if (!object.image.isNull()) {
            painter.drawImage(QRectF(0, 0, 1, 1), object.image);
        } else {
            painter.setBrush(object.fill.isValid() ? QBrush(object.fill) : QBrush(Qt::NoBrush));
            painter.setPen(object.pen);
            painter.drawPath(object.path);
        }
        painter.restore();
    }
    painter.end();
    return image;
}
}
