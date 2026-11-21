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
| **IPMI over LAN**（`ipmitool -I lanplus`） | ❌ | — | 无 UDP 623 监听 |

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

它对接主机的是 `ubmc_ipmi_slave.ko`（KCS 侧接口），所以 **IPMI 目前只在
主机内部可用**（主机 OS 里 `ipmitool` 能通），**网络上没有 IPMI**。

## 仍然缺的：IPMI over LAN

```
ss -uln   → 只有 0.0.0.0:68（DHCP client）
```

没有 623/udp，也没有 `ipmitool` 二进制。要做到 `ipmitool -I lanplus -H <bmc> -U admin -P xxx chassis power on`，
需要额外实现：

- IPMI 2.0 RMCP+ 的 UDP 623 服务端（会话、认证、加密）
- 把 NetFn Chassis / Sensor / SEL / FRU 命令映射到现有的
  `ubmc_sys_ctrl.sh` 和 `/status/bmc/*`

这是一个独立工程量，不是配置问题。**但用户列的需求（看输出、发按键、
开关机重启、挂 ISO）都不依赖它** —— Web UI 已经全覆盖。

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
