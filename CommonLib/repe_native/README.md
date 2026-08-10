# repe_native — REPEエンコード/デコードのC++/Cython高速版

`CommonLib/RepeFormatLib.py` の `encode_position` / `decode_position` が
内部で行っている組合せ順位 (comb_rank/unrank) と多重集合順列の順位
(multiset permutation rank/unrank) の計算は、純Pythonのループで書くと
1レコードあたりの計算コストが無視できません。cshogi 本体が「C++コア +
Cythonバインディング」という構成で高速化しているのに倣い、REPEについても
同じ構成の高速版をここに用意しています。

## 構成

```
repe_native/
├── repe_core.hpp     C++コア。組合せ数学・エンコード/デコードのロジック本体。
│                     ヘッダオンリーで、Python/Cythonに一切依存しない。
│                     `unsigned __int128` (GCC/Clang拡張) で128bit整数演算を行う。
├── repe_native.pyx   Cythonバインディング。cshogi.Boardから取り出した
│                     生データ (pieces配列・持ち駒配列・玉位置など) を
│                     C++側の構造体に詰め替えるだけの薄い層。
├── setup.py          ビルドスクリプト。
└── README.md         このファイル。
```

`RepeFormatLib.py` 側のビット配置規約 (79マスの走査順、side bit列の順序、
駒種類列の並び順、成り情報のビット割当順) と完全に同じアルゴリズムを
`repe_core.hpp` に実装しています。両実装の出力はテストデータ全件
(57,305局面) でバイト完全一致することを確認済みです。

## ビルド方法

```bash
cd CommonLib/repe_native
pip install cython --break-system-packages   # 未インストールなら
python setup.py build_ext --inplace
```

成功すると `repe_native.cpython-*.so` (Linux/Mac) または
`repe_native.cp*.pyd` (Windows) がこのディレクトリに生成されます。

`RepeFormatLib.py` は自身のファイル位置を基準にこのディレクトリを探し、
コンパイル済みの `.so`/`.pyd`/`.dylib` が実際に存在する場合のみ
`import repe_native` を試みます。ビルドしていない場合は自動的に
`RepeFormatLib.py` 内の純Python実装にフォールバックするので、**このビルドは
必須ではありません** (未ビルドでも `convert_teacher.py` は今まで通り動きます。
遅いだけです)。`RepeFormatLib.NATIVE_AVAILABLE` で有効/無効を確認できます。

```python
import RepeFormatLib
print(RepeFormatLib.NATIVE_AVAILABLE)  # True ならnative版を使用中
```

## 要件

- C++17 対応コンパイラ (GCC / Clang)。`unsigned __int128` 拡張を使うため、
  **MSVCでは直接ビルドできません**。Windows で使う場合は WSL / MinGW / clang-cl
  を利用してください (本プロジェクトの教師データ変換は主にLinux上での運用を
  想定しています)。
- Cython 3.x, setuptools。

## ベンチマーク (参考値)

`test.psv` (57,305局面, このリポジトリの検証に使用したものと同一形式) を
このマシン上でエンコード/デコードした際のスループット (同一セッション内で
直接比較したもの。絶対値は環境・CPUに強く依存するため、あくまで相対的な
目安):

| | 純Python | native (最適化前) | native (現在: LUT + incremental multinomial) |
|---|---|---|---|
| encode | 約7,700 rec/s | 約30,000 rec/s | **約81,300 rec/s** |
| decode | 約6,000 rec/s | 約12,400 rec/s | **約186,200 rec/s** |

native側は Rust版 (`tatara` の `crates/shogi-format/src/repe.rs`) と同じ
2段階の最適化を行っている:

1. **lookup table化**: 組合せ順位 (`C(v,i)`, `v=0..=79`) と、盤上駒数
   `s` ごとの累積offsetは record に依らない定数なので、初回呼び出し時に
   一度だけ (`repe::tables::part1_tables()` / `part2_tables()`、C++11の
   関数内staticでスレッドセーフに1回だけ構築) 事前計算しテーブル参照に
   置き換えている。
2. **incremental multinomial**: Part2 (駒種類列) の4要素目以降のフォールバック
   処理は、直前の状態の multinomial 値から `multinomial(remaining-1,
   counts[s]-=1) == m * counts[s] / remaining` という組合せ論の恒等式
   (割り切れる) で乗除算1回に置き換えており、`multinomial()` の再計算を
   避けている。decode側の伸びが特に大きいのはこの効果が大きい。

decode側の伸びがencodeより大きいのは、encode側はまだ盤面スキャン
(`cshogi.Board.pieces` の走査) など native化していない周辺処理の比率が
相対的に高いためです。

## 正しさの検証

`repe_core.hpp` は元々 Python (`RepeFormatLib.py`) → Rust (tatara連携作業時)
の順に実装・検証したアルゴリズムを、同じ規約のまま C++ に移植したものです。
このリポジトリでの検証時には、`test.psv` 全57,305局面について

- native encode の出力 == 純Python encode の出力 (バイト完全一致)
- native decode の出力 (盤面・持ち駒・玉位置・勝敗・評価値) == 純Python decode
  の出力

をすべて確認しています。加えて、開発時に独立した最小構成の C++
テストプログラムでも `repe_core.hpp` 単体の正しさを確認しています。

## トラブルシューティング

- `import repe_native` が失敗する / `NATIVE_AVAILABLE` が `False` のまま:
  - `python setup.py build_ext --inplace` を `repe_native/` ディレクトリ内で
    実行したか確認してください。
  - ビルドしたPythonのバージョン・アーキテクチャと実行時のものが一致しているか
    確認してください (`.so` のファイル名にPythonのABIタグが含まれています)。
- ビルドエラーになる: C++17対応コンパイラ (GCC 7+ / Clang 5+ 目安) と
  Cython 3.x が入っているか確認してください。
