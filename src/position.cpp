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

#include <algorithm>
#include <cassert>
#include <cstddef> // For offsetof()
#include <cstring> // For std::memset, std::memcmp
#include <iomanip>
#include <sstream>
#include <vector>
#include <memory>

#include "bitboard.h"
#include "misc.h"
#include "position.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"

using std::string;

namespace Jieqi {

namespace Zobrist {

  Key psq[PIECE_NB][SQUARE_NB];
  // One key per (piece, pool count) pair. A pool holds at most 5 of a type, so only
  // indices 0..4 are ever read, but init() fills 7 per piece: the array must be at
  // least that large (it was [5], an out-of-bounds write in init()). Keeping the
  // fill loop as is keeps every Zobrist key bit-identical to upstream.
  Key psqDark[PIECE_NB][7];
  Key side;
}

namespace {

const string PieceToChar(" RACPNBK racpnbk XXXXXX  xxxxxx ");

// Number of pieces of each type a side starts with (index: PieceType)
constexpr int InitialCount[PIECE_TYPE_NB] = { 0, 2, 2, 2, 5, 2, 2, 1, 0, 0 };

// Square on Red's half whose starting piece type gives the movement of a face-down
// piece of colour c on s, or -1 if s is not one of c's starting squares.
int dark_origin(Color c, int s) {
    int t = (c == WHITE ? s : SQ_I9 - s);
    return (t >= 0 && t <= SQ_I4 && BPiece[t] != NO_PIECE_TYPE) ? t : -1;
}

constexpr Piece Pieces[] = { W_ROOK, W_ADVISOR, W_CANNON, W_PAWN, W_KNIGHT, W_BISHOP, W_KING,
                             B_ROOK, B_ADVISOR, B_CANNON, B_PAWN, B_KNIGHT, B_BISHOP, B_KING,
                             BW_ROOK, BW_ADVISOR, BW_CANNON, BW_PAWN, BW_KNIGHT, BW_BISHOP,
                             BB_ROOK, BB_ADVISOR, BB_CANNON, BB_PAWN, BB_KNIGHT, BB_BISHOP };
} // namespace

namespace PieceExchange {
    Piece charToPiece(unsigned char token) {
        if (token == 'x' || token == 'X')return NO_PIECE;
        size_t idx = PieceToChar.find(token);
        if (idx != string::npos) {
            return Piece(idx);
        }
        else
        {
            return NO_PIECE;
        }
    }
} // namespace


/// operator<<(Position) returns an ASCII representation of the position

std::ostream& operator<<(std::ostream& os, const Position& pos) {

  os << "\n +---+---+---+---+---+---+---+---+---+\n";


  for (Rank r = RANK_9; r >= RANK_0; --r)
  {
      for (File f = FILE_A; f <= FILE_I; ++f)
          os << " | " << PieceToChar[pos.piece_on(make_square(f, r))];

      os << " | " << r << "\n +---+---+---+---+---+---+---+---+---+\n";
  }

  os << "   a   b   c   d   e   f   g   h   i\n"
     << "\nFen: " << pos.fen() << "\nKey: " << std::hex << std::uppercase
     << std::setfill('0') << std::setw(16) << pos.key()
     << std::setfill(' ') << std::dec << "\nCheckers: ";

  for (Bitboard b = pos.checkers(); b; )
      os << UCI::square(pop_lsb(b)) << " ";

  return os;
}


/// Position::init() initializes at startup the various arrays used to compute hash keys

void Position::init() {

  PRNG rng(1070372);

  for (Piece pc : Pieces) {
      for (Square s = SQ_A0; s <= SQ_I9; ++s)
          Zobrist::psq[pc][s] = rng.rand<Key>();
      for (int i = 0; i <= 6; ++i)
          Zobrist::psqDark[pc][i] = rng.rand<Key>();
  }


  Zobrist::side = rng.rand<Key>();
}


/// Position::set() initializes the position object with the given FEN string.
/// This function is not very robust - make sure that input FENs are correct,
/// this is assumed to be the responsibility of the GUI.

Position& Position::set(const string& fenStr, StateInfo* si, Thread* th) {
/*
   A FEN string defines a particular position using only the ASCII character set.

   A FEN string contains six fields separated by a space. The fields are:

   1) Piece placement (from white's perspective). Each rank is described, starting
      with rank 9 and ending with rank 0. Within each rank, the contents of each
      square are described from file A through file I. Following the Standard
      Algebraic Notation (SAN), each piece is identified by a single letter taken
      from the standard English names. White pieces are designated using upper-case
      letters ("RACPNBK") whilst Black uses lowercase ("racpnbk"). Blank squares are
      noted using digits 1 through 9 (the number of blank squares), and "/"
      separates ranks.

   2) Active color. "w" means white moves next, "b" means black.

   3) Halfmove clock. This is the number of halfmoves since the last pawn advance
      or capture. This is used to determine if a draw can be claimed under the
      fifty-move rule.

   4) Fullmove number. The number of the full move. It starts at 1, and is
      incremented after Black's move.
*/

  unsigned char token;
  size_t idx;
  Square sq = SQ_A9;
  std::istringstream ss(fenStr);

  std::memset(this, 0, sizeof(Position));
  std::memset(si, 0, sizeof(StateInfo));
  st = si;

  ss >> std::noskipws;

  // 1. Piece placement
  // Squares outside the board and face-down pieces off their side's starting points
  // are skipped: a malformed FEN used to write outside board[] (ASan: stack-buffer-
  // overflow in put_piece) or create a face-down piece with no movement type.
  // fen_is_valid() rejects such FENs; this only keeps set() memory-safe.
  std::vector<std::pair<Piece, Square>> putPieces;
  int file = 0, rank = RANK_9;
  while ((ss >> token) && !isspace(token))
  {
      if (isdigit(token))
          file += token - '0';

      else if (token == '/')
          --rank, file = 0;

      else if ((idx = PieceToChar.find(token)) != string::npos && token != ' ') {
          if (file < FILE_NB && rank >= RANK_0) {
              sq = make_square(File(file), Rank(rank));
              if (token == 'x' || token == 'X') {
                  Color c = token == 'X' ? WHITE : BLACK;
                  int o = dark_origin(c, sq);
                  if (o >= 0)
                      putPieces.push_back(std::make_pair(Piece(make_piece(c, BPiece[o]) | 16), sq));
              }
              else
                  put_piece(Piece(idx), sq);
          }
          ++file;
      }
  }

    // 2. Active color
    ss >> token;
    sideToMove = (token == 'w' ? WHITE : BLACK);

  // 3. Pools, then the optional move counters. The fields are split on whitespace:
  // reading the pool character by character took the halfmove clock for the pool
  // when the pool field was empty (as fen() writes it), and lost the move number.
  std::vector<string> rest;
  string field;
  ss >> std::skipws;
  while (ss >> field)
      rest.push_back(field);
  size_t k = 0;
  string poolField;
  if (k < rest.size() && !isdigit((unsigned char)rest[k][0]))
      poolField = rest[k++];

  Piece pt = NO_PIECE;
  restPieces[WHITE].clear();
  restPieces[BLACK].clear();
  for (unsigned char c : poolField) {
      token = c;
      if ((idx = PieceToChar.find(token)) != string::npos && token != ' ') {
          if (token == 'x' || token == 'X')
              pt = NO_PIECE;
          else
              pt = Piece(idx);
      }

      // A count is only meaningful after a pool piece letter (not after x/X, a king,
      // or at the start of the field): color_of(NO_PIECE) asserts, and in release
      // builds the count was silently added to Red's pool as NO_PIECE.
      // The pools are capped at a side's initial set (15 pieces, at most 5 of a
      // type): larger counts overflowed RestList's fixed arrays (segfault).
      if (isdigit(token) && pt != NO_PIECE && type_of(pt) != KING) {
          RestList& rl = restPieces[color_of(pt)];
          for (int i = 0; i < token - '0'; i++)
              if (rl.countType(type_of(pt)) < InitialCount[type_of(pt)] && rl.size() < 15)
                  rl.push_back(pt);
      }
  }
  for (size_t i = 0; i < putPieces.size(); i++)
  {
      put_piece(putPieces.at(i).first, putPieces.at(i).second);
  }

  // 4-5. Halfmove clock (not used) and fullmove number; extra "-" fields of a
  // standard xiangqi FEN ("w - - 0 1") are skipped.
  while (k < rest.size() && rest[k] == "-")
      ++k;
  gamePly = 1;
  if (k + 1 < rest.size())
      gamePly = atoi(rest[k + 1].c_str());

  // Convert from fullmove starting from 1 to gamePly starting from 0,
  // handle also common incorrect FEN with fullmove = 0.
  gamePly = std::max(2 * (gamePly - 1), 0) + (sideToMove == BLACK);

  thisThread = th;
  set_state(st);

  assert(pos_is_ok());

  return *this;
}


/// Position::fen_is_valid() checks a jieqi FEN strictly, so that set() and the rest
/// of the engine can rely on it: 10 ranks of 9 files, one king per side inside its
/// palace, face-down pieces only on their own side's starting points, pools made of
/// "<piece><digit>" pairs, no more pieces of a type than a side starts with, at least
/// one pool identity per face-down piece, and the side not to move not in check
/// (including the flying general). The pool field may be "-" or empty, and extra "-"
/// fields (standard xiangqi FEN "w - - 0 1") are accepted.

bool Position::fen_is_valid(const string& fenStr, string& err) {

  std::istringstream ss(fenStr);
  string board, side, tok;
  if (!(ss >> board >> side)) { err = "missing board or side to move"; return false; }

  int light[COLOR_NB][PIECE_TYPE_NB] = {}, dark[COLOR_NB] = {}, kings[COLOR_NB] = {};
  int file = 0, rank = RANK_9;
  for (char c : board)
  {
      if (c == '/')
      {
          if (file != FILE_NB) { err = "rank " + std::to_string(rank) + " has " + std::to_string(file) + " files"; return false; }
          if (--rank < RANK_0) { err = "more than 10 ranks"; return false; }
          file = 0;
          continue;
      }
      if (c >= '1' && c <= '9')
      {
          if ((file += c - '0') > FILE_NB) { err = "rank " + std::to_string(rank) + " has more than 9 files"; return false; }
          continue;
      }
      size_t idx = PieceToChar.find(c);
      if (c == ' ' || idx == string::npos) { err = string("invalid character '") + c + "' in board"; return false; }
      if (file >= FILE_NB) { err = "rank " + std::to_string(rank) + " has more than 9 files"; return false; }
      Square s = make_square(File(file), Rank(rank));
      if (c == 'X' || c == 'x')
      {
          Color col = c == 'X' ? WHITE : BLACK;
          if (dark_origin(col, s) < 0) { err = "face-down piece on " + UCI::square(s) + ", not a starting point of its side"; return false; }
          dark[col]++;
      }
      else
      {
          Piece pc = Piece(idx);
          if (type_of(pc) == KING)
          {
              kings[color_of(pc)]++;
              // Palace holds both palaces: a king in the other side's palace passed.
              if (!(Palace & HalfBB[color_of(pc)] & s)) { err = "king outside its own palace on " + UCI::square(s); return false; }
          }
          else
              light[color_of(pc)][type_of(pc)]++;
      }
      ++file;
  }
  if (rank != RANK_0 || file != FILE_NB) { err = "board must have 10 ranks of 9 files"; return false; }
  if (kings[WHITE] != 1 || kings[BLACK] != 1) { err = "each side needs exactly one king"; return false; }
  if (side != "w" && side != "b") { err = "side to move must be 'w' or 'b'"; return false; }

  std::vector<string> rest;
  while (ss >> tok)
      rest.push_back(tok);
  size_t k = 0;
  string pool;
  if (k < rest.size() && !isdigit((unsigned char)rest[k][0]))
      pool = rest[k++];
  if (pool == "-")
      pool.clear();

  int inPool[COLOR_NB][PIECE_TYPE_NB] = {}, poolSize[COLOR_NB] = {};
  for (size_t i = 0; i < pool.size(); i += 2)
  {
      size_t idx = string("RACPNBracpnb").find(pool[i]);
      if (idx == string::npos) { err = string("invalid pool piece '") + pool[i] + "'"; return false; }
      if (i + 1 >= pool.size() || !isdigit((unsigned char)pool[i + 1])) { err = string("pool piece '") + pool[i] + "' must be followed by one digit"; return false; }
      Piece pc = Piece(PieceToChar.find(pool[i]));
      inPool[color_of(pc)][type_of(pc)] += pool[i + 1] - '0';
      poolSize[color_of(pc)] += pool[i + 1] - '0';
  }

  while (k < rest.size() && rest[k] == "-")
      ++k;
  for (int n = 0; k < rest.size() && n < 2; ++k, ++n)
      if (rest[k].find_first_not_of("0123456789") != string::npos) { err = "move counters must be numbers"; return false; }

  for (Color c : { WHITE, BLACK })
  {
      for (PieceType pt : { ROOK, ADVISOR, CANNON, PAWN, KNIGHT, BISHOP })
          if (light[c][pt] + inPool[c][pt] > InitialCount[pt])
          {
              err = string("too many '") + PieceToChar[make_piece(c, pt)] + "' (face-up plus pool > " + std::to_string(InitialCount[pt]) + ")";
              return false;
          }
      if (dark[c] > poolSize[c])
      {
          err = std::to_string(dark[c]) + " face-down " + (c == WHITE ? "red" : "black") + " pieces but only " + std::to_string(poolSize[c]) + " identities in the pool";
          return false;
      }
  }

  // The text is well formed, so set() is safe. Check the position itself.
  auto si = std::make_unique<StateInfo>();
  auto p  = std::make_unique<Position>();
  p->set(fenStr, si.get(), nullptr);
  Color them = ~p->side_to_move();
  if (p->checkers_to(p->side_to_move(), p->square<KING>(them))) { err = "the side not to move is in check"; return false; }
  if (attacks_bb<ROOK>(p->square<KING>(WHITE), p->pieces()) & p->pieces(BLACK, KING)) { err = "the kings face each other"; return false; }

  return true;
}


/// Position::set_check_info() sets king attacks to detect if a move gives check

void Position::set_check_info(StateInfo* si) const {

  Color  us   = sideToMove;
  Square uksq = square<KING>( us);
  Square oksq = square<KING>(~us);

  
  si->blockersForKing[ us] = blockers_for_king(pieces(~us), uksq, si->pinners[~us]);
  si->blockersForKing[~us] = blockers_for_king(pieces( us), oksq, si->pinners[ us]);

  // We have to take special cares about the cannon and checks
  si->needSlowCheck = checkers() || (attacks_bb<ROOK>(uksq) & pieces(~us, CANNON));

  si->checkSquares[PAWN]   = pawn_attacks_to_bb(sideToMove, oksq);
  si->checkSquares[KNIGHT] = attacks_bb<KNIGHT_TO>(oksq, pieces());
  si->checkSquares[CANNON] = attacks_bb<CANNON>(oksq, pieces());
  si->checkSquares[ROOK]   = attacks_bb<ROOK>(oksq, pieces());
  //士相将军
  si->checkSquares[ADVISOR] = attacks_bb<ADVISOR>(oksq, pieces());
  si->checkSquares[BISHOP] = attacks_bb<BISHOP>(oksq, pieces());
  si->checkSquares[KING] = 0;
  // The remaining slots were never written, but generate<QUIET_CHECKS> reads
  // check_squares(ADVISOR_B) for face-down advisors (valgrind: conditional jump on an
  // uninitialised value in generate_moves<..., ADVISOR_B, QUIET_CHECKS>). A face-down
  // advisor only reaches its own palace centre, so it can never give check.
  si->checkSquares[NO_PIECE_TYPE] = 0;
  si->checkSquares[KNIGHT_TO] = 0;
  si->checkSquares[ADVISOR_B] = 0;
}


/// Position::set_state() computes the hash keys of the position, and other
/// data that once computed is updated incrementally as moves are made.
/// The function is only used when a new position is set up, and to verify
/// the correctness of the StateInfo data when running in debug mode.

void Position::set_state(StateInfo* si) const {

  si->key = si->materialKey = 0;
  si->material[WHITE] = si->material[BLACK] = VALUE_ZERO;
  si->checkersBB = checkers_to(~sideToMove, square<KING>(sideToMove));
  si->move = MOVE_NONE;
  si->darkDepth = 0;
  si->darkTypes = 1;

  set_check_info(si);

  for (Bitboard b = pieces(); b; )
  {
      Square s = pop_lsb(b);
      Piece pc = piece_on(s);
      si->key ^= Zobrist::psq[pc][s];

      if (type_of(pc) != KING)
          si->material[color_of(pc)] += Darkof(pc) == UNKNOWN ?
                        restPieces[color_of(pc)].evgValue() :
                        PieceValue[MG][pc];
  }
  for (int t = 0; t < PIECE_TYPE_NB; t++)
  {
      PieceType pt = PieceType(t);
      for (int j = 0; j < restPieces[WHITE].countType(pt); j++)
      {
          si->key ^= Zobrist::psqDark[make_piece(WHITE, pt)][j];
      }
      for (int j = 0; j < restPieces[BLACK].countType(pt); j++)
      {
          si->key ^= Zobrist::psqDark[make_piece(BLACK, pt)][j];
      }
        
  }

  if (sideToMove == BLACK)
      si->key ^= Zobrist::side;

  for (Piece pc : Pieces)
      for (int cnt = 0; cnt < pieceCount[pc]; ++cnt)
          si->materialKey ^= Zobrist::psq[pc][cnt];
}


/// Position::fen() returns a FEN representation of the position.

string Position::fen() const {

  int emptyCnt;
  std::ostringstream ss;
  
  for (Rank r = RANK_9; r >= RANK_0; --r)
  {
      for (File f = FILE_A; f <= FILE_I; ++f)
      {
          for (emptyCnt = 0; f <= FILE_I && empty(make_square(f, r)); ++f)
              ++emptyCnt;

          if (emptyCnt)
              ss << emptyCnt;

          if (f <= FILE_I)
              ss << PieceToChar[piece_on(make_square(f, r))];
      }

      if (r > RANK_0)
          ss << '/';
  }

  ss << (sideToMove == WHITE ? " w " : " b ");

  int DarkNum[PIECE_NB];
  memset(DarkNum, 0, sizeof(DarkNum));
  for (int i = 0; i < restPieces[WHITE].size(); i++) {
      Piece p = restPieces[WHITE].at(i);
      if (p != NO_PIECE)
      {
          DarkNum[p]++;
      }
  }
  for (int i = 0; i < restPieces[BLACK].size(); i++) {
      Piece p = restPieces[BLACK].at(i);
      if (p != NO_PIECE)
      {
          DarkNum[p]++;
      }
  }
  for (int i = 1; i < PIECE_NB; i++)
  {
      if (DarkNum[i] > 0) {
          ss << PieceToChar[Piece(i)] << DarkNum[i];
      }
  }
  ss << " " << 0 << " " << 1 + (gamePly - (sideToMove == BLACK)) / 2;

  return ss.str();
}


/// Position::blockers_for_king() returns a bitboard of all the pieces (both colors)
/// that are blocking attacks on the square 's' from 'sliders'. A piece blocks a
/// slider if removing that piece from the board would result in a position where
/// square 's' is attacked. For example, a king-attack blocking piece can be either
/// a pinned or a discovered check piece, according if its color is the opposite
/// or the same of the color of the slider.

Bitboard Position::blockers_for_king(Bitboard sliders, Square s, Bitboard& pinners) const {
  // TODO: 这个函数也不正确惹qwq，需要考虑象的blocker
  Bitboard blockers = 0;
  pinners = 0;

  // Snipers are pieces that attack 's' when a piece and other pieces are removed
  Bitboard snipers = (  (attacks_bb<  ROOK>(s) & (pieces(ROOK) | pieces(CANNON) | pieces(KING)))
                      | (attacks_bb<KNIGHT>(s) & pieces(KNIGHT))
                      | (attacks_bb<BISHOP>(s) & pieces(BISHOP))) & sliders;
  Bitboard occupancy = pieces() ^ (snipers & ~pieces(CANNON));

  while (snipers)
  {
    Square sniperSq = pop_lsb(snipers);
    bool isCannon = type_of(piece_on(sniperSq)) == CANNON;
    Bitboard b = between_bb(s, sniperSq) & (isCannon ? pieces() ^ sniperSq : occupancy);

    if (b && ((!isCannon && !more_than_one(b)) || (isCannon && popcount(b) == 2)))
    {
        blockers |= b;
        if (b & pieces(color_of(piece_on(s))))
            pinners |= sniperSq;
    }
  }
  return blockers;
}


/// Position::attackers_to() computes a bitboard of all pieces which attack a
/// given square. Slider attacks use the occupied bitboard to indicate occupancy.

Bitboard Position::attackers_to(Square s, Bitboard occupied) const {
  // TODO: 暗士
  return  (pawn_attacks_to_bb(WHITE, s)       & pieces(WHITE, PAWN))
        | (pawn_attacks_to_bb(BLACK, s)       & pieces(BLACK, PAWN))
        | (attacks_bb<KNIGHT_TO>(s, occupied) & pieces( KNIGHT))
        | (attacks_bb<     ROOK>(s, occupied) & pieces(   ROOK))
        | (attacks_bb<   CANNON>(s, occupied) & pieces( CANNON))
        | (attacks_bb<   BISHOP>(s, occupied) & pieces( BISHOP))
        | (attacks_bb<  ADVISOR>(s)           & pieces(ADVISOR))
        | (attacks_bb<ADVISOR_B>(s)           & pieces(ADVISOR_B))
        | (attacks_bb<     KING>(s)           & pieces(   KING));
}


/// Position::checkers_to() computes a bitboard of all pieces of a given color
/// which gives check to a given square. Slider attacks use the occupied bitboard
/// to indicate occupancy.

Bitboard Position::checkers_to(Color c, Square s, Bitboard occupied) const {
    return ( (pawn_attacks_to_bb(c, s)           & pieces(   PAWN))
           | (attacks_bb<KNIGHT_TO>(s, occupied) & pieces( KNIGHT))
           | (attacks_bb<   BISHOP>(s, occupied) & pieces(BISHOP))
           | (attacks_bb<  ADVISOR>(s, occupied) & pieces(ADVISOR))
           | (attacks_bb<     ROOK>(s, occupied) & pieces(   ROOK))
           | (attacks_bb<   CANNON>(s, occupied) & pieces( CANNON)) ) & pieces(c);
}


/// Position::legal() tests whether a pseudo-legal move is legal

bool Position::legal(Move m) const {

  assert(is_ok(m));

  Color us = sideToMove;
  Square from = from_sq(m);
  Square to = to_sq(m);
  Bitboard occupied = (pieces() ^ from) | to;
  Square ksq = type_of(moved_piece(m)) == KING ? to : square<KING>(us);

  assert(color_of(moved_piece(m)) == us);
  assert(piece_on(square<KING>(us)) == make_piece(us, KING));

  // A non-king move is always legal when not moving the king or a pinned piece if we don't need slow check
  if (!st->needSlowCheck && ksq != to && !(blockers_for_king(us) & from))
      return true;

  // Flying general rule
  if (attacks_bb<ROOK>(ksq, occupied) & pieces(~us, KING))
      return false;

  // If the moving piece is a king, check whether the destination square is
  // attacked by the opponent.
  if (type_of(piece_on(from)) == KING)
      return !(checkers_to(~us, to, occupied));

  // A non-king move is legal if the king is not under attack after the move.
  return !(checkers_to(~us, ksq, occupied) & ~square_bb(to));
}


/// Position::pseudo_legal() takes a random move and tests whether the move is
/// pseudo legal. It is used to validate moves from TT that can be corrupted
/// due to SMP concurrent access or hash position key aliasing.

bool Position::pseudo_legal(const Move m) const {

  Color us = sideToMove;
  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece pc = moved_piece(m);

  // If the 'from' square is not occupied by a piece belonging to the side to
  // move, the move is obviously not legal.
  if (pc == NO_PIECE || color_of(pc) != us)
      return false;

  // The destination square cannot be occupied by a friendly piece
  if (pieces(us) & to)
      return false;

  // Handle the special cases
  if (type_of(pc) == PAWN)
      return pawn_attacks_bb(us, from) & to;
  else if (type_of(pc) == CANNON && !capture(m))
      return attacks_bb<ROOK>(from, pieces()) & to;
  else
      return attacks_bb(type_of(pc), from, pieces()) & to;
}

/// Position::gives_check() tests whether a pseudo-legal move gives a check

bool Position::gives_check(Move m, PieceType flipped) {

  assert(is_ok(m));
  assert(color_of(moved_piece(m)) == sideToMove);

  Square from = from_sq(m);
  Square to = to_sq(m);
  Square ksq = square<KING>(~sideToMove);
  PieceType pt;

  if (isDark(from)) {
      // Identity unknown: skip the direct-check test, but the discovered / screen
      // checks below do not depend on the identity.
      pt = flipped ? flipped : NO_PIECE_TYPE;
  }
  else
  {
      pt = type_of(moved_piece(m));
  }
  
  //PieceType pt = type_of(moved_piece(m));

  // Is there a direct check?
  if (pt == CANNON) {
      if (attacks_bb<CANNON>(to, (pieces() ^ from) | to) & ksq)
          return true;
  } else if (check_squares(pt) & to)
      return true;

  // Is there a discovered check?
  if (attacks_bb<ROOK>(ksq) & pieces(sideToMove, CANNON))
      return checkers_to(sideToMove, ksq, (pieces() ^ from) | to) & ~square_bb(from);
  else if ((blockers_for_king(~sideToMove) & from) && !aligned(from, to, ksq))
      return true;

  return false;
}

bool Position::getDark(StateInfo& newSt, int& typecount, bool& isDarkDepth) {
    assert(&newSt != st);
    Square ds = st->darkSquare;
    if (ds == SQ_NONE)return false;
    Color us = ~sideToMove;
    Piece pc = NO_PIECE;
    typecount = 0;
    const int evgOld = restPieces[us].evgValueRaw();   // pool average before the reveal
    isDarkDepth = st->darkDepth > MAXDARKDEPTH || st->darkTypes > MAXDARKTYPES;
    if (st->darkDepth - MAXDARKDEPTH > QDARKDEPTH)return false;
    Key poolKey = 0;
    while (st->darkTypeIndex < BISHOP)
    {
        st->darkTypeIndex++;
        PieceType t;
        t = PieceType(st->darkTypeIndex);
        
        pc = restPieces[us].pop_back(t);
        if (pc == NO_PIECE)continue;
        typecount = restPieces[us].countType(t);
        poolKey = Zobrist::psqDark[pc][typecount];
        typecount++;
        break;
    }
    if (pc == NO_PIECE)return false;

    // The revealed position differs from the chance node only by the pool and by the
    // identity of the piece on ds; the side to move is the same. Its key is therefore
    // the chance node's key with those two changes, and nothing else. In particular the
    // chance node's own key and the bloom filter are left alone: the chance node is not
    // part of the game history (newSt.previous skips it), and its parent was already
    // added to the filter by do_move().
    thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
    Key k = st->key ^ poolKey;
    st->darkPsq = psq;
    std::memcpy(&newSt, st, offsetof(StateInfo, key));
    newSt.previous = st->previous;
    newSt.previousDark = st;
    // Fields after 'key' are not copied by the memcpy above. The revealed state stands
    // for the position after st->move, so it inherits the move and the captured piece;
    // search reads both (priorCapture, captured_piece(), chase detection).
    newSt.move = st->move;
    newSt.capturedPiece = st->capturedPiece;
    newSt.darkPiece = NO_PIECE;
    newSt.darkSquare = SQ_NONE;
    newSt.darkTypeIndex = NO_PIECE_TYPE;
    st = &newSt;
    //++gamePly;
    //++st->pliesFromNull;
    Color them = ~us;

    Piece old = piece_on(ds);
    assert(color_of(old) == us);
    // Update hash key
    k ^= Zobrist::psq[old][ds] ^ Zobrist::psq[pc][ds];
    //replcae
    // psq / material / materialKey. The dark piece is priced at the pool average evgOld
    // on its starting square (move_piece() does not move a dark piece's psq term). Replace
    // it by the revealed piece on ds and re-price the other dark pieces of `us`, whose
    // average changed when pc left the pool. Before this, psq and material[] kept the
    // dark price (material got a flat +69) and materialKey was not updated at all, so the
    // material hash returned the imbalance of whichever identity was evaluated first.
    psq -= dark_score(us, from_sq(st->move), evgOld);
    remove_piece(ds,false);
    st->materialKey ^= Zobrist::psq[old][pieceCount[old]];
    put_piece(pc, ds, false);
    st->materialKey ^= Zobrist::psq[pc][pieceCount[pc] - 1];
    psq += PSQT::psq[pc][ds];
    st->material[us] += PieceValue[MG][pc] - evgOld;
    reprice_dark(us, evgOld);


    // Update the key with the final value
    st->key = k;

    // Calculate checkers bitboard (if move gives check)
    st->checkersBB =  checkers_to(us, square<KING>(them));

    // Update king attacks used for fast check detection
    set_check_info(st);

    assert(pos_is_ok());

    return true;
}

/// Position::dark_score() is the psq term of one face-down piece of colour c on s
/// when its pool averages evg: the term put_piece()/remove_piece() add and remove.

Score Position::dark_score(Color c, Square s, int evg) const {
    int v = (c == WHITE ? evg : -evg);
    File f = File(edge_distance(file_of(s)));
    if (f > FILE_E) --f;
    return make_score(v, v) + PSQT::psqCap[rank_of(s)][f];
}

/// Position::reprice_dark() is called after c's pool changed (its average was evgOld).
/// Every face-down piece of c still on the board is priced at the pool average in psq
/// and material[], so all of them move by the change of the average. This keeps both
/// equal to what set() / set_state() compute for the same position.

void Position::reprice_dark(Color c, int evgOld) {
    int n = 0;
    for (PieceType pt : { ROOK, ADVISOR, CANNON, PAWN, KNIGHT, BISHOP })
        n += pieceCount[make_piece(c, pt) ^ 16];
    int d = n * (restPieces[c].evgValueRaw() - evgOld);
    st->material[c] += d;
    int s = (c == WHITE ? d : -d);
    psq += make_score(s, s);
}

void Position::setDark() {
    // Finally point our state pointer back to the previous state
    st = st->previousDark;

    assert(st->darkSquare != SQ_NONE);
    assert(!isDark(st->darkSquare));
    assert(Darkof(st->darkPiece) == UNKNOWN);

    //update rest
    Piece p = piece_on(st->darkSquare);
    restPieces[~sideToMove].push_back(p);

    //replcae
    remove_piece(st->darkSquare, false);
    put_piece(st->darkPiece, st->darkSquare, false);
    psq = st->darkPsq;
    //--gamePly;

    // getDark() changed neither the chance node's key nor the bloom filter, so there
    // is nothing to undo here.

    assert(pos_is_ok());
}



/// Position::do_move() makes a move, and saves all information necessary
/// to a StateInfo object. The move is assumed to be legal. Pseudo-legal
/// moves should be filtered out before this function is called.

bool Position::do_move(Move m, StateInfo& newSt, bool givesCheck) {

  assert(is_ok(m));
  assert(&newSt != st);
  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece to_pc = get_Piece(m);
  if (to_pc) {
      // Identity supplied by the GUI: reveal the piece on `from` permanently, in the
      // current (history) state, before moving it.
      Piece darkPc = piece_on(from);
      // UCI's parse_move() guarantees these; they used to be trusted blindly.
      assert(Darkof(darkPc) == UNKNOWN);
      assert(color_of(to_pc) == sideToMove && type_of(to_pc) != KING);
      assert(restPieces[sideToMove].countType(type_of(to_pc)) > 0);
      const int evgOld = restPieces[sideToMove].evgValueRaw();
      remove_piece(from);
      st->materialKey ^= Zobrist::psq[darkPc][pieceCount[darkPc]];
      restPieces[sideToMove].pop_back(type_of(to_pc));
      st->key ^= Zobrist::psqDark[to_pc][restPieces[sideToMove].countType(type_of(to_pc))];
      put_piece(to_pc,from);
      st->key ^= Zobrist::psq[darkPc][from] ^ Zobrist::psq[to_pc][from];
      st->materialKey ^= Zobrist::psq[to_pc][pieceCount[to_pc] - 1];
      st->material[sideToMove] += PieceValue[MG][to_pc] - evgOld;
      reprice_dark(sideToMove, evgOld);
  }

  Piece old = piece_on(from);
  bool dark = Darkof(old);
  // Update the bloom filter
  ++filter[st->key];

  thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
  Key k = st->key ^ Zobrist::side;

  // Copy some fields of the old state to our new StateInfo object except the
  // ones which are going to be recalculated from scratch anyway and then switch
  // our state pointer to point to the new (ready to be updated) state.
  std::memcpy(&newSt, st, offsetof(StateInfo, key));
  newSt.previous = st;
  st = &newSt;
  st->move = m;

  // Increment ply counters.
  ++gamePly;
  ++st->pliesFromNull;

  Color us = sideToMove;
  Color them = ~us;

  Piece pc = piece_on(from);
  Piece captured = piece_on(to);

  assert(color_of(pc) == us);
  assert(captured == NO_PIECE || color_of(captured) == them);
  if (type_of(captured) == KING) {
      sync_cout << *this << sync_endl;
  }
  assert(type_of(captured) != KING);

  if (captured)
  {
      Square capsq = to;

      
      if (Darkof(captured) == UNKNOWN)
      {
          st->material[them] -= restPieces[them].evgValue();
      }
      else
      {
          st->material[them] -= PieceValue[MG][captured];
      }

      // Update board and piece lists
      remove_piece(capsq);
      Piece capPiece = cap_Piece(m);
      if (capPiece) {
          assert(Darkof(captured) == UNKNOWN);
          assert(color_of(capPiece) == them && type_of(capPiece) != KING);
          assert(restPieces[them].countType(type_of(capPiece)) > 0);
          const int evgThemOld = restPieces[them].evgValueRaw();
          restPieces[them].pop_back(type_of(capPiece));
          // st is already the new state here, whose key is overwritten with k below:
          // the pool change must go into k.
          k ^= Zobrist::psqDark[capPiece][restPieces[them].countType(type_of(capPiece))];
          reprice_dark(them, evgThemOld);
      }

      // Update hash key
      k ^= Zobrist::psq[captured][capsq];
      st->materialKey ^= Zobrist::psq[captured][pieceCount[captured]];
      prefetch(thisThread->materialTable[st->materialKey]);
  }
  // Update hash key
  k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

  // Move the piece.
  move_piece(from, to);

  // Set capture piece
  st->capturedPiece = captured;

  // Update the key with the final value
  st->key = k;

  // Calculate checkers bitboard (if move gives check)
  st->checkersBB = givesCheck ? checkers_to(us, square<KING>(them)) : Bitboard(0);

  sideToMove = ~sideToMove;

  // Update king attacks used for fast check detection
  set_check_info(st);

  assert(pos_is_ok());

  if (dark) {
      st->darkPiece = pc;
      st->darkSquare = to;
      st->darkTypeIndex = NO_PIECE_TYPE;
      st->darkDepth++;
      st->darkTypes *= restPieces[us].notNullTypeCount();
  }
  else
  {
      st->darkSquare = SQ_NONE;
  }

  return dark;
}


/// Position::undo_move() unmakes a move. When it returns, the position should
/// be restored to exactly the same state as before the move was made.

void Position::undo_move(Move m) {

  assert(is_ok(m));

  sideToMove = ~sideToMove;

  Square from = from_sq(m);
  Square to = to_sq(m);

  assert(empty(from));
  assert(type_of(st->capturedPiece) != KING);


  move_piece(to, from); // Put the piece back at the source square

  if (st->capturedPiece)
  {
      Square capsq = to;

      put_piece(st->capturedPiece, capsq); // Restore the captured piece
  }

  // Finally point our state pointer back to the previous state
  st = st->previous;

  if (isDark(from) == UNKNOWN) {
      st->darkSquare = SQ_NONE;
  }

  --gamePly;

  // Update the bloom filter
  --filter[st->key];

  assert(pos_is_ok());
}


/// Position::do_null_move() is used to do a "null move": it flips
/// the side to move without executing any move on the board.

void Position::do_null_move(StateInfo& newSt) {

  assert(!checkers());
  assert(&newSt != st);

  // Update the bloom filter
  ++filter[st->key];

  std::memcpy(&newSt, st, sizeof(StateInfo));

  newSt.previous = st;
  st = &newSt;


  st->key ^= Zobrist::side;
  prefetch(TT.first_entry(key()));

  st->pliesFromNull = 0;

  sideToMove = ~sideToMove;

  set_check_info(st);

  assert(pos_is_ok());
}


/// Position::undo_null_move() must be used to undo a "null move"

void Position::undo_null_move() {

  assert(!checkers());

  st = st->previous;
  sideToMove = ~sideToMove;

  // Update the bloom filter
  --filter[st->key];
}


/// Position::key_after() computes the new hash key after the given move. Needed
/// for speculative prefetch.

Key Position::key_after(Move m) const {

  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece pc = piece_on(from);
  Piece captured = piece_on(to);
  Key k = st->key ^ Zobrist::side;

  if (captured)
      k ^= Zobrist::psq[captured][to];

 return k ^ Zobrist::psq[pc][to] ^ Zobrist::psq[pc][from];
}


/// Position::see_ge (Static Exchange Evaluation Greater or Equal) tests if the
/// SEE value of move is greater or equal to the given threshold. We'll use an
/// algorithm similar to alpha-beta pruning with a null window.

bool Position::see_ge(Move m, Value threshold) const {

  assert(is_ok(m));

  Square from = from_sq(m), to = to_sq(m);

  int swap = value_on(to) - threshold;
  if (swap < 0)
      return false;

  swap = value_on(from) - swap;
  if (swap <= 0)
      return true;

  assert(color_of(piece_on(from)) == sideToMove);
  Bitboard occupied = pieces() ^ from ^ to;
  Color stm = sideToMove;
  Bitboard attackers = attackers_to(to, occupied);

  // Flying general
  if (attackers & pieces(stm, KING))
      attackers |= attacks_bb<ROOK>(to, occupied & ~pieces(ROOK)) & pieces(~stm, KING);
  if (attackers & pieces(~stm, KING))
      attackers |= attacks_bb<ROOK>(to, occupied & ~pieces(ROOK)) & pieces(stm, KING);

  Bitboard nonCannons = attackers & ~pieces(CANNON);
  Bitboard cannons = attackers & pieces(CANNON);
  Bitboard stmAttackers, bb;
  int res = 1;

  while (true)
  {
      stm = ~stm;
      attackers &= occupied;

      // If stm has no more attackers then give up: stm loses
      if (!(stmAttackers = attackers & pieces(stm)))
          break;

      // Don't allow pinned pieces to attack as long as there are
      // pinners on their original square.
      if (pinners(~stm) & occupied)
      {
          stmAttackers &= ~blockers_for_king(stm);

          if (!stmAttackers)
              break;
      }

      res ^= 1;

      // Locate and remove the next least valuable attacker, and add to the
      // bitboard 'attackers' any protential attackers when it is removed.
      // Each attacker is valued with value_on(), like the first mover and the
      // captured piece above: a face-down piece is worth the average of its pool,
      // not the value of the piece type that its starting point gives it.
      if ((bb = stmAttackers & pieces(PAWN)))
      {
          if ((swap = value_on(lsb(bb)) - swap) < res)
              break;

          occupied ^= least_significant_square_bb(bb);
          nonCannons |= attacks_bb<ROOK>(to, occupied) & pieces(ROOK);
          cannons = attacks_bb<CANNON>(to, occupied) & pieces(CANNON);
          attackers = nonCannons | cannons;
      }

      else if ((bb = stmAttackers & pieces(ADVISOR, ADVISOR_B)))
      {
          if ((swap = value_on(lsb(bb)) - swap) < res)
              break;

          occupied ^= least_significant_square_bb(bb);
          // The advisor stood diagonally next to 'to', on a square that is both a
          // knight's leg and a bishop's eye: either piece may now attack 'to'.
          nonCannons |= (attacks_bb<KNIGHT_TO>(to, occupied) & pieces(KNIGHT))
                      | (attacks_bb<BISHOP>(to, occupied) & pieces(BISHOP));
          attackers = nonCannons | cannons;
      }

      else if ((bb = stmAttackers & pieces(BISHOP)))
      {
          if ((swap = value_on(lsb(bb)) - swap) < res)
              break;

          occupied ^= least_significant_square_bb(bb);
      }

      else if ((bb = stmAttackers & pieces(CANNON)))
      {
          if ((swap = value_on(lsb(bb)) - swap) < res)
              break;

          occupied ^= least_significant_square_bb(bb);
          cannons = attacks_bb<CANNON>(to, occupied) & pieces(CANNON);
          attackers = nonCannons | cannons;
      }

      else if ((bb = stmAttackers & pieces(KNIGHT)))
      {
          if ((swap = value_on(lsb(bb)) - swap) < res)
              break;

          occupied ^= least_significant_square_bb(bb);
      }

      else if ((bb = stmAttackers & pieces(ROOK)))
      {
          if ((swap = value_on(lsb(bb)) - swap) < res)
              break;

          occupied ^= least_significant_square_bb(bb);
          nonCannons |= attacks_bb<ROOK>(to, occupied) & pieces(ROOK);
          cannons = attacks_bb<CANNON>(to, occupied) & pieces(CANNON);
          attackers = nonCannons | cannons;
      }

      else // KING
           // If we "capture" with the king but opponent still has attackers,
           // reverse the result.
          return (attackers & ~pieces(stm)) ? res ^ 1 : res;
  }

  return bool(res);
}


/// light_do_move() just like do move, but a little lighter

std::pair<Piece, int> Position::light_do_move(Move m) {

    Square from = from_sq(m);
    Square to = to_sq(m);
    Piece captured = piece_on(to);
    int id = idBoard[to];

    // Update id board
    idBoard[to] = idBoard[from];
    idBoard[from] = 0;

    if (captured)
        // Update board and piece lists
        remove_piece(to,false);

    move_piece(from, to);

    sideToMove = ~sideToMove;

    return { captured, id };
}


/// light_undo_move() just like undo move, but a little lighter

void Position::light_undo_move(Move m, Piece captured, int id) {

    sideToMove = ~sideToMove;

    Square from = from_sq(m);
    Square to = to_sq(m);

    // Put back id board
    idBoard[from] = idBoard[to];
    idBoard[to] = id;

    move_piece(to, from); // Put the piece back at the source square

    if (captured)
    {
        Square capsq = to;

        put_piece(captured, capsq,false); // Restore the captured piece
    }
}


/// Position::set_chase_info() sets the chase information from state st - d to state st

void Position::set_chase_info(int d, uint16_t* chase) {

    // Grant each piece on board a unique id for each side
    int whiteId = 0;
    int blackId = 0;
    for (Square s = SQ_A0; s <= SQ_I9; ++s)
        if (board[s] != NO_PIECE)
            idBoard[s] = color_of(board[s]) == WHITE ? whiteId++ : blackId++;

    // Rollback until we reached st - d. chase[i] receives the chase information of
    // the state i plies before the starting one. It used to be written into
    // StateInfo::chased, but the states before the root are shared by all search
    // threads (ThreadPool::start_thinking), so that was a data race (TSan).
    for (int i = 0; i < d; ++i) {
        ChaseMap newChase = chased(~sideToMove);
        light_undo_move(st->move, st->capturedPiece);
        st = st->previous;
        // Take the exact diff to detect the chase
        chase[i] = newChase & chased(sideToMove);
    }
}


/// Position::chase_legal() tests whether a pseudo-legal move is chase legal

bool Position::chase_legal(Move m, Bitboard b) const {

    assert(is_ok(m));

    Color us = sideToMove;
    Square from = from_sq(m);
    Square to = to_sq(m);
    Bitboard occupied = (pieces() ^ from) | to;

    assert(color_of(moved_piece(m)) == us);
    assert(piece_on(square<KING>(us)) == make_piece(us, KING));

    // Flying general rule
    Square ksq = type_of(moved_piece(m)) == KING ? to : square<KING>(us);
    if (attacks_bb<ROOK>(ksq, occupied) & pieces(~us, KING))
        return false;

    // If the moving piece is a king, check whether the destination
    // square is not under new attack after the move.
    if (type_of(piece_on(from)) == KING)
        return !(checkers_to(~us, to, occupied) & ~b);

    // A non-king move is chase legal if the king is not under new attack after the move.
    return !((checkers_to(~us, ksq, occupied) & ~square_bb(to)) & ~b);
}


/// Position::chased() calculate the chase information for a given color.

ChaseMap Position::chased(Color c) {

    ChaseMap chase;
    if (st->move == MOVE_NONE)
        return chase;

    // Checkers bitboard for both side
    Bitboard checkUs = st->checkersBB;
    Bitboard checkThem = checkers_to(sideToMove, square<KING>(~sideToMove));
    if (c != sideToMove)
        std::swap(checkUs, checkThem);

    std::swap(c, sideToMove);

    // King and pawn can legally perpetual chase
    // TODO: 棋规可能可以不将暗子作为attackers，attackers &= ~darkPieces;
    Bitboard attackers = pieces(sideToMove) & ~pieces(sideToMove, KING, PAWN);
    while (attackers)
    {
        Square from = pop_lsb(attackers);
        PieceType attackerType = type_of(piece_on(from));
        // TODO: 暗士
        Bitboard attacks = attacks_bb(attackerType, from, pieces()) & pieces(~sideToMove);

        // Exclude attacks on unpromoted pawns and checks
        attacks &= ~(pieces(~sideToMove, KING, PAWN) ^ (pieces(~sideToMove, PAWN) & HalfBB[sideToMove]));

        // Attacks against stronger pieces
        Bitboard candidates = 0;
        if (attackerType == KNIGHT || attackerType == CANNON)
            candidates = attacks & pieces(~sideToMove, ROOK);
        if (attackerType == BISHOP || attackerType == ADVISOR || attackerType == ADVISOR_B)
            candidates = attacks & pieces(~sideToMove, ROOK, CANNON, KNIGHT);
        attacks ^= candidates;
        while (candidates)
        {
            Square to = pop_lsb(candidates);
            if (chase_legal(make_move(from, to), checkUs))
                chase |= make_chase(idBoard[to], idBoard[from]);
        }

        // Attacks against potentially unprotected pieces
        while (attacks)
        {
            Square to = pop_lsb(attacks);
            Move m = make_move(from, to);

            if (chase_legal(m, checkUs))
            {
                bool trueChase = true;
                const auto& [captured, id] = light_do_move(m);
                Bitboard recaptures = attackers_to(to) & pieces(sideToMove);
                while (recaptures)
                {
                    Square s = pop_lsb(recaptures);
                    if (chase_legal(make_move(s, to), checkThem)) {
                        trueChase = false;
                        break;
                    }
                }
                light_undo_move(m, captured, id);

                if (trueChase)
                {
                    // Exclude mutual/symmetric attacks except pins
                    if (attackerType == type_of(piece_on(to)))
                    {
                        sideToMove = ~sideToMove;
                        if (   (attackerType == KNIGHT && !(attacks_bb<KNIGHT>(to, pieces()) & from))
                            || !chase_legal(make_move(to, from), checkThem))
                            chase |= make_chase(idBoard[to], idBoard[from]);
                        sideToMove = ~sideToMove;
                    }
                    else
                        chase |= make_chase(idBoard[to], idBoard[from]);
                }
            }
        }
    }

    std::swap(c, sideToMove);

    return chase;
}


/// Position::is_repeated() tests whether the position may end the game by draw repetition, perpetual
/// check repetition or perpetual chase repetition that allows a player to claim a game result.

bool Position::is_repeated(Value& result, int ply) const {

    if (st->pliesFromNull < 4 || !filter[st->key])
        return false;

    StateInfo* stp = st->previous->previous;
    bool perpetualThem = st->checkersBB && stp->checkersBB;
    bool perpetualUs = st->previous->checkersBB && stp->previous->checkersBB;

    for (int i = 4; i <= st->pliesFromNull; i += 2)
    {
        stp = stp->previous->previous;
        perpetualThem &= bool(stp->checkersBB);

        // Return a score if a position repeats once earlier.
        if (stp->key == st->key)
        {
            if (perpetualThem || perpetualUs)
            {
                result = !perpetualUs ? mate_in(ply) : !perpetualThem ? mated_in(ply) : VALUE_DRAW;
                return true;
            }

            // Copy the current position to a rollback struct, so we don't need to do those moves again
            Position rollback;
            memcpy((void *)&rollback, (const void *)this, offsetof(Position, filter));

            // Set up chase information: chase[k] belongs to the state k plies back.
            // Kept local so that the shared pre-root states are never written.
            std::vector<uint16_t> chase(i);
            rollback.set_chase_info(i, chase.data());

            // Chasing detection
            stp = st->previous->previous;
            uint16_t chaseThem = chase[0] & chase[2];
            uint16_t chaseUs = chase[1] & chase[3];

            for (int j = 4; j <= i; j += 2)
            {
                // Chase stops after i moves
                if (j != i)
                    chaseThem &= chase[j];
                stp = stp->previous->previous;

                // Return a score if a position repeats once earlier.
                if (stp->key == st->key)
                {
                    result = (chaseThem || chaseUs) ? (!chaseUs ? mate_in(ply) : !chaseThem ? mated_in(ply) : VALUE_DRAW) : VALUE_DRAW;
                    return true;
                }

                if (j + 1 <= i)
                    chaseUs &= chase[j + 1];
            }
        }

        if (i + 1 <= st->pliesFromNull)
            perpetualUs &= bool(stp->previous->checkersBB);
    }

    return false;
}


/// Position::flip() flips position with the white and black sides reversed. This
/// is only useful for debugging e.g. for finding evaluation symmetry bugs.

void Position::flip() {

  string f, token;
  std::stringstream ss(fen());

  for (Rank r = RANK_9; r >= RANK_0; --r) // Piece placement
  {
      std::getline(ss, token, r > RANK_0 ? '/' : ' ');
      f.insert(0, token + (f.empty() ? " " : "/"));
  }

  ss >> token; // Active color
  f += (token == "w" ? "B " : "W "); // Will be lowercased later

  ss >> token;
  f += token + " ";

  std::transform(f.begin(), f.end(), f.begin(),
                 [](char c) { return char(islower(c) ? toupper(c) : tolower(c)); });

  ss >> token;
  f += token;

  std::getline(ss, token); // Half and full moves
  f += token;

  set(f, st, this_thread());

  assert(pos_is_ok());
}


/// Position::pos_is_ok() performs some consistency checks for the
/// position object and raises an asserts if something wrong is detected.
/// This is meant to be helpful when debugging.

bool Position::pos_is_ok() const {

  constexpr bool Fast = true; // Quick (default) or full check?

  if (   (sideToMove != WHITE && sideToMove != BLACK)
      || piece_on(square<KING>(WHITE)) != W_KING
      || piece_on(square<KING>(BLACK)) != B_KING)
      assert(0 && "pos_is_ok: Default");

  if (Fast)
      return true;

  if (   pieceCount[W_KING] != 1
      || pieceCount[B_KING] != 1
      || checkers_to(sideToMove, square<KING>(~sideToMove)))
      assert(0 && "pos_is_ok: Kings");

  if (   (pieces(WHITE, PAWN) & ~PawnBB[WHITE])
      || (pieces(BLACK, PAWN) & ~PawnBB[BLACK])
      || pieceCount[W_PAWN] > 5
      || pieceCount[B_PAWN] > 5)
      assert(0 && "pos_is_ok: Pawns");

  if (   (pieces(WHITE) & pieces(BLACK))
      || (pieces(WHITE) | pieces(BLACK)) != pieces()
      || popcount(pieces(WHITE)) > 16
      || popcount(pieces(BLACK)) > 16)
      assert(0 && "pos_is_ok: Bitboards");

  for (PieceType p1 = PAWN; p1 <= KING; ++p1)
      for (PieceType p2 = PAWN; p2 <= KING; ++p2)
          if (p1 != p2 && (pieces(p1) & pieces(p2)))
              assert(0 && "pos_is_ok: Bitboards");

  StateInfo si = *st;

  set_state(&si);
  if (std::memcmp(&si, st, sizeof(StateInfo)))
      assert(0 && "pos_is_ok: State");

  for (Piece pc : Pieces)
      if (   pieceCount[pc] != popcount(pieces(color_of(pc), type_of(pc)))
          || pieceCount[pc] != std::count(board, board + SQUARE_NB, pc))
          assert(0 && "pos_is_ok: Pieces");

  return true;
}

} // namespace Jieqi
