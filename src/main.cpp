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

#include <iostream>

#include "bitboard.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "uci.h"

using namespace Jieqi;

int main(int argc, char* argv[]) {

  std::cout << engine_info() << std::endl;

  CommandLine::init(argc, argv);
  UCI::init(Options);
  Tune::init();
  PSQT::init();
  Bitboards::init();
  Position::init();
  Threads.set(size_t(Options["Threads"]));
  Search::clear(); // After threads are up

  UCI::loop(argc, argv);

  Threads.set(0);
  return 0;
}
