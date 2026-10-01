#pragma once

// The portable v0.10.0 generator places attributes before extern "C".
// GCC ignores that alias, leaving guest calls undefined and hooks ineffective.
// The Switch SDK's newer template puts attributes on the declaration instead.
// Force-include the same definition without modifying disposable guest output.
#if defined(__SWITCH__) && defined(__GNUC__) && !defined(__clang__)
#ifndef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) \
  extern "C" REX_FUNC(name) __attribute__((alias("__imp__" #name), weak, noinline)); \
  REX_EXTERN(__imp__##name)
#endif
#endif
