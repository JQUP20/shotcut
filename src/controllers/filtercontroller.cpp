/*
 * Copyright (c) 2014-2022 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "shotcut_mlt_properties.h"
#include "filtercontroller.h"
#include <QQmlEngine>
#include <QDir>
#include <Logger.h>
#include <QQmlComponent>
#include <QTimerEvent>
#include "mltcontroller.h"
#include "settings.h"
#include "qmltypes/qmlmetadata.h"
#include "qmltypes/qmlutilities.h"
#include "qmltypes/qmlfilter.h"

#include <MltLink.h>

FilterController::FilterController(QObject *parent) : QObject(parent),
    m_mltService(0),
    m_metadataModel(this),
    m_attachedModel(this),
    m_currentFilterIndex(QmlFilter::NoCurrentFilter)
{
    LOG_DEBUG() << "BEGIN";
    startTimer(0);
    connect(&m_attachedModel, SIGNAL(changed()), this, SLOT(handleAttachedModelChange()));
    connect(&m_attachedModel, SIGNAL(modelAboutToBeReset()), this,
            SLOT(handleAttachedModelAboutToReset()));
    connect(&m_attachedModel, SIGNAL(rowsRemoved(const QModelIndex &, int, int)), this,
            SLOT(handleAttachedRowsRemoved(const QModelIndex &, int, int)));
    connect(&m_attachedModel, SIGNAL(rowsInserted(const QModelIndex &, int, int)), this,
            SLOT(handleAttachedRowsInserted(const QModelIndex &, int, int)));
    connect(&m_attachedModel, SIGNAL(duplicateAddFailed(int)), this,
            SLOT(handleAttachDuplicateFailed(int)));
    LOG_DEBUG() << "END";
}

void FilterController::loadFilterMetadata()
{
    LOG_DEBUG() << "BEGIN";
    QScopedPointer<Mlt::Properties> mltFilters(MLT.repository()->filters());
    QScopedPointer<Mlt::Properties> mltLinks(MLT.repository()->links());
    QScopedPointer<Mlt::Properties> mltProducers(MLT.repository()->producers());
    QDir dir = QmlUtilities::qmlDir();
    dir.cd("filters");
    foreach (QString dirName, dir.entryList(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Executable)) {
        QDir subdir = dir;
        subdir.cd(dirName);
        subdir.setFilter(QDir::Files | QDir::NoDotAndDotDot | QDir::Readable);
        subdir.setNameFilters(QStringList("meta*.qml"));
        foreach (QString fileName, subdir.entryList()) {
            LOG_DEBUG() << "reading filter metadata" << dirName << fileName;
            QQmlComponent component(QmlUtilities::sharedEngine(), subdir.absoluteFilePath(fileName));
            QmlMetadata *meta = qobject_cast<QmlMetadata *>(component.create());
            if (meta) {
                QScopedPointer<Mlt::Properties> mltMetadata(MLT.repository()->metadata(mlt_service_filter_type,
                                                                                       meta->mlt_service().toLatin1().constData()));
                QString version;
                if (mltMetadata && mltMetadata->is_valid() && mltMetadata->get("version")) {
                    version = QString::fromLatin1(mltMetadata->get("version"));
                    if (version.startsWith("lavfi"))
                        version.remove(0, 5);
                }

                // Check if mlt_service is available.
                if (mltFilters->get_data(meta->mlt_service().toLatin1().constData()) &&
                        // Check if MLT glaxnimate producer is available if needed
                        ("maskGlaxnimate" != meta->objectName() || mltProducers->get_data("glaxnimate")) &&
                        (version.isEmpty() || meta->isMltVersion(version))) {
                    LOG_DEBUG() << "added filter" << meta->name();
                    meta->loadSettings();
                    meta->setPath(subdir);
                    meta->setParent(0);
                    addMetadata(meta);

                    // Check if a keyframes minimum version is required.
                    if (!version.isEmpty() && meta->keyframes()) {
                        meta->setProperty("version", version);
                        meta->keyframes()->checkVersion(version);
                    }
                } else if (meta->type() == QmlMetadata::Link
                           && mltLinks->get_data(meta->mlt_service().toLatin1().constData())) {
                    LOG_DEBUG() << "added link" << meta->name();
                    meta->loadSettings();
                    meta->setPath(subdir);
                    meta->setParent(0);
                    addMetadata(meta);
                }

                if (meta->isDeprecated())
                    meta->setName(meta->name() + " " + tr("(DEPRECATED)"));
            } else if (!meta) {
                LOG_WARNING() << component.errorString();
            }
        }
    };
    LOG_DEBUG() << "END";
}

QmlMetadata *FilterController::metadataForService(Mlt::Service *service)
{
    LOG_DEBUG() << "BEGIN";
    QmlMetadata *meta = 0;
    int rowCount = m_metadataModel.rowCount();
    QString uniqueId = service->get(kShotcutFilterProperty);

    // Fallback to mlt_service for legacy filters
    if (uniqueId.isEmpty()) {
        uniqueId = service->get("mlt_service");
    }

    for (int i = 0; i < rowCount; i++) {
        QmlMetadata *tmpMeta = m_metadataModel.get(i);
        if (tmpMeta->uniqueId() == uniqueId) {
            meta = tmpMeta;
            break;
        }
    }

    return meta;
    LOG_DEBUG() << "END";
}

void FilterController::timerEvent(QTimerEvent *event)
{
    LOG_DEBUG() << "BEGIN";

    loadFilterMetadata();
    killTimer(event->timerId());

    LOG_DEBUG() << "END";
}

MetadataModel *FilterController::metadataModel()
{
    LOG_DEBUG() << "BEGIN";
    return &m_metadataModel;
    LOG_DEBUG() << "END";
}

AttachedFiltersModel *FilterController::attachedModel()
{
    LOG_DEBUG() << "BEGIN";
    return &m_attachedModel;
    LOG_DEBUG() << "END";
}

void FilterController::setProducer(Mlt::Producer *producer)
{
    LOG_DEBUG() << "BEGIN";
    m_attachedModel.setProducer(producer);
    if (producer && producer->is_valid()) {
        m_metadataModel.setIsClipProducer(!MLT.isTrackProducer(*producer));
        m_metadataModel.setIsChainProducer(producer->type() == mlt_service_chain_type);
    }
    LOG_DEBUG() << "END";
}

void FilterController::setCurrentFilter(int attachedIndex, bool isNew)
{
    LOG_DEBUG() << "BEGIN";
    if (attachedIndex == m_currentFilterIndex) {
        return;
    }
    m_currentFilterIndex = attachedIndex;

    // VUIs may instruct MLT filters to not render if they are doing the rendering
    // theirself, for example, Text: Rich. Component.onDestruction is not firing.
    if (m_mltService) {
        if (m_mltService->get_int("_hide")) {
            m_mltService->clear("_hide");
            MLT.refreshConsumer();
        }
    }

    QmlMetadata *meta = m_attachedModel.getMetadata(m_currentFilterIndex);
    QmlFilter *filter = 0;
    if (meta) {
        emit currentFilterChanged(nullptr, nullptr, QmlFilter::NoCurrentFilter);
        m_mltService = m_attachedModel.getService(m_currentFilterIndex);
        if (!m_mltService) return;
        filter = new QmlFilter(*m_mltService, meta);
        filter->setIsNew(isNew);
        connect(filter, SIGNAL(changed()), SLOT(onQmlFilterChanged()));
        connect(filter, SIGNAL(changed(QString)), SLOT(onQmlFilterChanged(const QString &)));
    }

    emit currentFilterChanged(filter, meta, m_currentFilterIndex);
    m_currentFilter.reset(filter);
    LOG_DEBUG() << "END";
}

void FilterController::onFadeInChanged()
{
    LOG_DEBUG() << "BEGIN";
    if (m_currentFilter) {
        emit m_currentFilter->changed();
        emit m_currentFilter->animateInChanged();
    }
    LOG_DEBUG() << "END";
}

void FilterController::onFadeOutChanged()
{
    LOG_DEBUG() << "BEGIN";
    if (m_currentFilter) {
        emit m_currentFilter->changed();
        emit m_currentFilter->animateOutChanged();
    }
    LOG_DEBUG() << "END";
}

void FilterController::onServiceInChanged(int delta, Mlt::Service *service)
{
    LOG_DEBUG() << "BEGIN";
    if (delta && m_currentFilter && (!service
                                     || m_currentFilter->service().get_service() == service->get_service())) {
        emit m_currentFilter->inChanged(delta);
    }
    LOG_DEBUG() << "END";
}

void FilterController::onServiceOutChanged(int delta, Mlt::Service *service)
{
    LOG_DEBUG() << "BEGIN";
    if (delta && m_currentFilter && (!service
                                     || m_currentFilter->service().get_service() == service->get_service())) {
        emit m_currentFilter->outChanged(delta);
    }
    LOG_DEBUG() << "END";
}

void FilterController::handleAttachedModelChange()
{
    LOG_DEBUG() << "BEGIN";
    if (m_currentFilter) {
        emit m_currentFilter->changed("disable");
    }
    LOG_DEBUG() << "END";
}

void FilterController::handleAttachedModelAboutToReset()
{
    LOG_DEBUG() << "BEGIN";
    setCurrentFilter(QmlFilter::NoCurrentFilter);
    LOG_DEBUG() << "END";
}

void FilterController::handleAttachedRowsRemoved(const QModelIndex &, int first, int)
{
    LOG_DEBUG() << "BEGIN";
    m_currentFilterIndex = QmlFilter::DeselectCurrentFilter; // Force update
    setCurrentFilter(qBound(0, first, m_attachedModel.rowCount() - 1));
    LOG_DEBUG() << "END";
}

void FilterController::handleAttachedRowsInserted(const QModelIndex &, int first, int)
{
    LOG_DEBUG() << "BEGIN";
    m_currentFilterIndex = QmlFilter::DeselectCurrentFilter; // Force update
    setCurrentFilter(qBound(0, first, m_attachedModel.rowCount() - 1), true);
    LOG_DEBUG() << "END";
}

void FilterController::handleAttachDuplicateFailed(int index)
{
    LOG_DEBUG() << "BEGIN";
    const QmlMetadata *meta = m_attachedModel.getMetadata(index);
    emit statusChanged(tr("Only one %1 filter is allowed.").arg(meta->name()));
    setCurrentFilter(index);
    LOG_DEBUG() << "END";
}

void FilterController::onQmlFilterChanged()
{
    LOG_DEBUG() << "BEGIN";
    emit filterChanged(m_mltService);
    LOG_DEBUG() << "END";
}

void FilterController::onQmlFilterChanged(const QString &name)
{
    LOG_DEBUG() << "BEGIN";
    if (name == "disable") {
        QModelIndex index = m_attachedModel.index(m_currentFilterIndex);
        emit m_attachedModel.dataChanged(index, index, QVector<int>() << Qt::CheckStateRole);
    }
    LOG_DEBUG() << "END";
}

void FilterController::removeCurrent()
{
    LOG_DEBUG() << "BEGIN";
    if (m_currentFilterIndex > QmlFilter::NoCurrentFilter)
        m_attachedModel.remove(m_currentFilterIndex);
    LOG_DEBUG() << "END";
}

void FilterController::onProducerChanged()
{
    LOG_DEBUG() << "BEGIN";
    emit m_attachedModel.trackTitleChanged();
    LOG_DEBUG() << "END";
}

void FilterController::addMetadata(QmlMetadata *meta)
{
    LOG_DEBUG() << "BEGIN";
    m_metadataModel.add(meta);
    LOG_DEBUG() << "END";
}
