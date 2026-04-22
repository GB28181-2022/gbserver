# 第三方依赖

本目录通过 **Git Submodule** 引用固定版本的第三方源码，请勿再手动 `git clone` 覆盖子目录。

## 克隆主仓库时拉取子模块

```bash
git clone --recurse-submodules https://github.com/GB28181-2022/gbserver.git
cd gbserver
```

若已克隆主仓库但未带子模块：

```bash
git submodule update --init
```

（当前子模块未使用嵌套 submodule，一般无需 `--recursive`；若日后升级依赖需要，再改为 `git submodule update --init --recursive`。）

## 子模块说明

| 目录 | 用途 | 上游 |
|------|------|------|
| `pjproject` | SIP 协议栈（PJSIP） | [pjsip/pjproject](https://github.com/pjsip/pjproject) |
| `ZLToolKit` | 网络与 HTTP 工具库 | [ZLMediaKit/ZLToolKit](https://github.com/ZLMediaKit/ZLToolKit) |

## 构建

在子模块就绪后，于 `backend/build` 中执行：

```bash
cmake .. && make
```

详见项目根目录 `README.md`。
