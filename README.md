# build-my-own-text-editor

[Build Your Own Text Editor](https://viewsourcecode.org/snaptoken/kilo/) の軽量テキストエディタ "kilo" を作ってみる

本家リポジトリ：[antirez/kilo](https://github.com/antirez/kilo)

## Requirements

- gcc
    - `-std=C99`
- GNU Make
- POSIXヘッダ
    - `<sys/ioctl.h>`
    - `<termios.h>`
    - `<unistd.h>`

## Build

- `make` コマンドで `bin/release` 以下に `kilo` をビルド（out-of-source ビルド）

```sh
$ make
gcc -Wall -Wextra -Wpedantic -std=c99 -O2 -c src/kilo.c -o bin/release/src/kilo.o
gcc -Wall -Wextra -Wpedantic -std=c99 -O2 bin/release/src/kilo.o -o bin/release/kilo
```

- `make DEBUG=yes` でデバッグ情報を付加し、 `bin/debug` 以下に `kilo` をビルド
