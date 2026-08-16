# Atlanage

中文编程语言，自举编译器，运行时使用裸汇编实现。

## 使用

AT-V1.exe -exe examples/hello.at
examples/hello.exe

atc.sh examples/hello.at

AT-V1.exe examples/hello.at > hello.asm
AT-V1.exe -obj hello.asm -out=hello.obj
AT-V1.exe -link hello.obj -out=hello.exe
hello.exe

## 目录

atlanage/
├── AT-V1.exe
├── atlc.exe
├── atl_rt.obj
├── atc.sh
├── examples/
├── 文档/
├── lib/
├── runtime/
└── tools/atp/

## 文档

文档/介绍.md