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

#endif // SELECTOR_TR_MENU_H
