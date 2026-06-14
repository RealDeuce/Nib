// license:BSD-3-Clause
// copyright-holders:Stephen Hurd
// Inspired by VirtualT (Copyright 2004 Stephen Hurd and Ken Pettit, BSD-2-Clause)
#ifndef DIS86_H
#define DIS86_H

#include <cstdint>

int dis86(const uint8_t *code, int len, uint16_t ip, char *buf, int bufsz);

#endif
