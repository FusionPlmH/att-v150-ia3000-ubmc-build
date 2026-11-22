# 「死文件」核查：到底是真的没人用，还是引用错了

> **审计对象 = 出厂镜像 `1.2.0.8`（eMMC p2）**，不是现在机器上跑的那份。
> 做这次审计的目的是回答"移植文件时会不会踩到链错版本"，结论至今有效。
> 现状对照见文末第四节。

结论先行：**是真死，但不是引用错误导致的**。两类原因，都不是 bug。

方法：从**可执行文件**出发做可达性分析（BFS 沿 `NEEDED` 走），
额外把 `dlopen` 的显式路径（全树搜 `/usr/lib/xxx.so` 形式的字符串）、
PAM 模块（`/lib/security/*`）、NSS 模块（`libnss_*`）、
netopeer transapi 都算作根，避免把运行时加载误判成孤儿。

## 一、先确认出厂镜像本身是自洽的

```
ELF 总数（解析到动态段的）: 868
NEEDED 无法解析的 ELF:      0
```

**零个**。也就是说出厂镜像里没有任何一个程序引用了不存在的库。
如果某个组件本该链 `.35` 却链错了版本，这里必然出现 unresolved —— 没有。

## 二、34 个真正不可达的库（去重后，含 lib32 共 68 文件，6.0 MB）

### ① 升级残留 —— 被新版本取代

| 旧文件 | 被谁取代 | 证据 |
|---|---|---|
| `libcurl.so.4.5.0` | `libcurl.so.4.6.0` | `libcurl.so.4` 软链已指向 4.6.0 |
| `libssh.so.4.4.2` | `libssh.so.4.8.6` | `libssh.so.4` 软链已指向 4.8.6 |
| `libnetsnmp.so.35.0.0` | `libnetsnmp.so.40.0.0` | 无版本软链指向 .40.0.0 |
| `libnetsnmpagent.so.35.0.0` | `.40.0.0` | 同上 |
| `libnetsnmpmibs.so.35.0.0` | `.40.0.0` | 同上 |
| `libnetsnmptrapd.so.30.0.3` / `.35.0.0` | `.40.0.0` | 同上 |
| `libnetsnmphelpers.so.{30,35,40}` | 无人使用（2,956 B 空壳）| net-snmp 编译产物，本身没人链 |

**成因**：buildroot 把一个包升级装进已有的 `target/` 目录时，
会写入新文件、把无版本软链重指到新版本，但**不会删除旧版本的实体文件**。
所以旧文件变成孤儿 —— 这不是"引用错了"，恰恰相反，引用是对的，
正因为软链正确指向新版，旧版才没人用。

### ② 装了但没被任何程序链接的包

| 库 | 大小 | 说明 |
|---|---|---|
| `libevent-2.1` / `_core` / `_extra` / `_openssl` / `_pthreads` | 5 个，共 555 KB | 无人链接 |
| `libical` / `libicalss` / `libicalvcal` | 3 个，共 365 KB | 日历库，本机无日历功能 |
| `libart_lgpl_2` / `libcairo-gobject` | 92 KB | 图形库，BMC 无图形栈 |
| `libform` / `libmenu` / `libpanel` / `libhistory` | 100 KB | readline/ncurses 附属，UI 没用 |
| `libiptc` / `libnftnl` | 122 KB | iptables 旧接口，实际走的是 `libxtables` |
| `libparted-fs-resize` | 74 KB | 分区库的 resize 附属 |
| `libpcrecpp` | 24 KB | pcre 的 C++ 封装 |
| `libturbojpeg` | 221 KB | jpeg 加速，无图形栈 |
| `libdbus-c++-glib-1` | 13 KB | C++ 绑定，实际用的是 `libdbus-glib-1` |
| `libss` / `libradius` / `libsimaka` | 56 KB | strongswan 的 PAM 插件，但 `/lib/security` 里没有对应模块 |
| `libfl` | 3 KB | flex 运行时 |

多半是历史上编过、后来从 buildroot config 里去掉了，但 target 目录没重建。

## 三、差点被我误判成孤儿的 4 个（**不是**死文件）

`readelf -d` 看不到 `dlopen`，必须扫字符串才能发现：

| 库 | 加载方式 | 加载者 |
|---|---|---|
| **`libprod_mgmtd_inst.so`** | `dlopen("/usr/lib/libprod_mgmtd_inst.so")` | `/usr/bin/is_mgmtd` |
| `libprod_cli_inst.so` | `dlopen("/usr/lib/libprod_cli_inst.so")` | `/usr/bin/is_cli` |
| `ubmc-nc.so` | netopeer transapi | `/etc/netopeer/modules.conf.d/ubmc.xml` |
| `libcharon.so.0` / `libstrongswan.so.0` | 正常 `NEEDED` | `/usr/libexec/ipsec/charon`、`starter` |

> ⚠️ 第一轮扫描时我犯过两个错，都已修正：
> 1. 根目录**没递归子目录** → 漏掉 `usr/libexec/ipsec/charon`，
>    误判 `libcharon` 不可达（它的软链其实是齐的）
> 2. 只看 `NEEDED` **没扫 dlopen** → 误判 `libprod_mgmtd_inst.so` 是孤儿。
>    这个库是 mgmtd 的产品插件，本地重编的版本必须放回同一位置、
>    保持 `/usr/lib/libprod_mgmtd_inst.so → .so.0.0.0` 软链可解析。

## 四、对自编译版的意义

- `libprod_mgmtd_inst.so` 的软链在自编译版里是完好的
  （`libprod_mgmtd_inst.so` → `.so.0` → `.so.0.0.0`，指向重编版），
  `is_mgmtd` 的 `dlopen` 能正常加载
- 出厂镜像零 unresolved，说明移植的二进制不会有"链错版本"的问题
- 这 34 个孤儿里，除了 net-snmp `.30/.35` 和 `libcurl 4.5.0`/`libssh 4.4.2`
  是升级残留外，其余都是本机用不到的包，**不必也不建议移植**

### 现在（第二十五轮）的情况 —— 孤儿换了个方向

因为后来的做法是"**按需从出厂移植、不重建 Buildroot**"，软链得手工核对，
所以残留的方向反过来了：多出来的是**新**库，而默认软链仍指向**旧**库。

```
libcurl.so.4    -> libcurl.so.4.6.0     ← 已重指到移植进来的新版
libnetsnmp.so   -> libnetsnmp.so.30.0.3 ← 仍指基础镜像里的 5.7.3
libnetsnmp.so.40 -> libnetsnmp.so.40.0.0   ← 5.9 只有带 soname 的链接
libssh.so.4     -> libssh.so.4.4.2      ← 仍指基础镜像里的 0.7.5
```

**这不会出问题**：ELF 找的是 soname（`libnetsnmp.so.40`、`libssh.so.4`），
无版本软链只影响 `-lxxx` 链接期。实测抽查 8 个移植过来的程序
（`snmptop` `snmpwalk` `ssh` `sshd` `nginx` `curl` `openssl` `valgrind`），
`NEEDED` 全部能在树内解析，**0 个 missing**。

> 教训：这种"搬文件"的升级方式，每次都要把
> ① 新文件的 soname 链接、② 默认软链是否该重指、③ `NEEDED` 能否解析
> 三件事一起核对。只看"程序能跑"很容易漏掉第二条。
