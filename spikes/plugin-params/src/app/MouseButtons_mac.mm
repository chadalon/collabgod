// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #13. Throwaway code: product code never includes this.

#include "MouseButtons.h"

#import <AppKit/AppKit.h>

namespace cg
{
bool anyMouseButtonDown()
{
    // The buttons held now, whichever view or process takes the events.
    return [NSEvent pressedMouseButtons] != 0;
}
}
