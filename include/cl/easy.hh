/*
 * Copyright (C) 2010 Kamil Dudka <kdudka@redhat.com>
 *
 * This file is part of predator.
 *
 * predator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * predator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with predator.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef H_GUARD_EASY_H
#define H_GUARD_EASY_H

/**
 * @file easy.hh
 * an easy interface for building new analyzer based on code listener
 */

namespace CodeStorage {
    struct Storage;
}

/**
 * entry point for @b easy analyzers based on code listener
 * @param stor an already built instance of CodeStorage::Storage that describes
 * the analyzed code (for read-only access)
 * @param configString a custom configuration string passed from the gcc
 * command-line (or another code parser that is used as gcc replacement)
 */
extern void clEasyRun(
        const CodeStorage::Storage      &stor,
        const char                      *configString);

#endif /* H_GUARD_EASY_H */
