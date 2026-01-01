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

#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QMap>
#include <QQueue>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTimer>
#include <memory>

/* Geolocation Data Structure **************************************************/
struct GeoLocationData
{
    QString   ip;
    double    latitude{0.0};
    double    longitude{0.0};
    bool      is_valid{false};
    QDateTime cached_at;

    bool isExpired(int cache_ttl_seconds) const
    {
        if (!is_valid)
            return true;
        if (!cached_at.isValid())
            return true;
        return cached_at.msecsTo(QDateTime::currentDateTime()) > (cache_ttl_seconds * 1000);
    }
};

/* Rate Limiting Data Structure *************************************************/
struct RateLimitInfo
{
    int       remaining_requests{15};
    int       reset_time_seconds{60};
    QDateTime last_request_time;
    QDateTime reset_timestamp;
};

/* CGeoLocation Class **********************************************************/
class CGeoLocation : public QObject
{
    Q_OBJECT

public:
    explicit CGeoLocation(QObject* parent = nullptr);
    ~CGeoLocation();

    // Configuration
    void setRefreshIntervalMinutes(int minutes);
    void setMaxBatchSize(int size);
    void setRequestTimeoutMs(int timeout);
    void setEnableGeoLocation(bool enabled);

    // Query geolocation for a single IP (cached if available)
    GeoLocationData getGeoLocation(const QString& ip_address);

    // Query geolocation for multiple IPs (uses batching)
    QList<GeoLocationData> getGeoLocations(const QStringList& ip_addresses);

    // Clear the cache
    void clearCache();

    // Get current rate limit info
    RateLimitInfo getRateLimitInfo() const;

    // Check if we can make a request without hitting rate limits
    bool canMakeRequest();

    // Get seconds until rate limit resets
    int secondsUntilRateLimitReset() const;

signals:
    void geoLocationReady(const QString& ip, const GeoLocationData& geo_data);
    void geoLocationsReady(const QList<GeoLocationData>& geo_data);
    void rateLimitExceeded(int seconds_until_reset);
    void errorOccurred(const QString& error_message);

private slots:
    void onNetworkReply();

private:
    struct CacheEntry
    {
        GeoLocationData data;
        QDateTime       cached_at;
    };

    // Network request processing
    void processGeoLocationBatch(const QStringList& ip_addresses);
    void processPendingRequests();  // Send queued batches
    void parseJsonResponse(const QByteArray& response, QList<GeoLocationData>& results);
    void updateRateLimitInfo(const QNetworkReply* reply);
    QByteArray createBatchRequest(const QStringList& ip_addresses) const;

    // Rate limiting and batching
    bool isRateLimited() const;
    void scheduleNextBatchRequest();

    // Configuration
    int  m_cache_ttl_seconds{300};           // 5 minutes default
    int  m_max_batch_size{20};               // Max 20 IPs per batch (small and fast)
    int  m_request_timeout_ms{5000};         // 5 second timeout
    bool m_enable_geolocation{true};         // Enable/disable feature
    int  m_target_requests_per_minute{12};   // Stay well below the 15/min limit

    // Cache management
    QMap<QString, CacheEntry>   m_cache;
    mutable QMutex              m_cache_mutex;

    // Rate limiting
    RateLimitInfo               m_rate_limit;
    mutable QMutex              m_rate_limit_mutex;

    // Network
    std::unique_ptr<QNetworkAccessManager> m_network_manager;
    QMap<QNetworkReply*, QStringList>      m_pending_requests;  // Track which IPs belong to which request
    mutable QMutex                         m_network_mutex;
    QQueue<QStringList>                    m_request_queue;     // Queue pending batches
    int                                    m_active_requests{0}; // Track active requests
    QTimer                                 m_batch_timer;      // Timer between batches

    // Constants
    static constexpr const char* IP_API_BATCH_URL = "http://ip-api.com/batch";
    static constexpr int         RATE_LIMIT_WINDOW_MS = 60000;  // 1 minute
    static constexpr int         MIN_INTERVAL_BETWEEN_REQUESTS_MS = 1000;  // Min 1 second between batch requests
    static constexpr int         MAX_CONCURRENT_REQUESTS = 1;    // Max 1 concurrent request (free tier limit)
};
