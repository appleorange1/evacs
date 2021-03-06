#ifndef _VERIFY_BARCODE_H
#define _VERIFY_BARCODE_H
/* This file is (C) copyright 2001 Software Improvements, Pty Ltd */

/* This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

/* This file contains all the client barcode verification handling */
#include <stdbool.h>

struct barcode;

/* Is this initial barcode OK? */
extern bool verify_init_barcode(struct barcode *bc);

/* Is this confirming barcode OK? */
extern bool verify_barcode(struct barcode *bc);

#endif /*_VERIFY_BARCODE_H*/
