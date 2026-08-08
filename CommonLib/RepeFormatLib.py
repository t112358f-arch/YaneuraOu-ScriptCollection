"""REPE (Rank Encoding Position Eval) 形式のエンコード・デコード専用ライブラリ。

REPE は `Teacher_Data_Format_V1_Specification` を元にした、将棋局面を
1レコード256bit(32Byte)の固定長で保存する教師データ形式です。

pack / hcpe / hcpe3 / psv とは異なり、REPE は cshogi 側に構造体やdtypeの
定義を持たない全くの独自フォーマットです。局面を組合せ順位(rank)へ写像する
処理は cshogi の薄いラッパーで済むものではないため、他の形式と混在させずに
このファイルへ REPE 固有の処理をすべてまとめています。
`TeacherFormatLib.py` / `TeacherConvertLib.py` 側は本ファイルを呼び出すだけで、
REPEのビット配置やrank計算の詳細を知らなくて済むようにしています。

レコード構成(spec通り、MSB側から):

    Offset(bit)   Size(bit)   内容
    0             128         Part1: 玉位置 + 盤上駒/持ち駒の順位符号化
    128           76          Part2: 駒種類(38個)の多重集合順位符号化
    204           34          Part3: 成り情報(歩18,香4,桂4,銀4,角2,飛2)
    238           2           Part4: 勝敗 (00=Win, 01=Draw, 10=Lose, 11=Reserved)
    240           16          Part5: 評価値 (int16)
    合計 256bit(32Byte)

256bitはPart1を最上位ビット側、評価値を最下位ビット側とするビッグエンディアンの
1個の巨大整数として扱い、それを32byteへ変換して保存します。

勝敗・評価値は「手番側から見た」値です(PSVの `game_result` / `score` と同じ規約)。
盤面は必ず手番側から見た視点へ正規化して保存するため、手番情報そのものは
保存しません(仕様の1.1節の通り)。そのためREPEをデコードすると、常に
「手番側が先手(BLACK)であるかのように正規化された局面」を持つ
`cshogi.Board` (turn=BLACK) が得られます。これは元の局面の先後を捨てて
手番側視点へ正規化するというREPE自体の仕様であり、情報の欠落ではありません。

REPEが持つのは「局面・勝敗・評価値」のみです。指し手や手数など、他の教師
フォーマットが持つその他の情報は本ライブラリの変換対象外とし、
呼び出し側(`TeacherConvertLib.py`)で0埋めしてください。

盤面から128bit/76bit/34bitの各rankを計算する際に必要な組合せ順位符号化・
多重集合順列順位符号化は、spec中の数式をそのまま実装しています
(歩18,香4,桂4,銀4,金4,角2,飛2 = 合計38個、盤上マスは玉2枚を除いた79マス)。

以下はspec中に明記されていない、実装上決めた規約です(仕様自体は
どの具体的なビット割り当て順にするかまでは指定していないため、
エンコード・デコードで一貫していれば問題ありません)。

- 79マスの走査順序は cshogi の盤面インデックス(0～80, `file*9+rank`)の
  昇順とする。手番側がWHITEの場合は「180°回転」を `sq -> 80-sq` の座標
  変換として扱う(色の反転は「自分/相手」で直接判定するため、明示的な
  色スワップ処理は不要)。
- Part1のside bit列は、79マス中の盤上駒がある位置を昇順に見て、
  i番目(0-indexed)の駒のside bitをその整数の下からi bit目に格納する。
- Part2の駒種類列(長さ38)は「盤上駒(手番側視点で昇順のマス順)→
  手番側の持ち駒(駒種類昇順)→非手番側の持ち駒(駒種類昇順)」の順に
  並べた列とする。
- Part3の成り情報は、歩→香→桂→銀→角→飛(金は成れないため対象外)の順に
  ブロックを並べ、各ブロック内はPart2の駒種類列を先頭から見て、その
  駒種類が現れるたびに1bitずつ消費する(持ち駒として現れた場合は
  常に0を格納する。持ち駒は成れないため)。

高速化について:
    `encode_position` / `decode_position` の内部で行っている組合せ順位の
    計算(comb_rank/unrank・多重集合順列のrank/unrank)は純Pythonのループで
    書くと1レコードあたりの計算コストが無視できないため、cshogi 本体と
    同様に C++ コア + Cython バインディングによる高速版を `repe_native/`
    以下に用意しています。`repe_native/` を `python setup.py build_ext
    --inplace` でビルドし、import path 上に置いておくと、本モジュールの
    `encode_position` / `decode_position` は自動的にそちらへ処理を委譲します
    (未ビルドなら透過的にこのファイルの純Python実装にフォールバックするので、
    ビルドは必須ではありません)。native拡張の有無は `NATIVE_AVAILABLE` で
    確認できます。native側の出力は純Python実装と全件(57305局面)で
    バイト完全一致することを検証済みです。詳細は `repe_native/README.md`
    を参照してください。
"""

from __future__ import annotations

import glob
import math
import os
import sys
from functools import lru_cache

import cshogi

__all__ = [
    "REPE_SIZE",
    "NATIVE_AVAILABLE",
    "encode_position",
    "decode_position",
]

# C++/Cython で実装した高速版 (`repe_native/`, cshogi と同様の構成: C++コア +
# Cythonバインディング) が `build_ext --inplace` 済みで import path 上にあれば
# それを使い、無ければ本ファイルの純Python実装にフォールバックする。
# ビルド手順は `repe_native/README.md` を参照。速度差はおおよそ1桁
# (純Python: 数千 records/sec、native: 数万〜十数万 records/sec) で、
# どちらを使っても入出力は完全に同一 (native側はPython実装と全件突合済み)。
_REPE_NATIVE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "repe_native")


def _try_import_native():
    """ビルド済みの `repe_native` 拡張があればimportして返す。無ければ `None`。

    注意: `repe_native/` ディレクトリ自体は (ビルド前・拡張未生成でも)
    `CommonLib/` 配下に常に存在する (`.pyx` / `.hpp` / `setup.py` など)。
    呼び出し元 (`TeacherConvertLib.py` 等) は `CommonLib` 自体を `sys.path`
    に追加するため、素朴に `import repe_native` すると `__init__.py` を
    持たない `repe_native/` ディレクトリが PEP 420 namespace package として
    解決されてしまい、「未ビルドなのに import 自体は成功するが
    `encode_position` 属性が無い」という壊れ方をする。それを避けるため、
    (1) コンパイル済み拡張子(`.so`/`.pyd`/`.dylib`)の実体が存在することを
    先に確認してから import を試み、(2) importできても念のため属性の存在を
    確認する、という二重のガードを入れている。
    """
    if not os.path.isdir(_REPE_NATIVE_DIR):
        return None

    has_binary = any(
        glob.glob(os.path.join(_REPE_NATIVE_DIR, f"repe_native*{suffix}"))
        for suffix in (".so", ".pyd", ".dylib")
    )
    if not has_binary:
        return None

    if _REPE_NATIVE_DIR not in sys.path:
        sys.path.insert(0, _REPE_NATIVE_DIR)

    try:
        import repe_native as native_module
    except ImportError:
        return None

    if not hasattr(native_module, "encode_position") or not hasattr(native_module, "decode_position"):
        # namespace package 化などで想定外のものが解決された場合の防御。
        return None

    return native_module


_native = _try_import_native()
NATIVE_AVAILABLE = _native is not None

BLACK = cshogi.BLACK
WHITE = cshogi.WHITE

# 盤面の全マス数、玉2枚を除いたマス数
_N_SQUARES = 81
_N_SQUARES_NO_KING = 79

# 駒番号(spec 3節): 0=歩 1=香 2=桂 3=銀 4=金 5=角 6=飛
# 各駒番号ごとの総数(歩18,香4,桂4,銀4,金4,角2,飛2)。合計38。
_TYPE_COUNTS = (18, 4, 4, 4, 4, 2, 2)
_N_PIECES = sum(_TYPE_COUNTS)
if _N_PIECES != 38:
    raise RuntimeError(f"REPE piece total must be 38: {_N_PIECES}")

# 成り情報の対象駒(金・玉を除く)。spec 4節: 歩18,香4,桂4,銀4,角2,飛2 = 34。
_PROMOTABLE_TYPES = (0, 1, 2, 3, 5, 6)
_PROMOTABLE_COUNTS = (18, 4, 4, 4, 2, 2)
_N_PROMOTABLE = sum(_PROMOTABLE_COUNTS)
if _N_PROMOTABLE != 34:
    raise RuntimeError(f"REPE promotable piece total must be 34: {_N_PROMOTABLE}")

# cshogiの駒定数(1~14が先手駒、+16すると後手駒)から
# (駒番号0~6, 成りフラグ) へのマップ。玉(8)は別扱いのため含まない。
_RAW_TO_TYPE: dict[int, tuple[int, bool]] = {
    1: (0, False), 9: (0, True),  # 歩 / と金
    2: (1, False), 10: (1, True),  # 香 / 成香
    3: (2, False), 11: (2, True),  # 桂 / 成桂
    4: (3, False), 12: (3, True),  # 銀 / 成銀
    7: (4, False),  # 金(成りなし)
    5: (5, False), 13: (5, True),  # 角 / 馬
    6: (6, False), 14: (6, True),  # 飛 / 龍
}
_TYPE_TO_RAW: dict[tuple[int, bool], int] = {
    key: raw for raw, key in _RAW_TO_TYPE.items()
}

# Part3の各駒種類ブロックの開始bit位置
_PROMOTABLE_BIT_OFFSET: dict[int, int] = {}
_offset = 0
for _ti, _cnt in zip(_PROMOTABLE_TYPES, _PROMOTABLE_COUNTS):
    _PROMOTABLE_BIT_OFFSET[_ti] = _offset
    _offset += _cnt

_FACT = [math.factorial(i) for i in range(_N_PIECES + 1)]

REPE_SIZE = 32  # 256bit = 32Byte

_PART1_BITS = 128
_PART2_BITS = 76
_PART3_BITS = 34
_PART2_MASK = (1 << _PART2_BITS) - 1
_PART3_MASK = (1 << _PART3_BITS) - 1
_EVAL_MASK = 0xFFFF


# ============================================================
#   多重集合順列の順位符号化 (Part2用)
# ============================================================

@lru_cache(maxsize=1 << 20)
def _multinomial(n: int, counts: tuple[int, ...]) -> int:
    denom = 1
    for c in counts:
        denom *= _FACT[c]
    return _FACT[n] // denom


def _multiset_perm_rank(seq: list[int], counts: tuple[int, ...]) -> int:
    """`counts` で指定した多重集合の要素列 `seq` の辞書式順位を返す。"""
    remaining_counts = list(counts)
    remaining = sum(remaining_counts)
    rank = 0
    for sym in seq:
        for s in range(sym):
            if remaining_counts[s] > 0:
                remaining_counts[s] -= 1
                rank += _multinomial(remaining - 1, tuple(remaining_counts))
                remaining_counts[s] += 1
        remaining_counts[sym] -= 1
        remaining -= 1
    return rank


def _multiset_perm_unrank(rank: int, counts: tuple[int, ...], length: int) -> list[int]:
    """`_multiset_perm_rank` の逆変換。"""
    remaining_counts = list(counts)
    remaining = sum(remaining_counts)
    seq: list[int] = []
    for _ in range(length):
        for s in range(len(remaining_counts)):
            if remaining_counts[s] == 0:
                continue
            remaining_counts[s] -= 1
            cnt = _multinomial(remaining - 1, tuple(remaining_counts))
            if rank < cnt:
                seq.append(s)
                remaining -= 1
                break
            rank -= cnt
            remaining_counts[s] += 1
    return seq


# ============================================================
#   組合せの順位符号化 (Part1の駒配置用、combinatorial number system)
# ============================================================

def _comb_rank(positions: list[int]) -> int:
    """昇順に並んだ`positions`(0-indexed)が表す組合せの順位を返す。"""
    rank = 0
    for i, p in enumerate(positions):
        rank += math.comb(p, i + 1)
    return rank


def _comb_unrank(rank: int, n: int, k: int) -> list[int]:
    """`_comb_rank` の逆変換。n個の要素からk個選ぶ組合せを昇順リストで返す。"""
    result = [0] * k
    upper = n - 1
    rem = rank
    for i in range(k, 0, -1):
        lo, hi = i - 1, upper
        best = lo
        while lo <= hi:
            mid = (lo + hi) // 2
            if math.comb(mid, i) <= rem:
                best = mid
                lo = mid + 1
            else:
                hi = mid - 1
        result[i - 1] = best
        rem -= math.comb(best, i)
        upper = best - 1
    return result


# ============================================================
#   局面 <-> REPE
# ============================================================

def _encode_position_python(board: "cshogi.Board", game_result_stm: int, eval_stm: int) -> bytes:
    """cshogi.Boardと手番側から見た勝敗・評価値をREPEの32byteへ変換する。

    :param board: 変換したい局面。`board.turn` が手番側として使われる。
    :param game_result_stm: 手番側から見た勝敗。1=win, 0=draw, -1=lose
        (PSVの `game_result` と同じ規約)。
    :param eval_stm: 手番側から見た評価値 (int16の範囲)。
    :return: 32byteのREPEレコード。
    """
    self_color = board.turn
    enemy_color = 1 - self_color

    def canon_sq(sq: int) -> int:
        # 手番側がWHITEなら180°回転(sq -> 80-sq)。BLACKならそのまま。
        return sq if self_color == BLACK else (_N_SQUARES - 1 - sq)

    self_king_sq = canon_sq(board.king_square(self_color))
    enemy_king_sq = canon_sq(board.king_square(enemy_color))

    # 玉2枚を除いた79マスを、正規化後のマス番号の昇順で列挙する。
    available = [
        sq for sq in range(_N_SQUARES)
        if sq != self_king_sq and sq != enemy_king_sq
    ]

    pieces = board.pieces
    occupied_compact: list[int] = []
    side_bits_list: list[int] = []
    seq_types: list[int] = []
    seq_board_promoted: list[bool] = []

    for compact_i, canon in enumerate(available):
        # canon_sq は対合(involution)なので、逆変換も同じ式でよい。
        orig_sq = canon if self_color == BLACK else (_N_SQUARES - 1 - canon)
        piece = pieces[orig_sq]
        if piece == cshogi.NONE:
            continue

        piece_color = BLACK if piece <= 14 else WHITE
        raw = piece if piece <= 14 else piece - 16
        is_self = piece_color == self_color
        type_idx, promoted = _RAW_TO_TYPE[raw]

        occupied_compact.append(compact_i)
        side_bits_list.append(0 if is_self else 1)
        seq_types.append(type_idx)
        seq_board_promoted.append(promoted)

    s = len(occupied_compact)

    hand = board.pieces_in_hand
    self_hand = list(hand[self_color])
    enemy_hand = list(hand[enemy_color])
    hand_self_total = sum(self_hand)

    # --- Part1 ---
    comb_r = _comb_rank(occupied_compact)
    side_bits = 0
    for i, bit in enumerate(side_bits_list):
        side_bits |= bit << i

    offset = 0
    for k in range(s):
        offset += math.comb(_N_SQUARES_NO_KING, k) * (2 ** k) * (_N_PIECES + 1 - k)

    rank_for_s = ((comb_r << s) + side_bits) * (_N_PIECES + 1 - s) + hand_self_total
    rank1 = offset + rank_for_s
    part1 = (rank1 * 81 + self_king_sq) * 81 + enemy_king_sq

    if part1 >= (1 << _PART1_BITS):
        raise ValueError("REPE Part1 rank overflowed 128bit (invalid position?)")

    # --- Part2 ---
    full_seq = list(seq_types)
    for type_idx in range(7):
        full_seq.extend([type_idx] * self_hand[type_idx])
    for type_idx in range(7):
        full_seq.extend([type_idx] * enemy_hand[type_idx])

    if len(full_seq) != _N_PIECES:
        raise ValueError(
            f"REPE requires exactly {_N_PIECES} non-king pieces on board+hand, "
            f"got {len(full_seq)} (invalid position?)"
        )

    part2 = _multiset_perm_rank(full_seq, _TYPE_COUNTS)

    # --- Part3 ---
    is_board = [True] * s + [False] * (_N_PIECES - s)
    next_bit_of_type = dict.fromkeys(_PROMOTABLE_TYPES, 0)
    part3 = 0
    for idx, type_idx in enumerate(full_seq):
        base = _PROMOTABLE_BIT_OFFSET.get(type_idx)
        if base is None:
            continue
        bit = 1 if (is_board[idx] and seq_board_promoted[idx]) else 0
        part3 |= bit << (base + next_bit_of_type[type_idx])
        next_bit_of_type[type_idx] += 1

    # --- Part4 ---
    if game_result_stm == 1:
        result_bits = 0b00
    elif game_result_stm == -1:
        result_bits = 0b10
    elif game_result_stm == 0:
        result_bits = 0b01
    else:
        raise ValueError(f"invalid game_result_stm: {game_result_stm}")

    # --- Part5 ---
    eval_u16 = eval_stm & _EVAL_MASK

    value = (
        (((((part1 << _PART2_BITS) | part2) << _PART3_BITS) | part3) << 2 | result_bits)
        << 16
    ) | eval_u16
    return value.to_bytes(REPE_SIZE, "big")


def _decode_position_python(data: bytes) -> tuple["cshogi.Board", int, int]:
    """REPEの32byteをデコードし、`(board, game_result_stm, eval_stm)` を返す。

    `board` は手番側視点へ正規化された局面で、常に `turn == cshogi.BLACK`
    (=手番側をBLACKとみなす)状態で返る。元の局面の実際の先後は、
    REPEには保存されていないため復元できない(仕様通りの正規化)。
    `game_result_stm` / `eval_stm` は手番側から見た値 (PSVと同じ規約)。
    """
    if len(data) != REPE_SIZE:
        raise ValueError(f"REPE record must be {REPE_SIZE} bytes, got {len(data)}")

    value = int.from_bytes(data, "big")

    eval_u16 = value & _EVAL_MASK
    value >>= 16
    result_bits = value & 0b11
    value >>= 2
    part3 = value & _PART3_MASK
    value >>= _PART3_BITS
    part2 = value & _PART2_MASK
    value >>= _PART2_BITS
    part1 = value

    eval_stm = eval_u16 - 0x10000 if eval_u16 & 0x8000 else eval_u16

    if result_bits == 0b00:
        game_result_stm = 1
    elif result_bits == 0b10:
        game_result_stm = -1
    elif result_bits == 0b01:
        game_result_stm = 0
    else:
        raise ValueError("reserved result bits (0b11) found in REPE record")

    # --- Part1 のデコード: enemyKing -> selfKing -> rank の順 ---
    enemy_king_sq = part1 % 81
    tmp = part1 // 81
    self_king_sq = tmp % 81
    rank1 = tmp // 81

    s = 0
    rem = rank1
    while True:
        state_count = math.comb(_N_SQUARES_NO_KING, s) * (2 ** s) * (_N_PIECES + 1 - s)
        if rem < state_count:
            break
        rem -= state_count
        s += 1
        if s > _N_PIECES:
            raise ValueError("invalid REPE Part1 rank (out of range)")

    mult = _N_PIECES + 1 - s
    hand_self_total = rem % mult
    tmp2 = rem // mult
    side_bits = (tmp2 % (2 ** s)) if s > 0 else 0
    comb_r = (tmp2 >> s) if s > 0 else 0

    occupied_compact = _comb_unrank(comb_r, _N_SQUARES_NO_KING, s)
    available = [
        sq for sq in range(_N_SQUARES)
        if sq != self_king_sq and sq != enemy_king_sq
    ]
    seq_board_sq = [available[c] for c in occupied_compact]
    seq_board_side = [(side_bits >> i) & 1 for i in range(s)]

    hand_enemy_total = (_N_PIECES - s) - hand_self_total

    # --- Part2 のデコード ---
    full_seq = _multiset_perm_unrank(part2, _TYPE_COUNTS, _N_PIECES)

    board_types = full_seq[:s]
    self_hand_seq = full_seq[s: s + hand_self_total]
    enemy_hand_seq = full_seq[s + hand_self_total: s + hand_self_total + hand_enemy_total]

    self_hand = [0] * 7
    for type_idx in self_hand_seq:
        self_hand[type_idx] += 1
    enemy_hand = [0] * 7
    for type_idx in enemy_hand_seq:
        enemy_hand[type_idx] += 1

    # --- Part3 のデコード ---
    is_board = [True] * s + [False] * (_N_PIECES - s)
    next_bit_of_type = dict.fromkeys(_PROMOTABLE_TYPES, 0)
    promoted_board = [False] * s

    for idx, type_idx in enumerate(full_seq):
        base = _PROMOTABLE_BIT_OFFSET.get(type_idx)
        if base is None:
            continue
        bitpos = base + next_bit_of_type[type_idx]
        next_bit_of_type[type_idx] += 1
        bit = (part3 >> bitpos) & 1
        if is_board[idx]:
            promoted_board[idx] = bool(bit)

    # --- 局面の再構築 ---
    pieces = [cshogi.NONE] * _N_SQUARES
    pieces[self_king_sq] = 8  # BKING
    pieces[enemy_king_sq] = 24  # WKING
    for i in range(s):
        sq = seq_board_sq[i]
        type_idx = board_types[i]
        promoted = promoted_board[i]
        raw = _TYPE_TO_RAW[(type_idx, promoted)]
        side = seq_board_side[i]  # 0=self(BLACK), 1=enemy(WHITE)
        pieces[sq] = raw if side == 0 else raw + 16

    board = cshogi.Board()
    board.set_pieces(pieces, (self_hand, enemy_hand))
    return board, game_result_stm, eval_stm


# ============================================================
#   公開API (native拡張があればそちらへ委譲、無ければ純Python実装)
# ============================================================

def encode_position(board: "cshogi.Board", game_result_stm: int, eval_stm: int) -> bytes:
    """cshogi.Boardと手番側から見た勝敗・評価値をREPEの32byteへ変換する。

    :param board: 変換したい局面。`board.turn` が手番側として使われる。
    :param game_result_stm: 手番側から見た勝敗。1=win, 0=draw, -1=lose
        (PSVの `game_result` と同じ規約)。
    :param eval_stm: 手番側から見た評価値 (int16の範囲)。
    :return: 32byteのREPEレコード。

    `repe_native` (C++/Cython実装) がビルド済みならそちらを使い高速化する。
    未ビルドなら本ファイル内の純Python実装 (`_encode_position_python`) を使う。
    どちらの経路でも出力バイト列は完全に同一 (全件突合済み)。
    """
    if _native is not None:
        hand = board.pieces_in_hand
        return _native.encode_position(
            board.pieces,
            list(hand[BLACK]),
            list(hand[WHITE]),
            board.king_square(BLACK),
            board.king_square(WHITE),
            board.turn,
            game_result_stm,
            eval_stm,
        )
    return _encode_position_python(board, game_result_stm, eval_stm)


def decode_position(data: bytes) -> tuple["cshogi.Board", int, int]:
    """REPEの32byteをデコードし、`(board, game_result_stm, eval_stm)` を返す。

    `board` は手番側視点へ正規化された局面で、常に `turn == cshogi.BLACK`
    (=手番側をBLACKとみなす)状態で返る。元の局面の実際の先後は、
    REPEには保存されていないため復元できない(仕様通りの正規化)。
    `game_result_stm` / `eval_stm` は手番側から見た値 (PSVと同じ規約)。

    `repe_native` (C++/Cython実装) がビルド済みならそちらを使い高速化する。
    未ビルドなら本ファイル内の純Python実装 (`_decode_position_python`) を使う。
    どちらの経路でも出力 (盤面・持ち駒・勝敗・評価値) は完全に同一
    (全件突合済み)。
    """
    if _native is not None:
        pieces, hand_black, hand_white, _black_king, _white_king, game_result_stm, eval_stm = (
            _native.decode_position(data)
        )
        board = cshogi.Board()
        board.set_pieces(pieces, (hand_black, hand_white))
        return board, game_result_stm, eval_stm
    return _decode_position_python(data)
