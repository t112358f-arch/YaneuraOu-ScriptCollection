// repe_core.hpp
//
// REPE (Rank Encoding Position Eval) のエンコード・デコードを行う C++ コア。
// `CommonLib/RepeFormatLib.py` の純 Python 実装と完全に同じアルゴリズム・
// 同じビット配置規約で実装している (cshogi 本体が C++ コア + Cython binding
// という構成なので、それに倣って REPE も同じ構成にしている)。
//
// 依存: GCC / Clang の `unsigned __int128` 拡張 (128bit 整数演算に使用)。
// MSVC は `__int128` を持たないため、MSVC で直接ビルドする場合は Clang-cl か
// WSL/MinGW を使うこと (本プロジェクトは Linux 上での教師データ変換を主用途と
// しているため、通常は問題にならない)。
//
// スレッドセーフ: 本ファイルの関数はすべて純粋関数 (グローバル状態を持たない)
// であり、複数スレッドから並行に呼び出して問題ない。lookup table
// (`tables::part1_tables()` / `tables::part2_tables()`) は関数内 `static`
// による遅延初期化で、C++11 以降の仕様上スレッドセーフに一度だけ構築される
// (Rust版 `shogi-format/src/repe.rs` の `OnceLock` と同じ考え方)。
//
// ## 高速化 (lookup table + incremental multinomial)
//
// `convert_teacher.py` での大量データ変換 (数億レコード) がホットパスのため、
// エンコード・デコードの両方で最も重い「組合せ順位のrank化/逆写像」を、
// レコードに依存しない部分だけ事前計算して高速化している
// (Rust版 tatara `crates/shogi-format/src/repe.rs` と同じアルゴリズム):
//
// - **Part1**: 玉位置を除いた `n=79` マスに対する `C(v,i)` (`v=0..=79`,
//   `i=0..=38`) と、盤上駒数 `s` ごとの累積 offset (`s=0..=38`) は
//   レコードに依らない定数なので、`tables::part1_tables()` で一度だけ
//   事前計算する。decode 側は元々 `s` を線形探索 (`O(s)`、各 step で
//   `C(79,s)` を再計算) していたが、offset table を `std::upper_bound` で
//   二分探索するだけに置き換えている。encode/decode どちらの
//   `comb_rank` / `comb_unrank` も、その場での `binom()` 呼び出しを
//   table lookup に置き換えている。
// - **Part2**: 駒種類列 (長さ38) の先頭3要素は、初期状態
//   (`kTypeCounts` 固定) から辿れる状態数が `7 + 7*7 + 7*7*7 = 399` 通り
//   しかなく、record に依らず事前計算できる (`tables::part2_tables()`)。
// - **4要素目以降 (35個)**: 状態数が指数的に増え table 化は非現実的だが、
//   `multinomial(remaining-1, counts with c_s -= 1) == m * c_s / remaining`
//   という組合せ論の恒等式 (割り切れる) を使い、直前の状態の multinomial
//   値 `m` から乗除算1回で次の候補の閾値を求める。これにより
//   `multinomial()` (7個の `binom()` を経由する重い計算) の再計算を
//   毎候補ごとに行う必要がなくなる。encode (`multiset_perm_rank`) / decode
//   (`multiset_perm_unrank`) の両方に適用している。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace repe {

// 256bit = 32byte
constexpr int kRecordBytes = 32;

// 玉を除いた非玉駒の総数 (歩18,香4,桂4,銀4,金4,角2,飛2 = 38)。
constexpr int kNPieces = 38;
// 盤面のマス数 / 玉を除いたマス数
constexpr int kNSquares = 81;
constexpr int kNSquaresNoKing = 79;

// 駒番号(0=歩,1=香,2=桂,3=銀,4=金,5=角,6=飛)ごとの総数。
// cshogi の hand index (HPAWN=0, HLANCE=1, ..., HROOK=6) と同じ並び。
constexpr int kTypeCounts[7] = {18, 4, 4, 4, 4, 2, 2};

// 成り情報 (Part3) の対象駒番号 (金・玉を除く)。
constexpr int kPromotableTypes[6] = {0, 1, 2, 3, 5, 6};
constexpr int kPromotableCounts[6] = {18, 4, 4, 4, 2, 2};

// Part2 の先頭何要素を lookup table 化するか。
constexpr int kPart2LutDepth = 3;

using u128 = unsigned __int128;

namespace detail {

// cshogi の生駒種 (1-14, Noneや玉8を除く) から (駒番号0-6, 成りフラグ) へ変換。
inline bool raw_to_type(int raw, int* type_idx, bool* promoted) {
    switch (raw) {
        case 1: *type_idx = 0; *promoted = false; return true;   // 歩
        case 9: *type_idx = 0; *promoted = true; return true;    // と
        case 2: *type_idx = 1; *promoted = false; return true;   // 香
        case 10: *type_idx = 1; *promoted = true; return true;   // 成香
        case 3: *type_idx = 2; *promoted = false; return true;   // 桂
        case 11: *type_idx = 2; *promoted = true; return true;   // 成桂
        case 4: *type_idx = 3; *promoted = false; return true;   // 銀
        case 12: *type_idx = 3; *promoted = true; return true;   // 成銀
        case 7: *type_idx = 4; *promoted = false; return true;   // 金
        case 5: *type_idx = 5; *promoted = false; return true;   // 角
        case 13: *type_idx = 5; *promoted = true; return true;   // 馬
        case 6: *type_idx = 6; *promoted = false; return true;   // 飛
        case 14: *type_idx = 6; *promoted = true; return true;   // 龍
        default: return false;                                  // 玉やNoneはここに来ない
    }
}

// (駒番号0-6, 成りフラグ) から cshogi の生駒種 (1-14) へ変換。
inline int type_to_raw(int type_idx, bool promoted) {
    static const int kTable[7][2] = {
        {1, 9},    // 歩 / と
        {2, 10},   // 香 / 成香
        {3, 11},   // 桂 / 成桂
        {4, 12},   // 銀 / 成銀
        {7, 7},    // 金 (成りなし、常に7)
        {5, 13},   // 角 / 馬
        {6, 14},   // 飛 / 龍
    };
    return kTable[type_idx][promoted ? 1 : 0];
}

// C(n, k) を overflow-safe な乗除交互の標準アルゴリズムで計算する。
// `result = result * (n-i) / (i+1)` を i=0..k-1 の順に適用すると、各 step の
// 除算は必ず割り切れる (C(n, i+1) が整数であることの帰結)。
// 本モジュールが扱う n<=79 の範囲では中間値・最終値とも u128 に収まる
// (C(79, k) の最大値は約76bit)。
//
// lookup table 構築時 (`tables::part1_tables` / `tables::part2_tables` の
// 初回呼び出し) にのみ使う。record ごとのホットパスでは呼ばない。
inline u128 binom(int n, int k) {
    if (k < 0 || k > n) return 0;
    if (k > n - k) k = n - k;
    u128 result = 1;
    for (int i = 0; i < k; ++i) {
        result *= static_cast<u128>(n - i);
        result /= static_cast<u128>(i + 1);
    }
    return result;
}

// 多重集合の順列総数 n! / (c_1! c_2! ... c_m!) を、二項係数の連鎖積として
// 計算する (n! 自体を経由しないため n=38 でも overflow しない)。
// [`binom`] 同様、lookup table 構築時にのみ使う (ホットパスでは
// incremental multinomial の恒等式を使うため呼ばない)。
inline u128 multinomial(int n, const int counts[7]) {
    int remaining = n;
    u128 result = 1;
    for (int i = 0; i < 7; ++i) {
        result *= binom(remaining, counts[i]);
        remaining -= counts[i];
    }
    return result;
}

}  // namespace detail

// ============================================================
//   Lookup tables (record に依らない定数、初回呼び出し時に一度だけ構築)
// ============================================================

namespace tables {

// Part1 用: `C(v,i)` (v=0..=79, i=0..=38) と、盤上駒数 `s` ごとの累積 offset
// (`offset[s] = Σ_{k<s} C(79,k)*2^k*(39-k)`, s=0..=39)。
struct Part1Tables {
    u128 binom79[kNSquaresNoKing + 1][kNPieces + 1];
    u128 offset[kNPieces + 2];
};

inline const Part1Tables& part1_tables() {
    static const Part1Tables kTables = [] {
        Part1Tables t{};
        for (int v = 0; v <= kNSquaresNoKing; ++v) {
            for (int i = 0; i <= kNPieces; ++i) {
                t.binom79[v][i] = detail::binom(v, i);
            }
        }
        t.offset[0] = 0;
        for (int s = 0; s <= kNPieces; ++s) {
            const u128 state_count = t.binom79[kNSquaresNoKing][s] *
                                      (static_cast<u128>(1) << s) *
                                      static_cast<u128>(kNPieces + 1 - s);
            t.offset[s + 1] = t.offset[s] + state_count;
        }
        return t;
    }();
    return kTables;
}

// 駒番号 (0-6) を選ぶ際の累積閾値。`thresholds[s]` は「駒番号 `0..=s` を
// 選んだ場合の rank の上限 (exclusive)」。encode/decode 共通で使う
// (decode: `rank < thresholds[s]` となる最小の s を探す。
//  encode: 選ばれた駒番号の1つ手前までの累積を rank に加算する)。
using Thresholds = std::array<u128, 7>;

inline Thresholds thresholds_for(std::array<int, 7> counts, int remaining) {
    Thresholds result{};
    u128 cum = 0;
    for (int s = 0; s < 7; ++s) {
        if (counts[s] > 0) {
            counts[s] -= 1;
            cum += detail::multinomial(remaining - 1, counts.data());
            counts[s] += 1;
        }
        result[s] = cum;
    }
    return result;
}

// Part2 の先頭 `kPart2LutDepth` (=3) 要素用。初期状態 `kTypeCounts` が固定
// なので、先頭3要素のデコード/エンコードに現れる状態は
// `7 + 7*7 + 7*7*7 = 399` 通りしかなく、record に依らず事前計算できる。
struct Part2Tables {
    Thresholds level0;
    Thresholds level1[7];
    Thresholds level2[7][7];
};

inline const Part2Tables& part2_tables() {
    static const Part2Tables kTables = [] {
        Part2Tables t{};
        std::array<int, 7> base{};
        for (int i = 0; i < 7; ++i) base[i] = kTypeCounts[i];

        t.level0 = thresholds_for(base, kNPieces);
        for (int s0 = 0; s0 < 7; ++s0) {
            if (base[s0] == 0) continue;
            std::array<int, 7> c1 = base;
            c1[s0] -= 1;
            t.level1[s0] = thresholds_for(c1, kNPieces - 1);
            for (int s1 = 0; s1 < 7; ++s1) {
                if (c1[s1] == 0) continue;
                std::array<int, 7> c2 = c1;
                c2[s1] -= 1;
                t.level2[s0][s1] = thresholds_for(c2, kNPieces - 2);
            }
        }
        return t;
    }();
    return kTables;
}

// `thresholds` と現在の `rank` から、選ばれる駒番号・次要素用の rank・選ばれた
// 枝の幅 (= `multinomial(remaining-1, counts_with_s_decremented)`) を求める。
// 幅は4要素目以降のフォールバックで使う「現在の状態の multinomial 総数」の
// 種になる (`multinomial()` を呼ばずに済ませるため)。
inline void decode_one(const Thresholds& thresholds, u128 rank, int* out_symbol, u128* out_rank,
                        u128* out_width) {
    u128 prev = 0;
    for (int s = 0; s < 7; ++s) {
        const u128 t = thresholds[s];
        if (rank < t) {
            *out_symbol = s;
            *out_rank = rank - prev;
            *out_width = t - prev;
            return;
        }
        prev = t;
    }
    // `rank` が正しい範囲 (`< thresholds[6]`) なら到達しない。壊れた REPE
    // record の場合のみ防御的に最後の駒番号へ fallback する。
    *out_symbol = 6;
    *out_rank = (rank > prev) ? (rank - prev) : 0;
    *out_width = (thresholds[6] > prev) ? (thresholds[6] - prev) : 0;
}

}  // namespace tables

namespace detail {

// combinatorial number system による組合せの rank 化。
// `positions` は昇順の要素インデックス列 (0-indexed, 長さ k)。
// `n=79` 固定なので `C(v,i)` は `tables::part1_tables()` を引くだけで済む。
inline u128 comb_rank(const std::vector<int>& positions) {
    const auto& t = tables::part1_tables();
    u128 rank = 0;
    for (size_t i = 0; i < positions.size(); ++i) {
        rank += t.binom79[positions[i]][static_cast<int>(i) + 1];
    }
    return rank;
}

// combinatorial number system による組合せの逆写像 (unrank)。
// n=79個からk個選ぶ組合せのうちrank番目(0-indexed)を昇順配列で返す。
// `C(v,i)` の計算は全て `tables::part1_tables()` の lookup で済ませる。
inline void comb_unrank(u128 rank, int k, int out[/*k*/]) {
    const auto& t = tables::part1_tables();
    int upper = kNSquaresNoKing - 1;
    for (int i = k; i >= 1; --i) {
        int lo = i - 1, hi = upper;
        int best = lo;
        int l = lo, h = hi;
        while (true) {
            int mid = l + (h - l) / 2;
            if (t.binom79[mid][i] <= rank) {
                best = mid;
                if (mid == h) break;
                l = mid + 1;
            } else {
                if (mid == l) break;
                h = mid - 1;
            }
        }
        out[i - 1] = best;
        rank -= t.binom79[best][i];
        upper = (best == 0) ? 0 : (best - 1);
    }
}

// 多重集合順列の rank 化。`seq` は駒番号(0-6)列 (長さ `kNPieces` 前提)、
// `counts` は各駒番号の総数 (`kTypeCounts` 前提)。
//
// 先頭 `kPart2LutDepth` (=3) 要素は `tables::part2_tables()` の閾値を直接
// 引いて rank に加算する。4要素目以降は、直前の状態の multinomial 値 `m`
// から `multinomial(remaining-1, counts with c_s -= 1) == m * c_s / remaining`
// という恒等式で乗除算1回に置き換える (`multinomial()` の再計算を避ける)。
inline u128 multiset_perm_rank(const std::vector<int>& seq, const int counts[7]) {
    const auto& t = tables::part2_tables();

    int remaining_counts[7];
    std::memcpy(remaining_counts, counts, sizeof(remaining_counts));
    int remaining = 0;
    for (int i = 0; i < 7; ++i) remaining += remaining_counts[i];

    u128 rank = 0;
    u128 m = 0;  // 直前の状態の multinomial(remaining, remaining_counts)

    auto contribute_from_table = [&](const tables::Thresholds& th, int sym) {
        const u128 prev = (sym > 0) ? th[sym - 1] : static_cast<u128>(0);
        rank += prev;
        m = th[sym] - prev;
    };

    const int len = static_cast<int>(seq.size());
    for (int idx = 0; idx < len; ++idx) {
        const int sym = seq[idx];
        if (idx == 0) {
            contribute_from_table(t.level0, sym);
        } else if (idx == 1) {
            contribute_from_table(t.level1[seq[0]], sym);
        } else if (idx == 2) {
            contribute_from_table(t.level2[seq[0]][seq[1]], sym);
        } else {
            u128 cum = 0;
            for (int s = 0; s < sym; ++s) {
                const int c_s = remaining_counts[s];
                if (c_s == 0) continue;
                cum += m * static_cast<u128>(c_s) / static_cast<u128>(remaining);
            }
            rank += cum;
            m = m * static_cast<u128>(remaining_counts[sym]) / static_cast<u128>(remaining);
        }
        remaining_counts[sym] -= 1;
        remaining -= 1;
    }
    return rank;
}

// 多重集合順列の逆写像 (unrank)。長さ `length` (`kNPieces` 前提) の駒番号列を
// 復元する。先頭 `kPart2LutDepth` 要素は table 引き、4要素目以降は
// [`multiset_perm_rank`] と同じ incremental multinomial でフォールバックする。
inline void multiset_perm_unrank(u128 rank, const int counts[7], int length,
                                  std::vector<int>* out_seq) {
    const auto& t = tables::part2_tables();

    int remaining_counts[7];
    std::memcpy(remaining_counts, counts, sizeof(remaining_counts));
    int remaining = 0;
    for (int i = 0; i < 7; ++i) remaining += remaining_counts[i];

    out_seq->assign(static_cast<size_t>(length), 0);

    int s0 = 0, s1 = 0, s2 = 0;
    u128 r = rank, w = 0;

    tables::decode_one(t.level0, r, &s0, &r, &w);
    (*out_seq)[0] = s0;
    remaining_counts[s0] -= 1;
    remaining -= 1;

    if (length >= 2) {
        tables::decode_one(t.level1[s0], r, &s1, &r, &w);
        (*out_seq)[1] = s1;
        remaining_counts[s1] -= 1;
        remaining -= 1;
    }

    if (length >= 3) {
        tables::decode_one(t.level2[s0][s1], r, &s2, &r, &w);
        (*out_seq)[2] = s2;
        remaining_counts[s2] -= 1;
        remaining -= 1;
    }

    u128 m = w;  // 直前 (3要素目まで decode 済み) の multinomial 総数

    for (int idx = kPart2LutDepth; idx < length; ++idx) {
        int chosen = -1;
        u128 chosen_cnt = 0;
        for (int s = 0; s < 7; ++s) {
            const int c_s = remaining_counts[s];
            if (c_s == 0) continue;
            // cnt = multinomial(remaining-1, counts_with_s_decremented)
            //     = m * c_s / remaining (恒等式、割り切れる)
            const u128 cnt = m * static_cast<u128>(c_s) / static_cast<u128>(remaining);
            if (r < cnt) {
                chosen = s;
                chosen_cnt = cnt;
                break;
            }
            r -= cnt;
        }
        if (chosen < 0) {
            throw std::invalid_argument(
                "multiset_perm_unrank: rank out of range for current REPE Part2 state");
        }
        (*out_seq)[idx] = chosen;
        remaining_counts[chosen] -= 1;
        remaining -= 1;
        m = chosen_cnt;
    }
}

}  // namespace detail

// ============================================================
//   Public API
// ============================================================

// エンコード時に必要な入力 (Python側の cshogi.Board から取り出した値)。
struct EncodeInput {
    // cshogi の生駒コード。長さ81、index = file*9+rank (cshogi の Square と同じ)。
    // 0 = 空、1-14 = 先手駒、17-30 = 後手駒 (raw+16)。
    const int* pieces;  // [81]
    // 持ち駒枚数。index 0-6 = 歩,香,桂,銀,金,角,飛 (cshogi の hand index と同じ)。
    const int* hand_black;  // [7]
    const int* hand_white;  // [7]
    int black_king_sq;      // 0-80
    int white_king_sq;      // 0-80
    int turn;                // 0=Black(先手), 1=White(後手) が手番
    int game_result_stm;     // 手番側から見た勝敗。1=win, 0=draw, -1=lose
    int eval_stm;             // 手番側から見た評価値 (int16 range)
};

// デコード結果。`pieces` / `hand_*` は常に「手番側=BLACK, 相手側=WHITE」に
// 正規化された表現になる (REPEは手番情報を保存しないため)。
struct DecodedPosition {
    int pieces[kNSquares];   // 0=空, 1-14=自分(先手扱い), 17-30=相手(後手扱い)
    int hand_black[7];       // 自分(手番側)の持ち駒
    int hand_white[7];       // 相手の持ち駒
    int black_king_sq;       // 自玉位置 (正規化後)
    int white_king_sq;       // 敵玉位置 (正規化後)
    int game_result_stm;     // 1/0/-1
    int eval_stm;             // int16
};

// `EncodeInput` から REPE の32byteレコードを生成する。
// `out` は呼び出し側で確保した32byteバッファ。
inline void encode(const EncodeInput& in, uint8_t out[kRecordBytes]) {
    const bool self_is_white = (in.turn != 0);
    // 180度回転: 手番側がWHITEのときは sq -> 80-sq。
    auto canon_sq = [&](int sq) { return self_is_white ? (kNSquares - 1 - sq) : sq; };

    const int self_king_sq = canon_sq(self_is_white ? in.white_king_sq : in.black_king_sq);
    const int enemy_king_sq = canon_sq(self_is_white ? in.black_king_sq : in.white_king_sq);

    // 玉2枚を除いた79マスを、正規化後のマス番号の昇順で列挙する。
    std::vector<int> available;
    available.reserve(kNSquaresNoKing);
    for (int sq = 0; sq < kNSquares; ++sq) {
        if (sq != self_king_sq && sq != enemy_king_sq) available.push_back(sq);
    }

    std::vector<int> occupied_compact;
    std::vector<int> side_bits_list;
    std::vector<int> seq_types;
    std::vector<uint8_t> seq_board_promoted;

    for (int compact_i = 0; compact_i < static_cast<int>(available.size()); ++compact_i) {
        const int canon = available[compact_i];
        // canon_sq は対合(involution)なので逆変換も同じ式。
        const int orig_sq = self_is_white ? (kNSquares - 1 - canon) : canon;
        const int piece = in.pieces[orig_sq];
        if (piece == 0) continue;

        const bool piece_is_white = (piece >= 17);
        const int raw = piece_is_white ? (piece - 16) : piece;
        const bool is_self = (piece_is_white == self_is_white);

        int type_idx;
        bool promoted;
        if (!detail::raw_to_type(raw, &type_idx, &promoted)) {
            throw std::invalid_argument("encode: unexpected piece type on board (king leaked into scan?)");
        }

        occupied_compact.push_back(compact_i);
        side_bits_list.push_back(is_self ? 0 : 1);
        seq_types.push_back(type_idx);
        seq_board_promoted.push_back(promoted ? 1 : 0);
    }

    const int s = static_cast<int>(occupied_compact.size());

    const int* self_hand = self_is_white ? in.hand_white : in.hand_black;
    const int* enemy_hand = self_is_white ? in.hand_black : in.hand_white;
    int hand_self_total = 0;
    for (int i = 0; i < 7; ++i) hand_self_total += self_hand[i];

    // --- Part1 ---
    const u128 comb_r = detail::comb_rank(occupied_compact);
    u128 side_bits = 0;
    for (size_t i = 0; i < side_bits_list.size(); ++i) {
        side_bits |= static_cast<u128>(side_bits_list[i]) << i;
    }

    const u128 offset = tables::part1_tables().offset[s];

    const u128 rank_for_s =
        ((comb_r << s) + side_bits) * static_cast<u128>(kNPieces + 1 - s) +
        static_cast<u128>(hand_self_total);
    const u128 rank1 = offset + rank_for_s;
    const u128 part1 =
        (rank1 * 81 + static_cast<u128>(self_king_sq)) * 81 + static_cast<u128>(enemy_king_sq);

    // --- Part2 ---
    std::vector<int> full_seq = seq_types;
    full_seq.reserve(kNPieces);
    for (int type_idx = 0; type_idx < 7; ++type_idx) {
        for (int c = 0; c < self_hand[type_idx]; ++c) full_seq.push_back(type_idx);
    }
    for (int type_idx = 0; type_idx < 7; ++type_idx) {
        for (int c = 0; c < enemy_hand[type_idx]; ++c) full_seq.push_back(type_idx);
    }
    if (static_cast<int>(full_seq.size()) != kNPieces) {
        throw std::invalid_argument("encode: REPE requires exactly 38 non-king pieces total");
    }

    const u128 part2 = detail::multiset_perm_rank(full_seq, kTypeCounts);

    // --- Part3 ---
    int shift_base[7] = {0};
    bool is_promotable[7] = {false};
    {
        int sh = 0;
        for (int t = 0; t < 6; ++t) {
            const int ti = kPromotableTypes[t];
            shift_base[ti] = sh;
            is_promotable[ti] = true;
            sh += kPromotableCounts[t];
        }
    }
    int next_bit_of_type[7] = {0};
    u128 part3 = 0;
    for (int idx = 0; idx < static_cast<int>(full_seq.size()); ++idx) {
        const int ti = full_seq[idx];
        if (!is_promotable[ti]) continue;
        const bool is_board = (idx < s);
        const uint8_t bit = (is_board && seq_board_promoted[idx]) ? 1 : 0;
        const int bitpos = shift_base[ti] + next_bit_of_type[ti];
        part3 |= static_cast<u128>(bit) << bitpos;
        next_bit_of_type[ti] += 1;
    }

    // --- Part4 ---
    int result_bits;
    if (in.game_result_stm == 1) {
        result_bits = 0b00;
    } else if (in.game_result_stm == -1) {
        result_bits = 0b10;
    } else if (in.game_result_stm == 0) {
        result_bits = 0b01;
    } else {
        throw std::invalid_argument("encode: invalid game_result_stm");
    }

    // --- Part5 ---
    const uint16_t eval_u16 = static_cast<uint16_t>(in.eval_stm);

    // 256bit全体は u128 1個には収まらないため、Part1 (128bit) と
    // Part2+Part3+Part4+Part5 (76+34+2+16=128bit) の2つの u128 に分けて
    // 組み立てる (Python/Rust実装と同じく、ちょうど16byte境界で分割できる)。
    u128 rest = (((part2 << 34) | part3) << 2) | static_cast<u128>(result_bits);
    rest = (rest << 16) | static_cast<u128>(eval_u16);

    // ビッグエンディアンで32byteへ書き出す (out[0..16]=Part1, out[16..32]=rest)。
    u128 hi = part1;
    for (int i = 0; i < 16; ++i) {
        out[15 - i] = static_cast<uint8_t>(hi & 0xFF);
        hi >>= 8;
    }
    u128 lo = rest;
    for (int i = 0; i < 16; ++i) {
        out[31 - i] = static_cast<uint8_t>(lo & 0xFF);
        lo >>= 8;
    }
}

// REPEの32byteレコードをデコードする。不正なレコード (reserved result bits
// など) は `std::invalid_argument` を投げる。
inline void decode(const uint8_t data[kRecordBytes], DecodedPosition* out) {
    u128 part1 = 0;
    for (int i = 0; i < 16; ++i) {
        part1 = (part1 << 8) | data[i];
    }
    u128 rest = 0;
    for (int i = 16; i < 32; ++i) {
        rest = (rest << 8) | data[i];
    }

    const uint16_t eval_u16 = static_cast<uint16_t>(rest & 0xFFFF);
    rest >>= 16;
    const int result_bits = static_cast<int>(rest & 0b11);
    rest >>= 2;
    const u128 part3 = rest & ((static_cast<u128>(1) << 34) - 1);
    rest >>= 34;
    const u128 part2 = rest;  // 残り76bitがそのままPart2

    out->eval_stm = static_cast<int16_t>(eval_u16);
    switch (result_bits) {
        case 0b00: out->game_result_stm = 1; break;
        case 0b10: out->game_result_stm = -1; break;
        case 0b01: out->game_result_stm = 0; break;
        default:
            throw std::invalid_argument("decode: reserved result bits (0b11) in REPE record");
    }

    // --- Part1 のデコード: enemyKing -> selfKing -> rank の順 ---
    u128 v = part1;
    const int enemy_king_sq = static_cast<int>(v % 81);
    v /= 81;
    const int self_king_sq = static_cast<int>(v % 81);
    v /= 81;
    const u128 rank1 = v;

    if (self_king_sq >= kNSquares || enemy_king_sq >= kNSquares || self_king_sq == enemy_king_sq) {
        throw std::invalid_argument("decode: invalid king squares in REPE record");
    }

    // `s` (盤上駒数) を、事前計算した累積 offset table の二分探索で決める
    // (以前は s=0,1,2,... と回しながら毎回 C(79,s) を計算し直す線形探索だった)。
    const auto& t1 = tables::part1_tables();
    const u128* offset_end = t1.offset + (kNPieces + 2);
    const u128* it = std::upper_bound(t1.offset, offset_end, rank1);
    const int idx = static_cast<int>(it - t1.offset);
    if (idx <= 0 || idx > kNPieces + 1) {
        throw std::invalid_argument("decode: invalid REPE Part1 rank (out of range)");
    }
    const int s = idx - 1;
    const u128 rem = rank1 - t1.offset[idx - 1];

    const u128 mult = static_cast<u128>(kNPieces + 1 - s);
    const int hand_self_total = static_cast<int>(rem % mult);
    const u128 tmp2 = rem / mult;
    const u128 side_bits = (s > 0) ? (tmp2 % (static_cast<u128>(1) << s)) : 0;
    const u128 comb_r = (s > 0) ? (tmp2 >> s) : 0;

    std::vector<int> occupied_compact(s);
    detail::comb_unrank(comb_r, s, occupied_compact.data());

    std::vector<int> available;
    available.reserve(kNSquaresNoKing);
    for (int sq = 0; sq < kNSquares; ++sq) {
        if (sq != self_king_sq && sq != enemy_king_sq) available.push_back(sq);
    }

    std::vector<int> seq_board_sq(s);
    for (int i = 0; i < s; ++i) seq_board_sq[i] = available[occupied_compact[i]];
    std::vector<uint8_t> seq_board_side(s);
    for (int i = 0; i < s; ++i) seq_board_side[i] = static_cast<uint8_t>((side_bits >> i) & 1);

    const int hand_enemy_total = (kNPieces - s) - hand_self_total;

    // --- Part2 のデコード ---
    std::vector<int> full_seq;
    detail::multiset_perm_unrank(part2, kTypeCounts, kNPieces, &full_seq);

    int self_hand[7] = {0};
    for (int i = s; i < s + hand_self_total; ++i) self_hand[full_seq[i]] += 1;
    int enemy_hand[7] = {0};
    for (int i = s + hand_self_total; i < s + hand_self_total + hand_enemy_total; ++i) {
        enemy_hand[full_seq[i]] += 1;
    }

    // --- Part3 のデコード ---
    int shift_base[7] = {0};
    bool is_promotable[7] = {false};
    {
        int sh = 0;
        for (int t = 0; t < 6; ++t) {
            const int ti = kPromotableTypes[t];
            shift_base[ti] = sh;
            is_promotable[ti] = true;
            sh += kPromotableCounts[t];
        }
    }
    int next_bit_of_type[7] = {0};
    std::vector<uint8_t> promoted_board(s, 0);
    for (int idx2 = 0; idx2 < kNPieces; ++idx2) {
        const int ti = full_seq[idx2];
        if (!is_promotable[ti]) continue;
        const int bitpos = shift_base[ti] + next_bit_of_type[ti];
        next_bit_of_type[ti] += 1;
        const uint8_t bit = static_cast<uint8_t>((part3 >> bitpos) & 1);
        if (idx2 < s) promoted_board[idx2] = bit;
    }

    // --- 局面の再構築 (自分=BLACK, 相手=WHITEとして正規化) ---
    std::memset(out->pieces, 0, sizeof(out->pieces));
    out->pieces[self_king_sq] = 8;   // BKING
    out->pieces[enemy_king_sq] = 24;  // WKING
    for (int i = 0; i < s; ++i) {
        const int sq = seq_board_sq[i];
        const int type_idx = full_seq[i];
        const bool promoted = promoted_board[i] != 0;
        const int raw = detail::type_to_raw(type_idx, promoted);
        const int side = seq_board_side[i];  // 0=self(BLACK), 1=enemy(WHITE)
        out->pieces[sq] = (side == 0) ? raw : (raw + 16);
    }
    std::memcpy(out->hand_black, self_hand, sizeof(out->hand_black));
    std::memcpy(out->hand_white, enemy_hand, sizeof(out->hand_white));
    out->black_king_sq = self_king_sq;
    out->white_king_sq = enemy_king_sq;
}

}  // namespace repe
