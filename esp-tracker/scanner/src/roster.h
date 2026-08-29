#pragma once
#include <stdint.h>
#include <stddef.h>

// Cached roster of valid card UIDs, so the gate can answer immediately.
//
// The problem: the UID→child mapping lives ONLY on the server, deliberately —
// a stolen scanner must not reveal who attends the school. But that means the
// device cannot tell a registered card from a stranger's, and a child whose
// card was never enrolled would get a cheerful beep and find out weeks later.
//
// The compromise: the server sends HASHES of valid UIDs, never names. The
// scanner can say "yes, that card is enrolled" without holding a roster.
//
// Be honest about the limit: card UIDs are short and low-entropy, so these
// hashes are brute-forceable by anyone who steals the device. What they do NOT
// carry is identity — no names, no classes, no parents. That is the whole
// claim, and it is worth having.

namespace roster {
    bool begin();
    bool known(const char* uid);       // false also when the cache is empty
    bool stale();                      // older than ROSTER_TTL_S
    bool refresh();                    // GET /functions/v1/roster
    size_t size();
    uint32_t fetchedAt();
}
