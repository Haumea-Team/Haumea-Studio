// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
void MapCleanupPanel();
int MapCleanupDuplicateCount();
// The duplicate scan without the panel, for Namaka: copies of an object at
// the same place (within tolerance metres) and turn; strict also wants the
// same settings and materials. select leaves the copies selected; remove
// deletes them (one undo step). Returns how many copies; groups and
// protectedCount (objects other things depend on, never touched) are optional.
int MapCleanupDuplicates(float tolerance, bool onlySelected, bool strict, bool remove,
	int *groupCount = nullptr, int *protectedOut = nullptr);
