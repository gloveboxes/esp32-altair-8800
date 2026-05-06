#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    /* Render the MITS-inspired Altair 8800 boot splash to the VT100 terminal.
     * Waveshare 3.5" AXS15231B colour display only. */
    void altair_splash_show(void);
#endif

#ifdef __cplusplus
}
#endif
