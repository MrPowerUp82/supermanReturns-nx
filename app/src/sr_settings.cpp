#include "sr_settings.h"

REXCVAR_DEFINE_BOOL(sr_skip_intro, false, "Superman Returns",
                    "Skip publisher videos (experimental on Switch)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
