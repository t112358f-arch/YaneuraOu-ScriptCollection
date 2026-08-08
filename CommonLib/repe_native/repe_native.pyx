# distutils: language = c++
# cython: language_level=3, boundscheck=False, wraparound=False, cdivision=True
"""REPE (Rank Encoding Position Eval) の C++ 実装への Cython バインディング。

`CommonLib/RepeFormatLib.py` はこのモジュールを `import repe_native` できる
場合、内部の重い組合せ計算 (`encode_position` / `decode_position`) をここへ
委譲して高速化する。import に失敗した場合 (未ビルド環境) は純 Python 実装へ
自動でフォールバックするので、本モジュールは必須ではない
(`CommonLib/repe_native/README.md` にビルド手順を記載)。

このファイル自体は「cshogi.Board から取り出した生データ ⇄ C++ 側の構造体」の
薄い変換層のみを担い、組合せ数学のロジックは全て `repe_core.hpp` (C++) 側に
ある。
"""

from libc.stdint cimport uint8_t, int16_t

cdef extern from "repe_core.hpp" namespace "repe":
    const int kRecordBytes
    const int kNSquares

    cdef struct EncodeInput:
        const int* pieces
        const int* hand_black
        const int* hand_white
        int black_king_sq
        int white_king_sq
        int turn
        int game_result_stm
        int eval_stm

    cdef struct DecodedPosition:
        int pieces[81]
        int hand_black[7]
        int hand_white[7]
        int black_king_sq
        int white_king_sq
        int game_result_stm
        int eval_stm

    void encode(const EncodeInput& in_, uint8_t* out) except +
    void decode(const uint8_t* data, DecodedPosition* out) except +


REPE_SIZE = kRecordBytes


def encode_position(list pieces, list hand_black, list hand_white,
                     int black_king_sq, int white_king_sq, int turn,
                     int game_result_stm, int eval_stm):
    """cshogi.Board から取り出した生データをREPEの32byteへエンコードする。

    :param pieces: `board.pieces` (長さ81、cshogi生駒コード)。
    :param hand_black: `board.pieces_in_hand[cshogi.BLACK]` (長さ7)。
    :param hand_white: `board.pieces_in_hand[cshogi.WHITE]` (長さ7)。
    :param black_king_sq: `board.king_square(cshogi.BLACK)`。
    :param white_king_sq: `board.king_square(cshogi.WHITE)`。
    :param turn: `board.turn` (0=Black, 1=White)。
    :param game_result_stm: 手番側から見た勝敗 (1/0/-1)。
    :param eval_stm: 手番側から見た評価値 (int16範囲)。
    :return: 32byteの `bytes`。
    """
    if len(pieces) != 81:
        raise ValueError("pieces must have length 81")
    if len(hand_black) != 7 or len(hand_white) != 7:
        raise ValueError("hand_black/hand_white must have length 7")

    cdef int c_pieces[81]
    cdef int c_hand_black[7]
    cdef int c_hand_white[7]
    cdef int i
    for i in range(81):
        c_pieces[i] = pieces[i]
    for i in range(7):
        c_hand_black[i] = hand_black[i]
        c_hand_white[i] = hand_white[i]

    cdef EncodeInput enc_in
    enc_in.pieces = c_pieces
    enc_in.hand_black = c_hand_black
    enc_in.hand_white = c_hand_white
    enc_in.black_king_sq = black_king_sq
    enc_in.white_king_sq = white_king_sq
    enc_in.turn = turn
    enc_in.game_result_stm = game_result_stm
    enc_in.eval_stm = eval_stm

    cdef uint8_t out[32]
    encode(enc_in, out)
    return bytes(out[:32])


def decode_position(bytes data):
    """REPEの32byteをデコードする。

    :return: `(pieces, hand_black, hand_white, black_king_sq, white_king_sq,
        game_result_stm, eval_stm)`。`pieces` / `hand_black` / `hand_white` は
        常に「手番側=BLACK, 相手側=WHITE」に正規化された表現
        (`board.set_pieces(pieces, (hand_black, hand_white))` にそのまま渡せる)。
    """
    if len(data) != 32:
        raise ValueError(f"REPE record must be 32 bytes, got {len(data)}")

    cdef const uint8_t[:] view = data
    cdef DecodedPosition result
    decode(&view[0], &result)

    pieces = [result.pieces[i] for i in range(81)]
    hand_black = [result.hand_black[i] for i in range(7)]
    hand_white = [result.hand_white[i] for i in range(7)]
    return (
        pieces,
        hand_black,
        hand_white,
        result.black_king_sq,
        result.white_king_sq,
        result.game_result_stm,
        result.eval_stm,
    )
