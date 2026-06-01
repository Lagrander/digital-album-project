import subprocess
import re
import sys
from pathlib import Path


def get_real_wifi_ip():
    try:
        # 运行 Windows ipconfig 并以 gbk 解码
        res = subprocess.run("ipconfig", shell=True, capture_output=True, text=True, encoding="gbk", errors="ignore")
        # 匹配 "IPv4 地址" 并抓取其后面的 IP
        ips = re.findall(r"IPv4[^\d\n]+([0-9\.]+)", res.stdout)
        
        # 优先提取 192.168.x.x 或者是 10.x.x.x, 172.x.x.x 等真实的无线/有线物理局域网网段
        real_ips = [ip for ip in ips if ip.startswith("192.168") or ip.startswith("10.") or ip.startswith("172.")]
        if real_ips:
            return real_ips[0]
        # 如果没有，兜底选择第一个非 127.0.0.1 的 IP
        other_ips = [ip for ip in ips if ip != "127.0.0.1" and not ip.startswith("198.18")]
        if other_ips:
            return other_ips[0]
    except Exception as e:
        print(f"[WARN] ipconfig parse failed: {e}")
    return None


def update_config_file(file_path, new_ip):
    p = Path(file_path)
    if not p.exists():
        return False
    
    content = p.read_text(encoding='utf-8', errors='ignore')
    
    # 替换 CONFIG_SERVER_URL 为最新真实的局域网 IP
    pattern = r'(CONFIG_SERVER_URL=)\"http://[^:]+:8765\"'
    replacement = f'\1"http://{new_ip}:8765"'
    new_content, count = re.subn(pattern, replacement, content)

    # 替换语音助手的 ws 接口 CONFIG_VA_WS_URI 
    pattern_va = r'(CONFIG_VA_WS_URI=)\"ws://[^:]+:8888\"'
    replacement_va = f'\1"ws://{new_ip}:8888"'
    new_content, count_va = re.subn(pattern_va, replacement_va, new_content)
    
    if count > 0 or count_va > 0:
        p.write_text(new_content, encoding='utf-8')
        print(f"[OK] Updated {p.name} -> Server IP aligned to {new_ip}")
        return True
    return False


def main():
    lan_ip = get_real_wifi_ip()
    if not lan_ip:
        print("[ERROR] 无法自动获取到局域网 IP，可能是未连接 Wi-Fi，请检查连接状态。")
        sys.exit(1)
    
    print(f"[INFO] 过滤掉 TUN/VPN 虚拟网卡，自动挑选物理网卡 IP: {lan_ip}")
    
    workspace_root = Path(r"w:\\Desktop\\digital_album_project")
    sdk_path = workspace_root / "sdkconfig"
    defaults_path = workspace_root / "sdkconfig.defaults"
    
    updated_any = False
    if update_config_file(sdk_path, lan_ip):
        updated_any = True
    if update_config_file(defaults_path, lan_ip):
        updated_any = True
        
    if updated_any:
        # 用纯 GBK 兼容字符安全输出，防范 Windows 控制台 Unicode 编码崩溃
        print("\n" + "="*50)
        print(" [OK] LAN IP successfully synced and aligned!")
        print(f" Target Server URL: http://{lan_ip}:8765")
        print("="*50)
        print(" Please run the following command in your terminal to re-flash:")
        print("   idf.py build flash")
        print("="*50 + "\n")
    else:
        print(f"[INFO] IP is already {lan_ip}, no changes needed.")


if __name__ == "__main__":
    main()
