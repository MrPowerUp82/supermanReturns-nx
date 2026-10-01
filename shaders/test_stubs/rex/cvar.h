#pragma once
// Host test stub: cvars are plain functions returning their storage.
#define REXCVAR_DECLARE(type, name) type& FLAGS_##name##_storage_()
#define REXCVAR_GET(name) (FLAGS_##name##_storage_())
