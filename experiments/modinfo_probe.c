#include "hmr/runtime/hmr_runtime.h"

static const HmrRegexEntry kRegexes[] = {
    { "^INVITE", 0u },
    { "sip:1900[0-9]+@", 1u },
};

const HmrModuleInfo hmr_module_info = {
    HMR_ABI_VERSION,   /* abi_version */
    "NormalizePAI",    /* name */
    3u,                /* num_slots */
    2u,                /* num_regexes */
    kRegexes,          /* regexes */
};
