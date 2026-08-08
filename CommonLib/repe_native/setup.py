"""REPE native extension (C++ + Cython) のビルドスクリプト。

使い方:

    cd CommonLib/repe_native
    pip install cython --break-system-packages   # 未インストールなら
    python setup.py build_ext --inplace

成功すると `repe_native*.so` (Linux/Mac) または `repe_native*.pyd` (Windows)
がこのディレクトリに生成される。`CommonLib/RepeFormatLib.py` は import path上に
`repe_native` があれば自動的にそれを使い、無ければ純 Python 実装にフォールバック
するので、このビルドは必須ではなく任意の高速化オプションという位置付け。

詳細は README.md を参照。
"""

from setuptools import Extension, setup

try:
    from Cython.Build import cythonize
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "Cython が見つかりません。`pip install cython` してから再実行してください。"
    ) from exc


extensions = [
    Extension(
        name="repe_native",
        sources=["repe_native.pyx"],
        language="c++",
        extra_compile_args=["-O3", "-std=c++17"],
    )
]

setup(
    name="repe_native",
    ext_modules=cythonize(
        extensions,
        compiler_directives={"language_level": "3"},
    ),
    zip_safe=False,
)
