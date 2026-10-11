/*
  Jieqi, a UCI jieqi engine derived from Pikafish and Stockfish
  Copyright (C) 2026 The Jieqi developers
  Copyright (C) 2004-2022 The Stockfish developers (see AUTHORS file)

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

#ifndef EVALUATE_H_INCLUDED
#define EVALUATE_H_INCLUDED

#include <string>

#include "types.h"

namespace Jieqi {

class Position;

namespace Eval {

  std::string trace(Position& pos);
  Value evaluate(const Position& pos, int* complexity = nullptr);

} // namespace Eval

} // namespace Jieqi

#endif // #ifndef EVALUATE_H_INCLUDED
