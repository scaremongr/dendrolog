#ifndef LOGCOLORS_H
#define LOGCOLORS_H

#include <QColor>
#include "logentry.h"

namespace LogColors {
    // ------------------------------------------------------------------------
    // Global Color Constants
    // ------------------------------------------------------------------------
    inline QColor fatal() { return QColor(153, 0, 0); }      // Dark red (буро-красный)
    inline QColor error() { return QColor(255, 102, 102); }  // Light red
    inline QColor warn()  { return QColor(255, 204, 0); }    // Yellow
    
    inline QColor info()  { return QColor(0, 120, 215); }    // Blue
    inline QColor debug() { return QColor(128, 128, 128); }  // Gray
    inline QColor trace() { return QColor(169, 169, 169); }  // Light gray

    inline QColor separator() { return QColor(180, 180, 180); } // For W/E/F slashes

    // Retrieve color by level
    inline QColor forLevel(LogLevel level) {
        switch (level) {
            case LogLevel::Fatal: return fatal();
            case LogLevel::Error: return error();
            case LogLevel::Warn:  return warn();
            case LogLevel::Info:  return info();
            case LogLevel::Debug: return debug();
            case LogLevel::Trace: return trace();
            default:              return QColor(Qt::black);
        }
    }
}

#endif // LOGCOLORS_H
