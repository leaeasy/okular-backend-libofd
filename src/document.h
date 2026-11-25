/*
    SPDX-FileCopyrightText: 2026 OFD plugin contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef OKULAR_OFD_DOCUMENT_H
#define OKULAR_OFD_DOCUMENT_H

#include <QImage>
#include <QPainterPath>
#include <QPen>
#include <QRectF>
#include <QString>
#include <QTransform>

namespace OFD
{
// Coordinates are kept in OFD millimetres until rendering.
class Document
{
public:
    bool open(const QString &fileName);
    void close();
    int pageCount() const;
    QSizeF pageSize(int page) const;
    QImage render(int page, const QSize &size) const;
    QString errorString() const;
    bool hasUnsupportedFeatures() const;

private:
    struct Object {
        QRectF boundary;
        QTransform transform;
        QPainterPath path;
        QImage image;
        QList<QPainterPath> clips;
        bool transformClips = false;
        QColor fill;
        QPen pen = Qt::NoPen;
        double opacity = 1;
    };
    struct Page {
        QRectF box;
        QList<Object> objects;
    };
    class Parser;
    QList<Page> m_pages;
    QString m_error;
    bool m_unsupported = false;
};
}
#endif
