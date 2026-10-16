#!/usr/bin/env python3
"""
串口冒烟回归 —— 每次固件更新后运行，自动验证核心链路。

用法:
    python3 scripts/serial_smoke.py                     # 全量（含配网流程，会重写 Wi-Fi）
    python3 scripts/serial_smoke.py --wifi "SSID" "PASS"  # 指定用于恢复的 Wi-Fi
    python3 scripts/serial_smoke.py --quick             # 跳过 prov/配网（只测状态+同步）

依赖: pyserial（ESP-IDF 的 python 环境已自带）
退出码: 0 全部通过，1 有失败
"""
import argparse
import re
import sys
import time

import serial

PORT = "/dev/ttyACM0"
BAUD = 115200


class Dev:
    def __init__(self):
        self.s = serial.Serial()
        self.s.port = PORT
        self.s.baudrate = BAUD
        self.s.timeout = 0.2
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        self.buf = ""

    def close(self):
        self.s.close()

    def reset(self, wait=8.0):
        """RTS 脉冲复位；清除缓冲后持续读取（USB-JTAG FIFO 不读会丢日志）"""
        self.s.rts = True
        time.sleep(0.15)
        self.s.rts = False
        self.buf = ""
        end = time.time() + wait
        while time.time() < end:
            self.read(0.3)

    def console_ready(self, timeout=15.0):
        """敲回车确认 console 就绪"""
        end = time.time() + timeout
        while time.time() < end:
            self.buf = ""
            self.s.write(b"\n")
            if self.wait_for(r"whisport>", timeout=1.5):
                return True
        return False

    def read(self, secs=5.0):
        end = time.time() + secs
        while time.time() < end:
            d = self.s.read(4096)
            if d:
                self.buf += d.decode("utf-8", "replace")
        return self.buf

    def wait_for(self, pattern, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            self.read(0.4)
            m = re.search(pattern, self.buf)
            if m:
                return m
        return None

    def send(self, line):
        self.s.write((line + "\n").encode())
        self.buf = ""


passed, failed = 0, 0
ip_addr = None
pin_code = None


def check(name, cond, extra=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ✓ {name}")
    else:
        failed += 1
        print(f"  ✗ {name} {extra}")


def run_portal_checks(dev):
    """局域网配网页接口（与 AP 模式同一套 handler）"""
    import json
    import urllib.request

    # 实时取 PIN（设备可能已多次重启，日志中的旧 PIN 不可信）
    dev.buf = ""
    dev.send("pin")
    m = re.search(r"配网 PIN: (\d{4})", dev.read(3))
    pin = m.group(1) if m else None
    if pin:
        print(f"    当前 PIN = {pin}")

    base = f"http://{ip_addr}"

    def req(path, method="GET", body=None):
        data = json.dumps(body).encode() if body is not None else None
        r = urllib.request.Request(base + path, data=data, method=method)
        if data:
            r.add_header("content-type", "application/json")
        with urllib.request.urlopen(r, timeout=8) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")

    try:
        st, html = req("/")
        check("配网页可访问", st == 200 and "设备配置" in html, html[:120])
        st, body = req("/api/status")
        d = json.loads(body)
        check("状态接口", st == 200 and d.get("device_id"), body[:120])
        st, body = req("/api/name", "POST", {"label": "x", "pin": "0000"})
        check("PIN 校验拦截错误 PIN", '"ok":false' in body, body[:120])
        if pin:
            st, body = req("/api/name", "POST", {"label": d.get("label") or "AI Passport", "pin": pin})
            check("正确 PIN 改名成功", '"ok":true' in body, body[:120])
        st, body = req("/api/scan")
        check("Wi-Fi 扫描接口", st == 200 and '"aps"' in body, body[:160])
    except Exception as e:  # noqa: BLE001
        check("配网页接口可用", False, str(e))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wifi", nargs=2, metavar=("SSID", "PASS"), help="测试后恢复用 Wi-Fi")
    ap.add_argument("--quick", action="store_true", help="跳过配网全流程")
    args = ap.parse_args()

    dev = Dev()
    try:
        print("== 1. 启动与状态 ==")
        dev.reset(wait=8)
        log = dev.read(4)
        check("固件启动（whisport 横幅）", "whisport M1" in log, log[-160:])
        check("Wi-Fi 驱动启动", "sta started" in log or "ap started" in log or "got ip" in log)
        check("配置台就绪", "config portal ready" in log)
        m = re.search(r"pin=(\d{4})", log)
        if m:
            globals()["pin_code"] = m.group(1)
            print(f"    配网 PIN = {pin_code}")

        dev.console_ready()
        dev.send("status")
        st = dev.read(3)
        m = re.search(r"device_id : ([0-9a-f]{32})", st)
        check("console status 可用", m is not None, st[-120:])
        if m:
            print(f"    device_id = {m.group(1)}")

        if not args.quick:
            print("== 2. 配网流程（prov → AP 模式）==")
            dev.buf = ""
            dev.send("prov")
            t_end = time.time() + 10  # 清凭据并重启；持续读取避免 FIFO 丢日志
            while time.time() < t_end:
                dev.read(0.3)
            m = re.search(r"pin=(\d{4})", dev.buf)
            if m:  # PIN 每次开机重新生成，重启后需重抓
                globals()["pin_code"] = m.group(1)
                print(f"    重启后 PIN = {pin_code}")
            dev.console_ready()
            dev.send("status")
            st = dev.read(3)
            check("重启后进入 AP 配网模式", "AP 模式" in st, st[-200:])
            if "AP 模式" in st:
                dev.send("__apinfo__")  # 触发日志刷新（无此命令则忽略）
                dev.read(1)
                m = dev.wait_for(r"provisioning ap (whisport-\w{4})", timeout=3)
                if m:
                    print(f"    AP SSID = {m.group(1)}")

            print("== 3. 恢复 Wi-Fi（命令行 wifi 命令）==")
            if args.wifi:
                dev.send(f"wifi {args.wifi[0]} {args.wifi[1]}")
                dev.read(2)
                m = dev.wait_for(r"got ip (\d+\.\d+\.\d+\.\d+)", timeout=40)
                check("STA 重新联网", m is not None)
                if m:
                    globals()["ip_addr"] = m.group(1)
                    print(f"    ip = {ip_addr}")
            else:
                print("    (未提供 --wifi，跳过恢复；设备停留在 AP 模式)")

        print("== 4. 公告同步 ==")
        dev.send("sync")
        out = dev.read(12)
        ok = re.search(r"api: GET .*announcements.*-> 200", out)
        check("签名同步 200", ok is not None, out[-160:])
        check("签名/时钟正常（无 401）", "401" not in out)

        print("== 5. 按键/背光任务存活 ==")
        check("无 panic/看门狗", not re.search(r"Guru Meditation|abort\(\)|Task watchdog", dev.buf + out))

        if ip_addr:
            print("== 6. 局域网配网页接口 ==")
            run_portal_checks(dev)

        print("== 7. UI 导航与屏保（key 注入）==")
        dev.buf = ""

        def uitree():
            dev.buf = ""
            dev.send("uitree")
            out = dev.read(2.5)
            m = re.search(r"screen=(\w+) splash=(\d)\r?\n(.*)", out, re.S)
            return m

        def key(seq, wait=1.2):
            for k in seq:
                dev.send(f"key {k}")
                time.sleep(wait)

        m = uitree()
        check("启动后为主菜单", m and m.group(1) == "MENU", m.group(0)[:80] if m else "no uitree output")
        key(["down", "ok"])            # 主菜单 → 公告列表
        m = uitree()
        check("进入公告列表", m and m.group(1) == "ANN", m.group(1) if m else "?")
        if m and m.group(1) == "ANN":
            key(["ok"])                # 打开详情
            m = uitree()
            check("打开公告详情", m and m.group(1) == "DETAIL", m.group(1) if m else "?")
            key(["ok"])                # 返回列表
            key(["double"])            # 回主菜单
            m = uitree()
            check("返回主菜单", m and m.group(1) == "MENU", m.group(1) if m else "?")
        check("菜单文案完整（无缺字）", m and "Announcements" in m.group(3), "")

        print("    等 65s 触发屏保…")
        time.sleep(65)
        m = uitree()
        check("空闲后进入屏保（像素启动画面）", m and m.group(1) == "SPLASH" or (m and "splash=1" in m.group(0)),
              m.group(0)[:60] if m else "?")
        dev.send("key ok")
        time.sleep(1.5)
        m = uitree()
        check("按键退出屏保", m and "splash=0" in m.group(0), m.group(0)[:60] if m else "?")
    finally:
        dev.close()

    print(f"\n结果: {passed} 通过, {failed} 失败")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
