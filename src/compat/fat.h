// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal stand-in for the DS libfat header <fat.h>.
//
// SaveKey.cpp (the only core file that includes it) uses no libfat
// symbol at all — only opendir/mkdir/closedir from <dirent.h> and the
// MT24LC256 EEPROM file. The include is vestigial on the DS, where
// libfat was linked for the SD card; on the Q2 the EEPROM file lives
// on the SD card through the normal filesystem. Empty by design.
#ifndef Q2ATARI_COMPAT_FAT_H
#define Q2ATARI_COMPAT_FAT_H

#endif
