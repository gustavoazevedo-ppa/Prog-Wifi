/*
 * Prog menu translator: the Prog's own pages on the selector while the central is silent
 * (project / version, then IP and MAC of each interface enabled in the firmware), and the
 * central's screens as soon as it talks. Navigation: SEL ("->", right arrow) next,
 * OK ("<-", left arrow) previous.
 * Design: docs/selector-bridge-design.md section 7b
 */
#ifndef SELECTOR_TR_MENU_H
#define SELECTOR_TR_MENU_H

#include "selector_bridge.h"

const selector_translator_t *selector_tr_menu(void);

// True while the menu is not showing (the central's screens and keys pass through, including
// while a macro is being recorded). Written by the bridge task only, safe to read from any task.
// POST /iniciar is refused while it is true.
bool selector_tr_menu_transparent(void);

// Asks the menu to replay the macro in the scratchpad for a running POST /iniciar run. Never
// blocks. False when the bridge has not started. The result comes back through
// preset_server_macro_run_finish().
bool selector_tr_menu_run_macro(void);

#endif // SELECTOR_TR_MENU_H
