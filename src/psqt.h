/*
  Jieqi, a UCI jieqi engine derived from Pikafish and Stockfish
  Copyright (C) 2026 The Jieqi developers
  Copyright (C) 2004-2023 The Stockfish developers (see AUTHORS file)

  Jieqi is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Jieqi is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef PSQT_H_INCLUDED
#define PSQT_H_INCLUDED


#include "types.h"


namespace Jieqi::PSQT
{

extern Score psq[PIECE_NB][SQUARE_NB];
extern Score psqCap[RANK_NB][int(FILE_NB) / 2 + 1];
// Fill psqt array from a set of internally linked parameters
void init();

} // namespace Jieqi::PSQT


#endif // PSQT_H_INCLUDED
