#include "position.h"

#include "movegen.h"

#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>

namespace Zobrist {
uint64_t psq[PIECE_NB][64];
uint64_t side;
uint64_t castling[16];
uint64_t epFile[8];

static uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void init() {
    uint64_t seed = 0x4C697869616E2026ULL;  // "Lixian &"
    for (int p = 0; p < PIECE_NB; ++p)
        for (int s = 0; s < 64; ++s) psq[p][s] = splitmix64(seed);
    side = splitmix64(seed);
    for (int i = 0; i < 16; ++i) castling[i] = splitmix64(seed);
    for (int i = 0; i < 8; ++i) epFile[i] = splitmix64(seed);
}
}  // namespace Zobrist

namespace {

const Square E1 = 4, A1 = 0, H1 = 7;
const Square E8 = 60, A8 = 56, H8 = 63;

const int CASTLING_MASK[64] = {
    0xD, 0xF, 0xF, 0xF, 0xC, 0xF, 0xF, 0xE,
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0x7, 0xF, 0xF, 0xF, 0x3, 0xF, 0xF, 0xB
};

const int SEE_VALUE[PT_NB] = {100, 320, 330, 500, 900, 20000};

inline int promoValue(PieceType pt) {
    switch (pt) {
        case KNIGHT: return 320;
        case BISHOP: return 330;
        case ROOK: return 500;
        case QUEEN: return 900;
        default: return 0;
    }
}

}  // namespace

std::string squareToString(Square s) {
    if (!isValidSquare(s)) return "-";
    std::string out;
    out += char('a' + fileOf(s));
    out += char('1' + rankOf(s));
    return out;
}

Square parseSquare(const std::string& s) {
    if (s.size() < 2) return NO_SQ;
    const int f = std::tolower((unsigned char)s[0]) - 'a';
    const int r = s[1] - '1';
    if (f < 0 || f > 7 || r < 0 || r > 7) return NO_SQ;
    return makeSquare(f, r);
}

std::string moveToString(Move m) {
    std::string out = squareToString(fromSq(m)) + squareToString(toSq(m));
    if (isPromotion(m)) {
        const PieceType pt = promotionType(m);
        const char c = (pt == KNIGHT ? 'n' : pt == BISHOP ? 'b' : pt == ROOK ? 'r' : 'q');
        out += c;
    }
    return out;
}

Position::Position() { startpos(); }

void Position::clear() {
    for (int i = 0; i < PT_NB; ++i) byType[i] = 0;
    for (int i = 0; i < COLOR_NB; ++i) byColor[i] = 0;
    occupied = 0;
    for (int s = 0; s < 64; ++s) board[s] = NO_PIECE;
    side = WHITE;
    castling = 0;
    ep = NO_SQ;
    halfmove = 0;
    fullmove = 1;
    key = 0;
    kingSq[WHITE] = kingSq[BLACK] = NO_SQ;
    repKeys.clear();
}

void Position::startpos() {
    setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

uint64_t Position::computeKey() const {
    uint64_t k = 0;
    for (int s = 0; s < 64; ++s)
        if (board[s] != NO_PIECE) k ^= Zobrist::psq[board[s]][s];
    if (side == BLACK) k ^= Zobrist::side;
    k ^= Zobrist::castling[castling];
    if (ep != NO_SQ) k ^= Zobrist::epFile[fileOf(ep)];
    return k;
}

static PieceType pieceTypeFromChar(char c) {
    switch (std::tolower((unsigned char)c)) {
        case 'p': return PAWN;
        case 'n': return KNIGHT;
        case 'b': return BISHOP;
        case 'r': return ROOK;
        case 'q': return QUEEN;
        case 'k': return KING;
        default: return NO_PT;
    }
}

void Position::setFen(const std::string& fenText) {
    clear();
    std::istringstream ss(fenText);
    std::string boardPart, sidePart, castlePart, epPart;
    ss >> boardPart >> sidePart >> castlePart >> epPart;
    if (!ss) {
        // Fallback to a legal starting position on malformed input.
        if (fenText != "startpos" && !fenText.empty()) {
            setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        }
        return;
    }

    int rank = 7, file = 0;
    for (char c : boardPart) {
        if (c == '/') {
            --rank;
            file = 0;
        } else if (std::isdigit((unsigned char)c)) {
            file += c - '0';
        } else {
            const PieceType pt = pieceTypeFromChar(c);
            if (pt == NO_PT || rank < 0 || file > 7) continue;
            const Color col = std::isupper((unsigned char)c) ? WHITE : BLACK;
            const Square s = makeSquare(file, rank);
            addPiece(s, makePiece(col, pt));
            ++file;
        }
    }

    side = (sidePart.size() && (sidePart[0] == 'b' || sidePart[0] == 'B')) ? BLACK : WHITE;

    for (char c : castlePart) {
        switch (c) {
            case 'K': castling |= 1; break;
            case 'Q': castling |= 2; break;
            case 'k': castling |= 4; break;
            case 'q': castling |= 8; break;
            default: break;
        }
    }

    if (epPart != "-" && epPart.size() >= 2) ep = parseSquare(epPart);
    if (ep != NO_SQ) {
        const Color us = Color(side);
        const Color them = Color(us ^ 1);
        if (!(PawnAttacks[them][ep] & pieces(us, PAWN))) ep = NO_SQ;
    }

    if (!(ss >> halfmove)) halfmove = 0;
    if (!(ss >> fullmove)) fullmove = 1;
    if (fullmove < 1) fullmove = 1;
    if (halfmove < 0) halfmove = 0;

    key = computeKey();
    repKeys.clear();
    repKeys.push_back(key);
}

std::string Position::fen() const {
    std::string out;
    for (int r = 7; r >= 0; --r) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            const Piece p = board[makeSquare(f, r)];
            if (p == NO_PIECE) {
                ++empty;
                continue;
            }
            if (empty) {
                out += char('0' + empty);
                empty = 0;
            }
            const PieceType pt = typeOf(p);
            char c = (pt == PAWN ? 'p' : pt == KNIGHT ? 'n' : pt == BISHOP ? 'b'
                     : pt == ROOK ? 'r' : pt == QUEEN ? 'q' : 'k');
            if (colorOf(p) == WHITE) c = char(std::toupper((unsigned char)c));
            out += c;
        }
        if (empty) out += char('0' + empty);
        if (r) out += '/';
    }
    out += ' ';
    out += (side == WHITE ? 'w' : 'b');
    out += ' ';
    if (!castling) out += '-';
    else {
        if (castling & 1) out += 'K';
        if (castling & 2) out += 'Q';
        if (castling & 4) out += 'k';
        if (castling & 8) out += 'q';
    }
    out += ' ';
    out += (ep == NO_SQ ? "-" : squareToString(ep));
    out += ' ';
    out += std::to_string(halfmove);
    out += ' ';
    out += std::to_string(fullmove);
    return out;
}

bool Position::isValid() const {
    if (kingSq[WHITE] == NO_SQ || kingSq[BLACK] == NO_SQ) return false;
    if (popcount(byType[KING]) != 2) return false;
    if (byType[PAWN] & (BB::RANK_1 | BB::RANK_8)) return false;
    if (side != WHITE && side != BLACK) return false;
    return true;
}

void Position::addPiece(Square s, Piece p) {
    board[s] = p;
    const Bitboard b = squareBB(s);
    byType[typeOf(p)] |= b;
    byColor[colorOf(p)] |= b;
    occupied |= b;
    if (typeOf(p) == KING) kingSq[colorOf(p)] = s;
}

void Position::removePiece(Square s, Piece p) {
    const Bitboard b = squareBB(s);
    byType[typeOf(p)] &= ~b;
    byColor[colorOf(p)] &= ~b;
    occupied &= ~b;
    board[s] = NO_PIECE;
}

void Position::movePiece(Square from, Square to, Piece p) {
    const Bitboard mask = squareBB(from) | squareBB(to);
    byType[typeOf(p)] ^= mask;
    byColor[colorOf(p)] ^= mask;
    occupied ^= mask;
    board[from] = NO_PIECE;
    board[to] = p;
    if (typeOf(p) == KING) kingSq[colorOf(p)] = to;
}

bool Position::isSquareAttacked(Square sq, Color by) const {
    const Color other = Color(by ^ 1);
    if (PawnAttacks[other][sq] & pieces(by, PAWN)) return true;
    if (KnightAttacks[sq] & pieces(by, KNIGHT)) return true;
    if (KingAttacks[sq] & pieces(by, KING)) return true;
    const Bitboard bq = pieces(by, BISHOP) | pieces(by, QUEEN);
    if (bq && (bishopAttacks(sq, occupied) & bq)) return true;
    const Bitboard rq = pieces(by, ROOK) | pieces(by, QUEEN);
    if (rq && (rookAttacks(sq, occupied) & rq)) return true;
    return false;
}

Bitboard Position::attackersTo(Square sq, Bitboard occ) const {
    Bitboard att = 0;
    att |= PawnAttacks[BLACK][sq] & pieces(WHITE, PAWN);
    att |= PawnAttacks[WHITE][sq] & pieces(BLACK, PAWN);
    att |= KnightAttacks[sq] & byType[KNIGHT];
    att |= KingAttacks[sq] & byType[KING];
    att |= bishopAttacks(sq, occ) & (byType[BISHOP] | byType[QUEEN]);
    att |= rookAttacks(sq, occ) & (byType[ROOK] | byType[QUEEN]);
    return att;
}

Bitboard Position::pawnAttacks(Color c) const {
    Bitboard att = 0;
    Bitboard b = pieces(c, PAWN);
    while (b) {
        const Square s = popLsb(b);
        att |= PawnAttacks[c][s];
    }
    return att;
}

Bitboard Position::attackedSquares(Color c) const {
    Bitboard att = pawnAttacks(c);
    Bitboard b = pieces(c, KNIGHT);
    while (b) att |= KnightAttacks[popLsb(b)];
    b = pieces(c, BISHOP);
    while (b) att |= bishopAttacks(popLsb(b), occupied);
    b = pieces(c, ROOK);
    while (b) att |= rookAttacks(popLsb(b), occupied);
    b = pieces(c, QUEEN);
    while (b) att |= queenAttacks(popLsb(b), occupied);
    att |= KingAttacks[kingSq[c]];
    return att;
}

void Position::makeMove(Move m, Undo& u) {
    const Color us = Color(side);
    const Color them = Color(side ^ 1);
    const Square from = fromSq(m);
    const Square to = toSq(m);
    const int flags = moveFlags(m);
    const Piece pc = board[from];
    const PieceType pt = typeOf(pc);
    const bool promotion = flags >= PROMO_N;
    const bool capture = (flags == CAPTURE || flags == EP_CAPTURE || flags >= PROMO_CAP_N);

    u.key = key;
    u.castling = castling;
    u.ep = ep;
    u.halfmove = halfmove;
    u.captured = NO_PIECE;
    u.capturedSq = NO_SQ;
    u.kingSq[0] = kingSq[0];
    u.kingSq[1] = kingSq[1];

    if (ep != NO_SQ) key ^= Zobrist::epFile[fileOf(ep)];

    Square capSq = NO_SQ;
    if (flags == EP_CAPTURE) {
        capSq = Square(us == WHITE ? to - 8 : to + 8);
    } else if (capture) {
        capSq = to;
    }
    if (capSq != NO_SQ) {
        const Piece cap = board[capSq];
        u.captured = cap;
        u.capturedSq = capSq;
        key ^= Zobrist::psq[cap][capSq];
        if (typeOf(cap) == ROOK) castling &= CASTLING_MASK[capSq];
        removePiece(capSq, cap);
    }

    key ^= Zobrist::psq[pc][from];
    removePiece(from, pc);

    if (promotion) {
        const Piece promo = makePiece(us, promotionType(m));
        key ^= Zobrist::psq[promo][to];
        addPiece(to, promo);
    } else {
        key ^= Zobrist::psq[pc][to];
        addPiece(to, pc);
    }

    if (flags == KING_CASTLE) {
        const Square rf = (us == WHITE ? H1 : H8);
        const Square rt = (us == WHITE ? H1 - 2 : H8 - 2);
        const Piece rook = makePiece(us, ROOK);
        key ^= Zobrist::psq[rook][rf] ^ Zobrist::psq[rook][rt];
        movePiece(rf, rt, rook);
    } else if (flags == QUEEN_CASTLE) {
        const Square rf = (us == WHITE ? A1 : A8);
        const Square rt = (us == WHITE ? A1 + 3 : A8 + 3);
        const Piece rook = makePiece(us, ROOK);
        key ^= Zobrist::psq[rook][rf] ^ Zobrist::psq[rook][rt];
        movePiece(rf, rt, rook);
    }

    if (pt == KING) {
        kingSq[us] = to;
        castling &= CASTLING_MASK[from];
    } else if (pt == ROOK) {
        castling &= CASTLING_MASK[from];
    }

    ep = NO_SQ;
    if (flags == DOUBLE_PUSH) {
        const Square candidate = Square((from + to) / 2);
        if (PawnAttacks[us][candidate] & pieces(them, PAWN)) ep = candidate;
    }

    key ^= Zobrist::castling[u.castling] ^ Zobrist::castling[castling];
    if (ep != NO_SQ) key ^= Zobrist::epFile[fileOf(ep)];

    if (pt == PAWN || capture)
        halfmove = 0;
    else
        ++halfmove;
    if (us == BLACK) ++fullmove;

    side = them;
    key ^= Zobrist::side;

    repKeys.push_back(key);
}

void Position::unmakeMove(Move m, const Undo& u) {
    side ^= 1;
    const Color us = Color(side);
    const Square from = fromSq(m);
    const Square to = toSq(m);
    const int flags = moveFlags(m);

    const Piece placed = board[to];
    const PieceType placedType = typeOf(placed);
    removePiece(to, placed);

    const Piece original = makePiece(us, (flags >= PROMO_N) ? PAWN : placedType);
    addPiece(from, original);

    if (flags == KING_CASTLE) {
        const Square rf = (us == WHITE ? H1 : H8);
        const Square rt = (us == WHITE ? H1 - 2 : H8 - 2);
        const Piece rook = makePiece(us, ROOK);
        movePiece(rt, rf, rook);
    } else if (flags == QUEEN_CASTLE) {
        const Square rf = (us == WHITE ? A1 : A8);
        const Square rt = (us == WHITE ? A1 + 3 : A8 + 3);
        const Piece rook = makePiece(us, ROOK);
        movePiece(rt, rf, rook);
    }

    if (u.captured != NO_PIECE) addPiece(u.capturedSq, u.captured);

    key = u.key;
    castling = u.castling;
    ep = u.ep;
    halfmove = u.halfmove;
    kingSq[0] = u.kingSq[0];
    kingSq[1] = u.kingSq[1];
    if (us == BLACK) --fullmove;

    if (!repKeys.empty()) repKeys.pop_back();
}

void Position::makeNullMove(Undo& u) {
    u.key = key;
    u.castling = castling;
    u.ep = ep;
    u.halfmove = halfmove;
    u.captured = NO_PIECE;
    u.capturedSq = NO_SQ;
    u.kingSq[0] = kingSq[0];
    u.kingSq[1] = kingSq[1];

    if (ep != NO_SQ) key ^= Zobrist::epFile[fileOf(ep)];
    ep = NO_SQ;
    halfmove = 0;  // Null-move subtrees must not claim rule-50/repetition draws.
    side ^= 1;
    key ^= Zobrist::side;
}

void Position::unmakeNullMove(const Undo& u) {
    side ^= 1;
    key = u.key;
    castling = u.castling;
    ep = u.ep;
    halfmove = u.halfmove;
}

bool Position::moveIsLegal(Move m) const {
    Position copy = *this;
    const Color us = Color(side);
    Undo u;
    copy.makeMove(m, u);
    return !copy.inCheck(us);
}

Move Position::uciToMove(const std::string& text) const {
    if (text.size() < 4) return MOVE_NONE;
    const Square from = parseSquare(text.substr(0, 2));
    const Square to = parseSquare(text.substr(2, 2));
    if (from == NO_SQ || to == NO_SQ) return MOVE_NONE;
    PieceType promo = NO_PT;
    if (text.size() >= 5) {
        switch (std::tolower((unsigned char)text[4])) {
            case 'n': promo = KNIGHT; break;
            case 'b': promo = BISHOP; break;
            case 'r': promo = ROOK; break;
            case 'q': promo = QUEEN; break;
            default: break;
        }
    }

    MoveList ml;
    generateAll(*this, ml);
    for (int i = 0; i < ml.size; ++i) {
        const Move m = ml.moves[i];
        if (fromSq(m) != from || toSq(m) != to) continue;
        if (promo != NO_PT && (!isPromotion(m) || promotionType(m) != promo)) continue;
        if (!moveIsLegal(m)) continue;
        return m;
    }
    return MOVE_NONE;
}

bool Position::isRepetition() const {
    const int n = std::min<int>(halfmove, int(repKeys.size()) - 1);
    const int current = int(repKeys.size()) - 1;
    for (int i = current - 2; i >= current - n; i -= 2)
        if (repKeys[i] == key) return true;
    return false;
}

bool Position::isDraw() const {
    if (halfmove >= 100) {
        if (!inCheck(Color(side))) return true;
        MoveList ml;
        generateAll(*this, ml);
        for (int i = 0; i < ml.size; ++i)
            if (moveIsLegal(ml.moves[i])) return true;
        return false;
    }
    if (halfmove >= 4 && isRepetition()) return true;
    return isInsufficientMaterial();
}

bool Position::isInsufficientMaterial() const {
    if (byType[PAWN] | byType[ROOK] | byType[QUEEN]) return false;
    const int wMinor = popcount(byColor[WHITE] & (byType[KNIGHT] | byType[BISHOP]));
    const int bMinor = popcount(byColor[BLACK] & (byType[KNIGHT] | byType[BISHOP]));
    if (wMinor <= 1 && bMinor <= 1) return true;
    if (byType[KNIGHT] == 0) {
        const Bitboard bishops = byType[BISHOP];
        if (!(bishops & BB::DARK_SQUARES)) return true;
        if (!(bishops & BB::LIGHT_SQUARES)) return true;
    }
    return false;
}

bool Position::hasNonPawnMaterial(Color c) const {
    return (byColor[c] & (byType[KNIGHT] | byType[BISHOP] | byType[ROOK] | byType[QUEEN])) != 0;
}

int Position::nonPawnMaterial(Color c) const {
    return popcount(byColor[c] & (byType[KNIGHT] | byType[BISHOP] |
                                 byType[ROOK] | byType[QUEEN]));
}

bool Position::seeGE(Move m, int threshold) const {
    if (isCastle(m)) return VALUE_DRAW >= threshold;

    const Square from = fromSq(m);
    const Square to = toSq(m);
    const PieceType moving = typeOf(board[from]);

    int captured = 0;
    if (moveFlags(m) == EP_CAPTURE)
        captured = SEE_VALUE[PAWN];
    else if (isCapture(m))
        captured = SEE_VALUE[typeOf(board[to])];
    if (isPromotion(m)) captured += promoValue(promotionType(m)) - SEE_VALUE[PAWN];

    int swap = captured - threshold;
    if (swap < 0) return false;
    swap = SEE_VALUE[moving] - swap;
    if (swap <= 0) return true;

    Bitboard occ = occupied ^ squareBB(from) ^ squareBB(to);
    if (isCapture(m) && moveFlags(m) == EP_CAPTURE) {
        const Square capSq = Square(side == WHITE ? to - 8 : to + 8);
        occ ^= squareBB(capSq);
    }

    Color stm = Color(side);
    Bitboard attackers = attackersTo(to, occ);
    int res = 1;

    while (true) {
        stm = Color(stm ^ 1);
        attackers &= occ;
        const Bitboard stmAttackers = attackers & byColor[stm];
        if (!stmAttackers) break;

        res ^= 1;
        Bitboard bb;
        if ((bb = stmAttackers & byType[PAWN])) {
            if ((swap = SEE_VALUE[PAWN] - swap) < res) break;
            occ ^= squareBB(lsb(bb));
            attackers |= bishopAttacks(to, occ) & (byType[BISHOP] | byType[QUEEN]);
        } else if ((bb = stmAttackers & byType[KNIGHT])) {
            if ((swap = SEE_VALUE[KNIGHT] - swap) < res) break;
            occ ^= squareBB(lsb(bb));
        } else if ((bb = stmAttackers & byType[BISHOP])) {
            if ((swap = SEE_VALUE[BISHOP] - swap) < res) break;
            occ ^= squareBB(lsb(bb));
            attackers |= bishopAttacks(to, occ) & (byType[BISHOP] | byType[QUEEN]);
        } else if ((bb = stmAttackers & byType[ROOK])) {
            if ((swap = SEE_VALUE[ROOK] - swap) < res) break;
            occ ^= squareBB(lsb(bb));
            attackers |= rookAttacks(to, occ) & (byType[ROOK] | byType[QUEEN]);
        } else if ((bb = stmAttackers & byType[QUEEN])) {
            swap = SEE_VALUE[QUEEN] - swap;
            occ ^= squareBB(lsb(bb));
            attackers |= (bishopAttacks(to, occ) & (byType[BISHOP] | byType[QUEEN])) |
                         (rookAttacks(to, occ) & (byType[ROOK] | byType[QUEEN]));
        } else {
            return (attackers & ~byColor[stm]) ? (res ^ 1) : res;
        }
    }
    return bool(res);
}

void Position::print(std::ostream& os) const {
    os << "\n +---+---+---+---+---+---+---+---+\n";
    for (int r = 7; r >= 0; --r) {
        for (int f = 0; f < 8; ++f) {
            const Piece p = board[makeSquare(f, r)];
            char c = '.';
            if (p != NO_PIECE) {
                const PieceType pt = typeOf(p);
                c = (pt == PAWN ? 'p' : pt == KNIGHT ? 'n' : pt == BISHOP ? 'b'
                     : pt == ROOK ? 'r' : pt == QUEEN ? 'q' : 'k');
                if (colorOf(p) == WHITE) c = char(std::toupper((unsigned char)c));
            }
            os << " | " << c;
        }
        os << " | " << (r + 1) << "\n +---+---+---+---+---+---+---+---+\n";
    }
    os << "   a   b   c   d   e   f   g   h\n";
    os << "Fen: " << fen() << "\nKey: " << std::hex << key << std::dec << "\n";
}
