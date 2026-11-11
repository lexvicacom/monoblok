#ifndef PB_CONFIG_H
#define PB_CONFIG_H

#include "pb_program.h"

// Where config loaders copy env values and how they prefix error lines.
// Used by program load ("patchbay") and --validate ("validate"); borrowed for one call.
typedef struct pb_config_ctx {
    pb_arena *arena;
    const char *prefix;
} pb_config_ctx;

// True for top-level heads handled here: lvc, export, bridge, import.
bool pb_config_head_is(pb_slice head);

// Loads one top-level lvc/export/bridge/import form into the three config targets.
// A repeated export/bridge or import form is an error. Prints a prefixed message on failure.
bool pb_config_load_form(const pb_config_ctx *ctx, pb_values items,
                         pb_lvc_config *lvc, pb_bridge_config *bridge, pb_imports_config *imports);

void pb_remote_config_free(pb_remote_config *remote);
// Frees heap arrays of all three config targets and zeroes them.
void pb_config_free(pb_lvc_config *lvc, pb_bridge_config *bridge, pb_imports_config *imports);

#endif
