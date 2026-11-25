/*
    SPDX-FileCopyrightText: 2026 OFD plugin contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "generator_ofd.h"

#include <KLocalizedString>
#include <KPluginFactory>
#include <okular/core/page.h>

K_PLUGIN_CLASS_WITH_JSON(OFDGenerator, "libokularGenerator_ofd.json")

OFDGenerator::OFDGenerator(QObject *parent, const QVariantList &args)
    : Generator(parent, args)
{
    setFeature(Threaded);
}

bool OFDGenerator::loadDocument(const QString &fileName, QList<Okular::Page *> &pages)
{
    if (!m_document.open(fileName)) {
        Q_EMIT error(i18n("Could not open the OFD document: %1", m_document.errorString()), -1);
        return false;
    }
    constexpr double pointsPerMillimetre = 72.0 / 25.4;
    for (int i = 0; i < m_document.pageCount(); ++i) {
        const QSizeF size = m_document.pageSize(i) * pointsPerMillimetre;
        pages.append(new Okular::Page(i, size.width(), size.height(), Okular::Rotation0));
    }
    if (m_document.hasUnsupportedFeatures()) {
        Q_EMIT warning(i18n("This OFD document contains features that are not supported yet. Some content may be missing or displayed incorrectly. Digital "
                            "signatures are not verified."),
                       -1);
    }
    return true;
}

bool OFDGenerator::doCloseDocument()
{
    m_document.close();
    return true;
}

QImage OFDGenerator::image(Okular::PixmapRequest *request)
{
    // Okular supplies unrotated dimensions and rotates the result itself.
    return m_document.render(request->pageNumber(), QSize(request->width(), request->height()));
}

#include "generator_ofd.moc"
#include "moc_generator_ofd.cpp"
