// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QtGlobal>

// QCheckBox::checkStateChanged(Qt::CheckState) arrived in Qt 6.7.0 (it is in 6.7.0 and not in
// 6.6.3); stateChanged(int) was only *deprecated* in 6.9, and 6.7 and 6.8 already carry the
// QT_DEPRECATED_VERSION_X_6_9 marker on it. Keying this on 6.9 therefore picked the deprecated
// signal on exactly the versions that have the replacement, and warned for it — CI builds 6.8.3.
#if QT_VERSION < QT_VERSION_CHECK(6, 7, 0)
#define STATE_CHANGED stateChanged
#define CHECKSTATE_TYPE int
#else
#define STATE_CHANGED checkStateChanged
#define CHECKSTATE_TYPE Qt::CheckState
#endif
