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
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QDebug>

namespace
{
    // Global file for logging
    QFile*              g_pLogFile = nullptr;
    QTextStream*        g_pLogStream = nullptr;
    QtMessageHandler    g_pPreviousMessageHandler = nullptr;
    QtMsgType           g_logLevelMask = QtDebugMsg;

    // Custom message handler for file logging
    void CustomMessageHandler ( QtMsgType type, const QMessageLogContext& context, const QString& msg )
    {
        Q_UNUSED ( context );  // Not used in this implementation

        QString logMessage;

        // Format the log message with timestamp and level
        switch ( type )
        {
            case QtDebugMsg:
                logMessage = QString ( "[%1] DEBUG: %2" )
                    .arg ( QDateTime::currentDateTime().toString ( "yyyy-MM-dd hh:mm:ss.zzz" ) )
                    .arg ( msg );
                break;
            case QtInfoMsg:
                logMessage = QString ( "[%1] INFO: %2" )
                    .arg ( QDateTime::currentDateTime().toString ( "yyyy-MM-dd hh:mm:ss.zzz" ) )
                    .arg ( msg );
                break;
            case QtWarningMsg:
                logMessage = QString ( "[%1] WARNING: %2" )
                    .arg ( QDateTime::currentDateTime().toString ( "yyyy-MM-dd hh:mm:ss.zzz" ) )
                    .arg ( msg );
                break;
            case QtCriticalMsg:
                logMessage = QString ( "[%1] CRITICAL: %2" )
                    .arg ( QDateTime::currentDateTime().toString ( "yyyy-MM-dd hh:mm:ss.zzz" ) )
                    .arg ( msg );
                break;
            case QtFatalMsg:
                logMessage = QString ( "[%1] FATAL: %2" )
                    .arg ( QDateTime::currentDateTime().toString ( "yyyy-MM-dd hh:mm:ss.zzz" ) )
                    .arg ( msg );
                break;
        }

        // Write to file if available
        if ( g_pLogStream )
        {
            *g_pLogStream << logMessage << "\n";
            g_pLogStream->flush();
        }
    }
}  // namespace

// ============================================================================
// Logging Setup Functions
// ============================================================================

void setupLogging ( const QString& strLoggingFileName, QtMsgType logLevelMask )
{
    if ( strLoggingFileName.isEmpty() )
    {
        return;
    }

    g_logLevelMask = logLevelMask;

    // Open log file
    g_pLogFile = new QFile ( strLoggingFileName );
    if ( g_pLogFile->open ( QIODevice::Append | QIODevice::Text ) )
    {
        g_pLogStream = new QTextStream ( g_pLogFile );

        // Install our custom message handler (saves previous one)
        g_pPreviousMessageHandler = qInstallMessageHandler ( CustomMessageHandler );

        qInfo() << "ClientAggregator: Logging enabled to file:" << strLoggingFileName;
    }
    else
    {
        qWarning() << "ClientAggregator: Could not open log file:" << strLoggingFileName;
        delete g_pLogFile;
        g_pLogFile = nullptr;
    }
}

void shutdownLogging()
{
    // Cleanup logging
    if ( g_pLogStream )
    {
        *g_pLogStream << QString ( "[%1] ClientAggregator: Shutting down" )
            .arg ( QDateTime::currentDateTime().toString ( "yyyy-MM-dd hh:mm:ss.zzz" ) ) << "\n";
        g_pLogStream->flush();

        // Restore previous message handler
        qInstallMessageHandler ( g_pPreviousMessageHandler );
        g_pPreviousMessageHandler = nullptr;

        delete g_pLogStream;
        g_pLogStream = nullptr;
    }

    if ( g_pLogFile )
    {
        g_pLogFile->close();
        delete g_pLogFile;
        g_pLogFile = nullptr;
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

QString skillLevelToString ( ESkillLevel level )
{
    switch ( level )
    {
        case SL_BEGINNER:
            return QStringLiteral ( "Beginner" );
        case SL_INTERMEDIATE:
            return QStringLiteral ( "Intermediate" );
        case SL_PROFESSIONAL:
            return QStringLiteral ( "Professional" );
        case SL_NOT_SET:
        default:
            return QStringLiteral ( "Not Set" );
    }
}

QString countryToCode ( QLocale::Country country )
{
    if ( country == QLocale::AnyCountry )
    {
        return QString();
    }
    const QList<QLocale> locales = QLocale::matchingLocales ( QLocale::AnyLanguage, QLocale::AnyScript, country );
    if ( !locales.isEmpty() )
    {
        const QStringList parts = locales.first().name().split ( '_' );
        if ( parts.size() > 1 )
        {
            return parts.last();
        }
    }
    return QString();
}
