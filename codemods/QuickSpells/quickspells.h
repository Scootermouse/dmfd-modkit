// Quick Spells: auto-finish and spell hotkeys for the Witch and Oracle (solo only).
#pragma once
#include "../common/modbase.h"

enum { QS_PAD = 0x10000, QS_LT = 0x20000, QS_RT = 0x20001, QS_ACT = 0x40000 };  // QS_ACT | game button id (any controller)
enum { QS_WITCH, QS_ORACLE, QS_CLASSES, QS_SPELLS_PER_CLASS = 12 };
struct QsSpell { int id; const char* slug; const char* name; };  // id = index in the game's spell table
struct _XINPUT_STATE;

extern const QsSpell QS_SPELLS[QS_CLASSES][QS_SPELLS_PER_CLASS];
extern const char* const QS_CLASS_NAMES[QS_CLASSES];
bool QuickspellReady();          // the hook sites hold the game's code
bool QuickspellInit();           // hooks
bool IsSolo();                   // the game's own player-count check; call on the game thread
int QsSpellSeq(int id, int* out);  // a spell's buttons; 0 before the game is patched
bool QsHotkeyDown(int key, const _XINPUT_STATE* xs);
void QsKeyName(int key, char* out, int n);
int QsParseKey(const char* s);
int QsCapture(int ignoreVk);     // a hotkey pressed right now, or 0
int QsParseSeq(const char* s, int* out);
void QsFormatSeq(const int* seq, int len, char* out, int n);
extern volatile LONG g_quickspell;
extern volatile LONG g_inputDelayMs;  // minimum time between auto-entered buttons
extern int g_binds[QS_CLASSES][QS_SPELLS_PER_CLASS];  // hotkey per spell, 0 = none
extern CRITICAL_SECTION g_bindLock;

// ---- settings (main.cpp, quickspells.ini in the mod's folder) ----
struct QsSettings {
    bool autoFinish;           // finish the spell once only one matches
    int inputDelayMs;          // time between auto-entered buttons
    int binds[QS_CLASSES][QS_SPELLS_PER_CLASS];  // spell hotkeys
};
QsSettings QsGet();
void QsSet(const QsSettings& s, bool save);
