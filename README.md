# build-my-own-text-editor

テキストエディタ "kilo" を作ってみる

- [Build Your Own Text Editor](https://viewsourcecode.org/snaptoken/kilo/): C言語で軽量テキストエディタを作成するチュートリアルガイド
- [antirez/kilo](https://github.com/antirez/kilo): 上記チュートリアルで作成するテキストエディタ "kilo"

## Requirement

- gcc (C99)
- GNU Make
- POSIXライブラリ
    - `<fcntl.h>`
    - `<sys/ioctl.h>`
    - `<sys/types.h>`
    - `<termios.h>`
    - `<unistd.h>`

## Build

ビルド：

```sh
# build/release/kilo
$ make
```

デバッグビルド：

```sh
# build/debug/kilo
$ make DEBUG=yes
```

## Usage

```sh
$ kilo <filename>
```

## License

BSD2-Clause License

- kilo: [BSD2-Clause License](https://github.com/snaptoken/kilo-tutorial/blob/master/steps.diff.LICENSE)
- "Build Your Own Text Editor": [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)
