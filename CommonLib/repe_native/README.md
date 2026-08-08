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
このマシン上でエンコード/デコードした際のスループット:

| | 純Python | native (C++/Cython) |
|---|---|---|
| encode | 約14,500 records/sec | 約53,700 records/sec (約3.7倍) |
| decode | 約10,000 records/sec | 約24,700 records/sec (約2.5倍) |

decode側の伸びが相対的に小さいのは、`cshogi.Board.set_pieces()` の呼び出しや
Cython→Pythonリストへの変換など、native化していないcshogi/Python側の処理が
相対的に支配的になるためです (組合せ数学の計算自体はどちらも同程度、あるいは
それ以上高速化されています)。実際の倍率は環境やCPUに依存します。

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
