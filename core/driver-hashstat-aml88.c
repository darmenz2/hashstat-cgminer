/* SPDX-License-Identifier: GPL-3.0-only
 * HashStat S19-88 AML integration boundary, 2026.
 * HAL lifecycle is not implemented. Never claim a device or touch hardware.
 */
#include "config.h"
#include "miner.h"
#include "hashstat-core.h"

static void hashstat_aml88_detect(bool hotplug)
{
    if (!hotplug)
        applog(LOG_WARNING, "HashStat AML88: hardware lifecycle unavailable; "
                            "no device registered, no hardware access performed");

}

struct device_drv hashstat_aml88_drv = {
    .drv_id = DRIVER_hashstat_aml88,
    .dname = "hashstat-aml88-experimental",
    .name = "HS88",
    .drv_detect = hashstat_aml88_detect,
};
