import os
import sys
import subprocess
import socket
import re
from pathlib import Path

# 获取项目根目录 (backend 目录)
ROOT_DIR = Path(__file__).resolve().parent

def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # 连接一个公网地址以获取本地真实出口 IP
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"

def get_current_port():
    try:
        import config
        return getattr(config, "FLASK_PORT", 8765)
    except:
        return 8765

def clear_screen():
    os.system('cls' if os.name == 'nt' else 'clear')

def print_header():
    clear_screen()
    local_ip = get_local_ip()
    port = get_current_port()
    
    print("=" * 60)
    print(" 📸 电子相册 - 统一控制中心".center(56))
    print("=" * 60)
    print(f"🌍 当前服务器网络: http://{local_ip}:{port}")
    print("👉 (请确保 ESP32 的 Photo Server URL 与上方地址完全一致)")
    print("-" * 60)
    print("请选择今天的照片更新工作流：\n")
    print("  [1] ☁️  云端模式：百度网盘增量拉取 -> 轻量级AI识别 -> 渲染 -> 启动服务")
    print("  [2] 💻 本地模式：扫描本地硬盘新图 -> 深度AI分析打分 -> 渲染 -> 启动服务")
    print("  [3] 🚀 直接启动：跳过所有分析和渲染，直接启动 Web 服务器")
    print("  [4] 🎨 仅重渲染：仅重新挑选并渲染今日照片 -> 启动服务")
    print("  [5] ⚙️  配置网络：修改端口号并自动同步到 ESP32 源码配置")
    print("\n  [0] 退出")
    print("=" * 60)

def configure_network():
    print("\n" + "=" * 50)
    print("⚙️  网络参数配置")
    print("=" * 50)
    local_ip = get_local_ip()
    current_port = get_current_port()
    
    print(f"电脑当前局域网 IP 为: {local_ip} (自动检测)")
    new_port = input(f"请输入新的端口号 (当前为 {current_port}，直接回车保持不变): ").strip()
    
    if new_port and new_port.isdigit():
        new_port_int = int(new_port)
    else:
        new_port_int = current_port
        
    server_url = f"http://{local_ip}:{new_port_int}"
    
    # 1. 更新 backend/config.py
    config_path = ROOT_DIR / "config.py"
    if config_path.exists():
        content = config_path.read_text(encoding="utf-8")
        if "FLASK_PORT =" in content:
            content = re.sub(r"FLASK_PORT\s*=\s*\d+", f"FLASK_PORT = {new_port_int}", content)
        else:
            content += f"\nFLASK_PORT = {new_port_int}\n"
        config_path.write_text(content, encoding="utf-8")
        print(f"✅ 后端端口已更新为: {new_port_int}")

    # 2. 更新 ESP32 sdkconfig
    sdkconfig_path = ROOT_DIR.parent / "sdkconfig"
    if sdkconfig_path.exists():
        content = sdkconfig_path.read_text(encoding="utf-8")
        if "CONFIG_SERVER_URL=" in content:
            content = re.sub(r'CONFIG_SERVER_URL="[^"]*"', f'CONFIG_SERVER_URL="{server_url}"', content)
        else:
            content += f'\nCONFIG_SERVER_URL="{server_url}"\n'
        sdkconfig_path.write_text(content, encoding="utf-8")
        print(f"✅ ESP32 配置文件 sdkconfig 已同步: {server_url}")
        print("⚠️  注意: 您需要进入终端运行 `idf.py build flash` 才能将新地址烧录到设备中！")
    else:
        print("⚠️ 未找到 sdkconfig 文件，请手动通过 idf.py menuconfig 修改 ESP32 的服务器地址。")
        
    input("\n按回车键返回主菜单...")

def run_script(script_path: str, description: str) -> bool:
    """阻塞运行一个 python 子脚本，并在异常时给出提示"""
    target = ROOT_DIR / script_path
    if not target.exists():
        print(f"\n❌ 错误: 找不到脚本 {target}")
        return False
        
    print(f"\n[{description}] 正在启动任务: {script_path} ...")
    print("-" * 50)
    
    try:
        result = subprocess.run([sys.executable, str(target)], cwd=ROOT_DIR)
        print("-" * 50)
        
        if result.returncode != 0:
            print(f"\n⚠️ 警告: [{description}] 任务未能结束 (退出码 {result.returncode})。")
            choice = input("是否忽略此错误并继续下一步？(y/N): ").strip().lower()
            if choice != 'y':
                return False
        else:
            print(f"\n✅ [{description}] 任务执行完成！")
        return True
        
    except KeyboardInterrupt:
        print(f"\n\n🛑 检测到您按下了 Ctrl+C 强制中断了 [{description}]。")
        choice = input("是否跳过剩余拉取，直接强制进行后续任务？(y/N): ").strip().lower()
        if choice == 'y':
            return True
        return False
    except Exception as e:
        print(f"\n❌ 发生严重异常: {e}")
        return False

def start_server():
    """启动 Flask 核心服务"""
    local_ip = get_local_ip()
    port = get_current_port()
    print(f"\n🚀 正在拉起相册主 Web 服务器 (server.py)...")
    print(f"🔗 预计设备连接地址: http://{local_ip}:{port}")
    print("💡 提示：按 Ctrl+C 可停止服务器并退出程序。")
    print("=" * 60 + "\n")
    try:
        subprocess.run([sys.executable, str(ROOT_DIR / "server.py")], cwd=ROOT_DIR)
    except KeyboardInterrupt:
        print("\n\n👋 服务器已关闭。")

def main():
    while True:
        print_header()
        try:
            choice = input("👉 请输入序号 (0-5): ").strip()
        except KeyboardInterrupt:
            print("\n👋 退出程序。")
            sys.exit(0)
            
        if choice == '1':
            if not run_script("tasks/baidu_sync_task.py", "云端同步"): continue
            if not run_script("tasks/render_daily.py", "每日渲染"): continue
            start_server()
            break
            
        elif choice == '2':
            if not run_script("tasks/cloud_analyze_photos.py", "本地深度分析"): continue
            if not run_script("tasks/render_daily.py", "每日渲染"): continue
            start_server()
            break
            
        elif choice == '3':
            start_server()
            break
            
        elif choice == '4':
            if not run_script("tasks/render_daily.py", "每日渲染"): continue
            start_server()
            break
            
        elif choice == '5':
            configure_network()
            
        elif choice == '0':
            print("\n👋 退出程序。")
            sys.exit(0)
        else:
            print("❌ 无效的输入，请重新选择。")
            import time
            time.sleep(1)

if __name__ == "__main__":
    main()

