/******************************************************************************\
 * Copyright (c) 2004-2025
 *
 * Author(s):
 *  Volker Fischer
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

#include "clientaggregator.h"
#include <QJsonObject>
#include <QJsonArray>

// Forward declarations for utility functions (defined in clientaggregator_util.cpp)
extern QString skillLevelToString ( ESkillLevel level );
extern QString countryToCode ( QLocale::Country country );

QJsonObject Client::toJson ( bool detailed ) const
{
    QJsonObject obj;
    obj["chanid"]       = chanid;
    obj["country"]      = country;
    if ( detailed )
    {
        obj["countryid"]    = countryid;
        obj["skillid"]      = skillid;
        obj["skill"]        = skill;
    }
    obj["instrumentid"] = instrumentid;
    obj["country_code"] = country_code;
    obj["instrument"]   = instrument;
    obj["name"]         = name;
    obj["city"]         = city;
    return obj;
}

QJsonObject Server::toJson ( bool detailed ) const
{
    QJsonObject obj;
    obj["port"]        = static_cast<qint64> ( port );
    obj["maxclients"]  = maxclients;
    obj["name"]        = name;
    obj["country_code"] = country_code;
    obj["country"]     = country;
    obj["city"]        = city;
    obj["ip"]          = ip;
    obj["nclients"]    = nclients;

    // Add geolocation if available
    if ( geo_valid )
    {
        QJsonArray geoArray;
        geoArray.append ( geo_longitude );
        geoArray.append ( geo_latitude );
        obj["geo"] = geoArray;
    }

    // Add IPv4/IPv6 stack type information (available in non-detailed path)
    {
        QJsonObject stackObj;
        stackObj["type"] = static_cast<int>(ipStackType);
        stackObj["ipv4"] = reachableViaIPv4;
        stackObj["ipv6"] = reachableViaIPv6;
        if (!ipv6Address.isEmpty())
        {
            stackObj["ipv6_address"] = ipv6Address;
        }
        obj["ip_stack"] = stackObj;
    }

    if ( detailed )
    {
        obj["numip"]       = static_cast<qint64> ( numip );
        obj["countryid"]   = countryid;
        obj["perm"]        = perm;
        obj["ipaddrs"]     = ipaddrs;
        obj["ping"]        = ping;
        obj["os"]          = os;
        obj["version"]     = version;
        obj["versionsort"] = versionsort;
    }

    QJsonArray clientsArray;
    for ( const auto& client : clients )
    {
        clientsArray.append ( client.toJson ( detailed ) );
    }
    if ( !clientsArray.isEmpty() )
    {
        obj["clients"] = clientsArray;
    }

    return obj;
}
