#define DEBUG_DECLARE_ONLY
// Enables the genesys backend's built-in virtual device mode for a LiDE 50.
// Used only by LiDE50 Scan's --simulate switch (testing without hardware).
#include "../genesys/test_settings.h"
#include "../genesys/test_scanner_interface.h"

extern "C" void lide50_enable_simulation(void)
{
    genesys::enable_testing_mode(0x04a9, 0x2213, 0xffff,
        [](const genesys::Genesys_Device&, genesys::TestScannerInterface&, const std::string&) {});
}
