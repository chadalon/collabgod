// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace collabgod
{
    /** The app's bridge contract tests: every message's example, through the generated codec
        and dispatch and the real BridgeHost. No window. Prints what failed. Returns the
        process exit code: 0 when every check passed. */
    int runBridgeTests();
}
