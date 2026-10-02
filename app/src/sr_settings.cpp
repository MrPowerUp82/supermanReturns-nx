#include "sr_settings.h"

REXCVAR_DEFINE_BOOL(sr_skip_intro, false, "Superman Returns",
                    "Skip publisher videos (experimental on Switch)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(sr_renderer, "xenos", "Superman Returns",
                      "Graphics backend: xenos or native")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
