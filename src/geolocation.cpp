/******************************************************************************\
 * Copyright (c) 2025
 *
 * Author(s):
 *
 ******************************************************************************
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
\******************************************************************************/

#include "geolocation.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>
#include <QTimer>
#include <QMutexLocker>
#include <QUrl>
#include <QUrlQuery>

CGeoLocation::CGeoLocation(QObject* parent)
    : QObject(parent),
      m_network_manager(std::make_unique<QNetworkAccessManager>(this))
{
    // Initialize rate limit info
    m_rate_limit.remaining_requests = 15;
    m_rate_limit.reset_time_seconds = 60;
    m_rate_limit.last_request_time = QDateTime::currentDateTime();
    m_rate_limit.reset_timestamp = m_rate_limit.last_request_time.addSecs(60);

    // Setup batch timer for rate limiting
    m_batch_timer.setSingleShot(true);
    connect(&m_batch_timer, &QTimer::timeout, this, &CGeoLocation::processPendingRequests);

    qInfo() << "CGeoLocation: Initialized with default settings";
}

CGeoLocation::~CGeoLocation()
{
    clearCache();
    qInfo() << "CGeoLocation: Destroyed";
}

void CGeoLocation::setRefreshIntervalMinutes(int minutes)
{
    QMutexLocker locker(&m_cache_mutex);
    m_cache_ttl_seconds = minutes * 60;
    qInfo() << "CGeoLocation: Refresh interval set to" << minutes << "minutes";
}

void CGeoLocation::setMaxBatchSize(int size)
{
    if (size > 0 && size <= 100)
    {
        m_max_batch_size = size;
        qInfo() << "CGeoLocation: Max batch size set to" << size;
    }
    else
    {
        qWarning() << "CGeoLocation: Invalid batch size" << size << "(must be 1-100)";
    }
}

void CGeoLocation::setRequestTimeoutMs(int timeout)
{
    if (timeout > 0)
    {
        m_request_timeout_ms = timeout;
        qInfo() << "CGeoLocation: Request timeout set to" << timeout << "ms";
    }
}

void CGeoLocation::setEnableGeoLocation(bool enabled)
{
    m_enable_geolocation = enabled;
    qInfo() << "CGeoLocation:" << (enabled ? "Enabled" : "Disabled");
}

GeoLocationData CGeoLocation::getGeoLocation(const QString& ip_address)
{
    if (!m_enable_geolocation)
    {
        GeoLocationData invalid;
        invalid.ip = ip_address;
        invalid.is_valid = false;
        return invalid;
    }

    // Check cache first
    {
        QMutexLocker locker(&m_cache_mutex);
        auto it = m_cache.find(ip_address);
        if (it != m_cache.end() && !it.value().data.isExpired(m_cache_ttl_seconds))
        {
            qDebug() << "CGeoLocation: Cache hit for" << ip_address;
            return it.value().data;
        }
    }

    // Not in cache or expired, need to query
    QStringList ips;
    ips << ip_address;
    processGeoLocationBatch(ips);

    // Return empty result for now (will be signaled when ready)
    GeoLocationData result;
    result.ip = ip_address;
    result.is_valid = false;
    return result;
}

QList<GeoLocationData> CGeoLocation::getGeoLocations(const QStringList& ip_addresses)
{
    QList<GeoLocationData> results;

    if (!m_enable_geolocation)
    {
        for (const auto& ip : ip_addresses)
        {
            GeoLocationData invalid;
            invalid.ip = ip;
            invalid.is_valid = false;
            results.append(invalid);
        }
        return results;
    }

    QStringList ips_to_query;

    // Check cache for all IPs
    {
        QMutexLocker locker(&m_cache_mutex);
        for (const auto& ip : ip_addresses)
        {
            auto it = m_cache.find(ip);
            if (it != m_cache.end() && !it.value().data.isExpired(m_cache_ttl_seconds))
            {
                results.append(it.value().data);
                qDebug() << "CGeoLocation: Cache hit for" << ip;
            }
            else
            {
                ips_to_query.append(ip);
            }
        }
    }

    // Query missing IPs
    if (!ips_to_query.isEmpty())
    {
        processGeoLocationBatch(ips_to_query);
    }

    return results;
}

void CGeoLocation::clearCache()
{
    QMutexLocker locker(&m_cache_mutex);
    m_cache.clear();
    qInfo() << "CGeoLocation: Cache cleared";
}

RateLimitInfo CGeoLocation::getRateLimitInfo() const
{
    QMutexLocker locker(&m_rate_limit_mutex);
    return m_rate_limit;
}

bool CGeoLocation::isRateLimited() const
{
    QMutexLocker locker(&m_rate_limit_mutex);

    // Check if we've exceeded the limit
    if (m_rate_limit.remaining_requests <= 0)
    {
        // Check if reset time has passed
        if (QDateTime::currentDateTime() < m_rate_limit.reset_timestamp)
        {
            return true;
        }
        else
        {
            // Reset window has passed - this will be handled in non-const version
            // when rate limit is actually checked for making a request
            return false;
        }
    }

    return false;
}

// Non-const version for actually making requests
bool CGeoLocation::canMakeRequest()
{
    QMutexLocker locker(&m_rate_limit_mutex);

    // Check if we've exceeded the limit
    if (m_rate_limit.remaining_requests <= 0)
    {
        // Check if reset time has passed
        if (QDateTime::currentDateTime() < m_rate_limit.reset_timestamp)
        {
            return false;  // Still limited
        }
        else
        {
            // Reset the counter
            m_rate_limit.remaining_requests = 15;
            m_rate_limit.reset_timestamp = QDateTime::currentDateTime().addSecs(60);
            return true;
        }
    }

    return true;
}

int CGeoLocation::secondsUntilRateLimitReset() const
{
    QMutexLocker locker(&m_rate_limit_mutex);
    if (!m_rate_limit.reset_timestamp.isValid())
        return 0;

    int seconds = QDateTime::currentDateTime().secsTo(m_rate_limit.reset_timestamp);
    return qMax(0, seconds);
}

void CGeoLocation::processGeoLocationBatch(const QStringList& ip_addresses)
{
    if (ip_addresses.isEmpty())
        return;

    if (!canMakeRequest())
    {
        int wait_seconds = secondsUntilRateLimitReset();
        qWarning() << "CGeoLocation: Rate limited. Wait" << wait_seconds << "seconds";
        emit rateLimitExceeded(wait_seconds);
        return;
    }

    // Queue batches instead of sending all at once
    int batch_count = (ip_addresses.size() + m_max_batch_size - 1) / m_max_batch_size;
    
    {
        QMutexLocker locker(&m_network_mutex);
        for (int batch_idx = 0; batch_idx < batch_count; ++batch_idx)
        {
            int start = batch_idx * m_max_batch_size;
            int end = qMin(start + m_max_batch_size, ip_addresses.size());
            QStringList batch = ip_addresses.mid(start, end - start);
            m_request_queue.enqueue(batch);
        }
    }

    // Send first batch(es) if we have capacity
    processPendingRequests();

    // Update rate limit - decrement once for the whole batch set
    {
        QMutexLocker locker(&m_rate_limit_mutex);
        m_rate_limit.last_request_time = QDateTime::currentDateTime();
        m_rate_limit.reset_timestamp = m_rate_limit.last_request_time.addSecs(60);
        if (m_rate_limit.remaining_requests > 0)
            m_rate_limit.remaining_requests--;
    }
}

void CGeoLocation::processPendingRequests()
{
    QMutexLocker locker(&m_network_mutex);

    // If we have active requests or timer is running, wait
    if (m_active_requests > 0 || m_batch_timer.isActive())
    {
        return;
    }

    // Send one request if queue has items
    if (!m_request_queue.isEmpty())
    {
        QStringList batch = m_request_queue.dequeue();
        
        QByteArray body = createBatchRequest(batch);
        
        // Create URL with fields parameter
        QUrl url(IP_API_BATCH_URL);
        QUrlQuery query;
        query.addQueryItem("fields", "query,status,lat,lon");
        url.setQuery(query);
        
        QNetworkRequest req(url);
        
        req.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/json"));
        req.setHeader(QNetworkRequest::UserAgentHeader, QVariant("Jamulus/1.0"));

        qDebug() << "CGeoLocation: Sending batch request for" << batch.size() << "IPs" 
                 << "(queued:" << m_request_queue.size() << ")";

        QNetworkReply* pReply = m_network_manager->post(req, body);

        if (pReply)
        {
            m_pending_requests[pReply] = batch;
            m_active_requests++;

            // Connect the reply's finished signal to our handler
            connect(pReply, &QNetworkReply::finished, this, &CGeoLocation::onNetworkReply);

            // Set timeout
            QTimer::singleShot(m_request_timeout_ms, pReply, [pReply, this]() {
                if (pReply && pReply->isRunning())
                {
                    qWarning() << "CGeoLocation: Request timeout for batch";
                    pReply->abort();
                }
            });

            // Schedule next batch after minimum interval
            m_batch_timer.start(MIN_INTERVAL_BETWEEN_REQUESTS_MS);
        }
    }
}

QByteArray CGeoLocation::createBatchRequest(const QStringList& ip_addresses) const
{
    QJsonArray array;
    for (const auto& ip : ip_addresses)
    {
        QJsonObject obj;
        obj["query"] = ip;
        array.append(obj);
    }

    QJsonDocument doc(array);
    return doc.toJson(QJsonDocument::Compact);
}

void CGeoLocation::onNetworkReply()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;

    // Disconnect this signal to prevent double-firing
    disconnect(reply, nullptr, this, nullptr);

    QStringList query_ips;
    {
        QMutexLocker locker(&m_network_mutex);
        auto it = m_pending_requests.find(reply);
        if (it != m_pending_requests.end())
        {
            query_ips = it.value();
            m_pending_requests.erase(it);
        }
        m_active_requests--;
    }

    // Update rate limit info from response headers
    updateRateLimitInfo(reply);

    if (reply->error() != QNetworkReply::NoError)
    {
        qWarning() << "CGeoLocation: Network error for" << query_ips.size() << "IPs:" << reply->errorString();
        emit errorOccurred(reply->errorString());
        reply->deleteLater();
        
        // Process next pending request
        processPendingRequests();
        return;
    }

    QByteArray response = reply->readAll();
    QList<GeoLocationData> results;

    parseJsonResponse(response, results);

    // Cache results and emit signals
    {
        QMutexLocker locker(&m_cache_mutex);
        QDateTime now = QDateTime::currentDateTime();
        for (const auto& geo_data : results)
        {
            if (geo_data.is_valid)
            {
                CacheEntry entry;
                entry.data = geo_data;
                entry.cached_at = now;
                m_cache[geo_data.ip] = entry;

                qDebug() << "CGeoLocation: Cached" << geo_data.ip
                        << "at" << geo_data.latitude << "," << geo_data.longitude;
            }
            
            // Emit signal for each result
            emit geoLocationReady(geo_data.ip, geo_data);
        }
    }

    // Emit batch signal if results available
    if (!results.isEmpty())
    {
        emit geoLocationsReady(results);
    }

    reply->deleteLater();

    // Process next pending request
    processPendingRequests();
}

void CGeoLocation::parseJsonResponse(const QByteArray& response, QList<GeoLocationData>& results)
{
    QJsonDocument doc = QJsonDocument::fromJson(response);
    if (!doc.isArray())
    {
        qWarning() << "CGeoLocation: Invalid JSON response (not an array)";
        return;
    }

    QJsonArray array = doc.array();
    for (const auto& value : array)
    {
        if (!value.isObject())
            continue;

        QJsonObject obj = value.toObject();
        GeoLocationData geo_data;
        
        // Get IP from query field
        geo_data.ip = obj.value("query").toString();
        if (geo_data.ip.isEmpty())
        {
            qWarning() << "CGeoLocation: Empty IP in response";
            continue;
        }

        // Check status
        QString status = obj.value("status").toString();
        if (status != "success")
        {
            geo_data.is_valid = false;
            results.append(geo_data);
            qDebug() << "CGeoLocation: Failed to get location for" << geo_data.ip << "status:" << status;
            continue;
        }

        // Parse geolocation data - only lat/lon
        geo_data.latitude = obj.value("lat").toDouble();
        geo_data.longitude = obj.value("lon").toDouble();
        geo_data.is_valid = true;
        geo_data.cached_at = QDateTime::currentDateTime();

        results.append(geo_data);

        qDebug() << "CGeoLocation: Parsed" << geo_data.ip
                << "at" << geo_data.latitude << "," << geo_data.longitude;
    }
}

void CGeoLocation::updateRateLimitInfo(const QNetworkReply* reply)
{
    QMutexLocker locker(&m_rate_limit_mutex);

    // Check X-Rl header (remaining requests)
    QVariant x_rl = reply->header(QNetworkRequest::KnownHeaders(QNetworkRequest::UserAgentHeader)); // Fallback
    bool ok;
    int remaining = reply->rawHeader("X-Rl").toInt(&ok);
    if (ok && remaining >= 0)
    {
        m_rate_limit.remaining_requests = remaining;
        qDebug() << "CGeoLocation: Rate limit - remaining requests:" << remaining;
    }

    // Check X-Ttl header (seconds until reset)
    int ttl = reply->rawHeader("X-Ttl").toInt(&ok);
    if (ok && ttl > 0)
    {
        m_rate_limit.reset_time_seconds = ttl;
        m_rate_limit.reset_timestamp = QDateTime::currentDateTime().addSecs(ttl);
        qDebug() << "CGeoLocation: Rate limit - reset in" << ttl << "seconds";
    }
}
