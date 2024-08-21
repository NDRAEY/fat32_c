#pragma once

#include <stdint.h>

void LFN2SFN(const char *lfn, char *sfn);
uint8_t lfn_checksum(const char sfn[11]); 