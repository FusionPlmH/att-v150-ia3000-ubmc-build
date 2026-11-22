# IPMI 级远程管理能力盘点

目标：像 IPMI 一样远程看输出、发按键、控制开关机/重启、挂 ISO 启动。

结论：**这些功能固件里全都有，而且我们的构建与原厂 1.2.0.8 完全对齐**
（mgmtd 插件端点 73 条 vs 73 条，零差异）。之前不能用的只有 Web Console，
根因是防火墙缺例外，已在 [webtty-diagnosis.md](./webtty-diagnosis.md) 修掉。

## 能力矩阵

| IPMI 功能 | 状态 | 入口（Web / CLI） | 验证方式 |
|---|---|---|---|
| 远程看主机输出 | ✅ | `/admin/bmc/shell` | WS 收到完整启动日志 |
| 远程发按键 | ✅ | 同上 | 发 CR → 主机回显 login 提示符 |
| Console 日志回看 | ✅ | `/admin/bmc/console` | `/var/log/host_serial.log` 2.7 MB |
| 电源状态 | ✅ | `/admin/bmc/power` / `ubmc_sys_ctrl.sh -q` | 返回 `Host is powered up` |
| 开机 / 关机 / 强制关 / 复位 / 循环 | ✅ | 同上 `-o -s -f -r -c` | 接口齐全（未做破坏性实测） |
| 挂 ISO（本地） | ✅ | `/admin/bmc/usb` / `ubmc_usb_cdrom_ctrl.sh -a <iso>` | 实测 attach→`UDC: configured`，主机 dmesg 有 SCSI 交互 |
| 挂 ISO（Windows 共享） | ✅ | 同上 `-A <ip> <path> <user> <pass>` | 代码路径完整（mount cifs → gadget） |
| 卸载 ISO | ✅ | 同上 `-d` | 实测 `CDROM is free`，`UDC: not attached` |
| BIOS 升级 / 备份 / 恢复 | ✅ | `/admin/bmc/bios` | `/action/bmc/bios/*` 端点 |
| 系统事件日志 SEL | ✅ | `/admin/bmc/eventlog` | `/action/bmc/sel/clear`、`ubmc_ipmi_sel_prt` |
| 硬件健康（风扇/温度/电压/电源） | ✅ | 无独立页面 | `/status/bmc/health/*` |
| **IPMI over LAN**（`ipmitool -I lanplus`） | ✅ | 自建 `ubmc_ipmi_lan`（udp/623，RMCP+ suite 3） | pyghmi + python-ipmi 双客户端实测 |
| IPMI `sel list` / `sensor` / `fru` / `chassis bootdev` | ✅ | 同上 | ~190 条 SEL、24 个传感器有值、完整 FRU、bootdev 读写往返 |
| IPMI `sol activate` | ✅ | 同上（复用 console fan-out socket） | 完整 1636 字节横幅 + login 提示；BREAK 与 ACK 流控实测 |

## 各功能的实际入口

### 电源控制

```sh
/usr/sbin/ubmc_sys_ctrl.sh -q      # 电源状态
/usr/sbin/ubmc_sys_ctrl.sh -p      # 操作进度
/usr/sbin/ubmc_sys_ctrl.sh -o      # 开机
/usr/sbin/ubmc_sys_ctrl.sh -s      # 关机（正常）
/usr/sbin/ubmc_sys_ctrl.sh -f      # 强制断电
/usr/sbin/ubmc_sys_ctrl.sh -r      # 复位
/usr/sbin/ubmc_sys_ctrl.sh -c      # 电源循环
/usr/sbin/ubmc_sys_ctrl.sh -g      # RS232 线缆检测（注意：不是主机串口状态）
```

`ubmc_sys_ctrl.sh` 是外壳，真正干活的是 `/usr/bin/ubmc_sys_ctrl`。
同一个脚本还负责 BIOS 的升级/备份/恢复（`-b -u -su -bb -rb`）和
主机 SPI flash 的 mux 切换（GPIO 27）。

`-g` 返回 "Serial is disconnected" 指的是外壳 **RS232 口有没有插线**
（`RS232_CABLE_DETECTED`），跟主机串口控制台无关，不要被误导。

### 虚拟光驱（USB CDROM）

```sh
/usr/sbin/ubmc_usb_cdrom_ctrl.sh -a /var/images/x.iso          # 挂本地 ISO
/usr/sbin/ubmc_usb_cdrom_ctrl.sh -A 192.168.1.10 share/x.iso user pass   # 挂 Windows 共享
/usr/sbin/ubmc_usb_cdrom_ctrl.sh -d                            # 卸载
/usr/sbin/ubmc_usb_cdrom_ctrl.sh -s                            # 占用状态
/usr/sbin/ubmc_usb_cdrom_ctrl.sh -n                            # 当前 ISO 名
/usr/sbin/ubmc_usb_cdrom_ctrl.sh -t                            # 存放位置
/usr/sbin/ubmc_usb_cdrom_ctrl.sh -r /var/images/x.iso          # 删本地 ISO（在用会拒绝）
```

底层就是 `modprobe g_mass_storage file=<iso> cdrom=y`。

**放 ISO 的位置**：`/var/images` = `/dev/mmcblk0p8`，4.9 GB，实测可用 4.5 GB。
根文件系统是 initramfs，`/tmp` 只有 249 MB tmpfs，**不要把 ISO 放 /tmp**。

实测记录（2 MB 测试镜像）：

```
- 挂载后 : CDROM is occupied / _gadget_test.iso / /var/images
           lsmod: g_mass_storage, usb_f_mass_storage, libcomposite, udc_core
           /sys/class/udc/musb-hdrc.1/state = configured
           dmesg : g_mass_storage gadget: bulk-in set halt / sending command-failure status
                   ← 说明主机真的在给这个虚拟光驱发 SCSI 命令
- 卸载后 : CDROM is free / state = not attached
```

## 远程控制台的完整链路

```
主机 x86 串口
     │  (BMC 侧 /dev/ttyS1，115200)
     ▼
is_console_manager -H /dev/ttyS1 -O /dev/ttyS0
     ├──> /var/log/host_serial.log        ← Console Log 页读这个
     └──> /dev/uart-sim
              │  uart_sim_ubmc.ko（驱动名 MY-TTY-SIM，主设备号 151）
              ▼
          /dev/ttyF0
              │  serial_socket_client（注意：它开的是 ttyF0，不是 ttyS1）
              ▼
           ttyd 1.5.2-bd9de8c  :7681
              │  nginx  location ^~ /ws → proxy_pass http://0.0.0.0:7681/ws
              ▼
           浏览器 wss://<bmc>/ws （子协议 "tty"，首帧 {"AuthToken":"<token>"}）
```

要点：

- `serial_socket_client` 绑的是 **`/dev/ttyF0`**（模拟串口），不是 `/dev/ttyS1`。
  排查控制台时别看错设备。
- Web Console 能出画面的大前提是**主机侧也把 console 打到这个串口**。
  实测本机主机跑的是原厂 ATT Linux，串口停留在
  `USENEODDXTX0101USZZ01 login:`，Web Console 能完整收到。
- `/dev/ttyS1` 的 `rx` 计数在开机阶段会涨到 2.7 万字节左右，
  之后不再增长 —— 主机没有新输出而已，不是链路坏了。

端到端实测（走 nginx 的真实 wss）：

```
握手: HTTP/1.1 101 Switching Protocols
AuthToken 后收到: 1/usr/bin/serial_socket_client (ubmc) / 2{ } / 0Press Ctrl+']' ...
发送 ttyd 输入帧 '0' + CR 之后收到 16 帧：
  0\r\r\n
  0******...  Warning Notice  ...******
  0USENEODDXTX0101USZZ01 login:
```

## 接口在哪儿实现的（容易找错地方）

**`is_mgmtd` 本体里没有任何 `/action/bmc/*`** —— 两版（自编译 1.1.0.5
和原厂 1.2.0.8）都是 0 命中。这些接口全在它 dlopen 的插件里：

```
/usr/lib/libprod_mgmtd_inst.so.0.0.0     ← /action/bmc/* 与 /status/bmc/*
/usr/lib/libprod_cli_inst.so.0.0.0       ← CLI 侧同一套
```

对比结果：**我们 73 条，原厂 73 条，`comm` 双向都为空**。包括：

```
/action/bmc/power/control
/action/bmc/usb-cdrom/attach-local|attach-remote|detach|remove-local-isofile
/action/bmc/bios/upgrade|upgrade/all|upgrade/image|backup|restore
/action/bmc/sel/clear
/action/bmc/images/remove-local-image
/action/bmc/phonehome/enabled
/status/bmc/power/state|action
/status/bmc/usb-cdrom/enabled|image|location
/status/bmc/console/cable-connected
/status/bmc/health/{fan,temperature,voltage,power_supply}/...
/status/bmc/info/{device_name,device_sn,fw_version,hw_version,sw_version,uboot_version,...}
```

插件再去调 `ubmc_sys_ctrl.sh <act>` 和 `/usr/sbin/ubmc_usb_cdrom_ctrl.sh`。

所以「我们的固件缺 IPMI 功能」这个担心可以排除 —— 能力面与原厂一致。

## 登录与账号（踩过的坑）

### is_admin 的密码在 /etc/shadow 里

Web 登录校验读的是 **`/etc/shadow`**（不是插件里内嵌的哈希）。实测把
`/etc/shadow` 里 `is_admin` 那行换成一个已知哈希后，用该密码登录立刻
成功并下发 `sysauth` cookie。

生成哈希（任意 Linux）：

```sh
python3 -c "import crypt; print(crypt.crypt('<password>','\$6\$v150adm\$'))"
```

替换时要避开 shell 展开（`$6$...` 会被吃掉），用 awk 整行替换：

```sh
printf '%s\n' 'is_admin:$6$v150adm$...:20774:0:99999:7:::' > /tmp/newline.txt
awk 'BEGIN{while((getline l<"/tmp/newline.txt")>0) nl=l} /^is_admin:/{print nl;next}{print}' \
    /etc/shadow > /tmp/shadow.new && cp /tmp/shadow.new /etc/shadow && chmod 600 /etc/shadow
```

### privilege 在 mgmtd 配置里，不在 shadow

```sh
/usr/bin/lua -e 'package.cpath="/opt/silc-web/host/usr/lib/lua/?.so;"..package.cpath
local c=require"mgmtdclient"; c.set_login("root",2,"local")
print(c.query("/config/unix/user/is_admin/privilege"))'
--> OK  3
```

`dispatcher.lua` 里 `PRIVILEGE_ADMIN = 2`，`check_normal()` 要求
`user_privil >= 2`。`is_admin` 是 3，够用；**root 是 0，所以 root 打不开
Console / Power / USB 这些页面**（但 RPC `/rpc/sys` 不受限，仍可提权）。

### LuCI 的 stok 在 URL 路径里

```
对： /cgi-bin/sweb/;stok=<32位hex>/admin/bmc/shell
错： /cgi-bin/sweb/admin/bmc/shell?stok=<32位hex>
```

### /etc/config/luci 缺失会导致所有 admin 页面打不开

详见 [webtty-diagnosis.md](./webtty-diagnosis.md) 的「第二个坑」一节。
`S46is_web_fw` 会在启动时兜底补齐。

## 硬件侧看到的东西

```
/sys/class/udc/musb-hdrc.1           USB device controller（虚拟光驱靠它）
/sys/kernel/config/                  空 —— 没走 configfs，是 legacy gadget
/dev/bmc_drv                         ubmc_ipmi 的硬件接口
/dev/i2c-0  /dev/i2c-3               电源/风扇/温度传感器
/dev/watchdog /dev/watchdog0
GPIO（已导出 12 个）                  23 26 27 44 45 47 55 61 65 66 68 86
                                     gpio27 = host SPI flash mux
i2c_gpio + gpio_pca953x               GPIO 扩展器
peci（device tree 里有节点）           Intel CPU 侧通道
```

`ubmc_ipmi` 里有 `ipmi_host_power_monitor_thread` / `ipmi_get_host_power_state`
/ `ubmc_ipmi_poll_gpio_events`，主机电源状态是它通过 GPIO 轮询出来的。

它对接主机的是 `ubmc_ipmi_slave.ko`（KCS 侧接口），所以 **原厂 IPMI 只在
主机内部可用**（主机 OS 里 `ipmitool` 能通），**网络上没有 IPMI**。

## IPMI over LAN：已补齐（自建服务端）

原厂没有 udp/623 监听者，第五~七轮自己实现了完整的一套：

```
usr/sbin/ubmc_ipmi_lan       IPMI 2.0 RMCP+ 服务端，cipher suite 3 / 17，静态链接
/config/firmware/ubmc_ipmi_lan  优先跑的那一份（ramdisk 根 fs，/usr 重启即还原）
etc/init.d/S47is_ipmi_lan    开机启动 + 守护重启 + 放行 udp/623
/config/ipmi-lan.passwd      独立口令表（不能用 /etc/shadow，见下）
```

已实现的命令：

| NetFn | 命令 | 能力 |
|---|---|---|
| App | `0x01` `0x38` `0x3B` `0x3C` `0x3D` `0x42` | 设备信息、通道能力、特权级、关会话、通道信息 |
| App | `0x40` `0x41` `0x43`-`0x47` | **用户管理**：`user list` / `set name` / `set password` / `set privilege` / `enable` |
| Transport | `0x02` | LAN 参数（IP/掩码/MAC/网关/VLAN/community/auth type/ARP 控制/cipher suites）|
| Chassis | `0x00` `0x01` `0x02` `0x08` `0x09` | 能力/状态、电源控制、boot flags 与 bootdev |
| Storage | `0x10` `0x11` | FRU（读 EEPROM TlvInfo，拼 Board + Product 区）|
| Storage | `0x20`-`0x23` `0x40` `0x42` `0x43` `0x47`-`0x49` | SDR 仓库、SEL 读写/清空、SEL 时间 |
| Storage | `0x24`-`0x2C` | **用户 SDR 仓库写入**：Add / Partial Add / Delete / Clear / Reserve |
| PEF (0x04) | `0x10`-`0x15` | PEF 配置面（控制、策略表、策略号表、事件过滤器表）|
| PEF (0x04) | `0x16` | **Alert Immediate**（发一条测试 PET）|
| Sensor | `0x2D` `0x23` | 传感器读数、Reading Factors |
| App | `0x48`-`0x4B` + payload type 1 | **SOL**（含双向 BREAK、ACK 流控）|
| RMCP class 6 | ASF presence ping → pong | 部分客户端握手前置 |

加密同时支持 **cipher suite 3**（HMAC-SHA1）和 **suite 17**（HMAC-SHA256），
两个客户端都不需要额外参数。

电源命令映射到 `ubmc_sys_ctrl.sh -q/-f/-o/-c/-r/-s`；SEL 读的是厂商格式
`/var/log/sel`（20 字节 × 200 槽循环缓冲）；传感器走 hwmon（`peci_cputemp`、
`silc_fan`、`ads7830`）；FRU 读 i2c `0-0050` 的 ONIE TlvInfo EEPROM。

> **`sel info` 显示 `Free Space: 0 bytes / Percent Used: 100%` 是正常的** ——
> 厂商的 SEL 是**预分配**的定长槽文件（20 字节 × 200 槽），不是按需增长。
> 槽用满后会回收最老的槽（这就是第二十五轮修"记录 ID 回绕导致客户端死循环"的来源）。
> 判断"有没有新事件"要看 `Last Add Time`，不是看空闲空间。

口令为什么不能沿用 shadow：RAKP2/RAKP3 的认证码是**以明文口令为 HMAC 密钥**
算的，单向哈希算不出来。真实 BMC 也都是单独存一份 IPMI 口令。

实测（pyghmi 与 python-ipmi 两个独立客户端交叉验证，镜像里那颗 strip 后的二进制）：

```
python-ipmi: bmc info / chassis status / sdr list / fru print / sel list / lan print  全部 rc=0
pyghmi    : 电源、SEL+传感器+FRU+bootdev、SOL、SOL 流控+BREAK 四套验收全绿
GET /ws   : 101（SOL 与 Web Console 并存）
```

完整协议细节、踩坑与已知缺口见 [ipmi-lan.md](./ipmi-lan.md) 和
[sol-design.md](./sol-design.md)。

**还没实现**（只有一项，而且是不该做的）：IPMB 桥接 —— 板上 i2c 没有 `0x20` 类
从机地址，没有可桥对象，NetFn `0x2C` 回 `0xC1`（这正是"没有桥的 BMC"的标准应答）。

第二十五轮之后，下面这些都已经可用：Set/Get LAN Configuration Parameters
（含 VLAN / community / auth type / ARP 控制）、Set/Get User Access、
PEF 配置面 + 告警目的地 + PET/syslog 发送器、SDR 仓库写入。详见
[ipmi-lan.md](./ipmi-lan.md)。

## 建议的使用方式

日常最省事的组合：

1. 浏览器开 `https://<bmc>/`，用 `is_admin` 登录
2. `System > BMC > Console Shell` 看主机画面、敲键盘
3. `System > BMC > Power Control` 开关机
4. `System > BMC > USB CDROM` 上传/选择 ISO 后 Attach，主机重启即可从光盘启动

命令行等价物（SSH 进去后）：

```sh
ubmc_sys_ctrl.sh -q                          # 看电源
cp /path/to/installer.iso /var/images/       # ISO 放这儿
ubmc_usb_cdrom_ctrl.sh -a /var/images/installer.iso
ubmc_sys_ctrl.sh -c                          # 电源循环，从光盘启动
ubmc_usb_cdrom_ctrl.sh -d                    # 用完卸载
```
