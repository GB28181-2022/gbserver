#!/usr/bin/env python3
"""
SIP INVITE/ACK 自动测试脚本
测试流程：
1. 检查服务健康状态
2. 触发预览请求（发送 INVITE）
3. 等待设备响应 200 OK
4. 验证后端是否发送 ACK
5. 输出测试结果
"""

import subprocess
import json
import time
import sys
import re

# 配置
BASE_URL = "http://192.168.1.9:8080"
CAMERA_ID = "42000000111317000010"
PLATFORM_ID = "42000000112007000011"
LOG_FILE = "/tmp/gb_service.log"

# ANSI 颜色
GREEN = "\033[92m"
RED = "\033[91m"
YELLOW = "\033[93m"
RESET = "\033[0m"

def log(msg, level="INFO"):
    """输出日志"""
    color = GREEN if level == "PASS" else RED if level == "FAIL" else YELLOW if level == "WARN" else RESET
    print(f"{color}[{level}]{RESET} {msg}")

def check_health():
    """检查服务健康状态"""
    try:
        result = subprocess.run(
            ["curl", "-s", f"{BASE_URL}/api/health"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            try:
                data = json.loads(result.stdout)
                if data.get("code") == 0:
                    log("服务健康检查通过", "PASS")
                    return True
            except:
                pass
        log(f"服务健康检查失败: {result.stdout[:100]}", "FAIL")
        return False
    except Exception as e:
        log(f"健康检查异常: {e}", "FAIL")
        return False

def trigger_preview():
    """触发预览请求"""
    log("触发预览请求...")
    try:
        result = subprocess.run(
            [
                "curl", "-s", "-X", "POST",
                f"{BASE_URL}/api/cameras/{CAMERA_ID}/preview/start",
                "-H", "Content-Type: application/json",
                "-d", json.dumps({"cameraId": CAMERA_ID, "platformId": PLATFORM_ID})
            ],
            capture_output=True, text=True, timeout=10
        )
        
        if result.returncode != 0:
            log(f"预览请求失败: {result.stderr}", "FAIL")
            return None
            
        try:
            data = json.loads(result.stdout)
            if data.get("code") == 0:
                session_id = data.get("data", {}).get("sessionId")
                log(f"预览启动成功, sessionId: {session_id[:16]}..." if session_id else "预览启动成功", "PASS")
                return session_id
            else:
                log(f"预览启动失败: {data.get('message')}", "FAIL")
                return None
        except json.JSONDecodeError:
            log(f"解析响应失败: {result.stdout[:200]}", "FAIL")
            return None
    except Exception as e:
        log(f"预览请求异常: {e}", "FAIL")
        return None

def check_logs_for_invite():
    """检查日志中是否有 INVITE 发送记录"""
    time.sleep(2)  # 等待日志写入
    try:
        result = subprocess.run(
            ["tail", "-50", LOG_FILE],
            capture_output=True, text=True, timeout=5
        )
        
        if result.returncode != 0:
            return False
            
        log_content = result.stdout
        
        # 检查 INVITE 发送
        if "【sendPlayInvite】INVITE sent successfully" in log_content:
            log("检测到 INVITE 已发送", "PASS")
            return True
        else:
            log("未检测到 INVITE 发送记录", "WARN")
            return False
    except:
        return False

def check_logs_for_ack():
    """检查日志中是否有 ACK 发送记录"""
    time.sleep(3)  # 等待设备响应 200 OK 和 ACK 发送
    try:
        result = subprocess.run(
            ["tail", "-100", LOG_FILE],
            capture_output=True, text=True, timeout=5
        )
        
        if result.returncode != 0:
            return False
            
        log_content = result.stdout
        
        # 检查 INVITE 200 OK 接收
        invite_200ok = "【INVITE 200 OK】Received 200 OK for INVITE" in log_content
        
        # 检查 ACK 发送
        ack_sent = "【sendAckForInvite】ACK sent successfully" in log_content
        
        if invite_200ok and ack_sent:
            log("检测到 INVITE 200 OK 和 ACK 发送", "PASS")
            return True
        elif invite_200ok:
            log("检测到 INVITE 200 OK，但未检测到 ACK 发送", "WARN")
            return False
        else:
            log("未检测到 INVITE 200 OK 响应（设备可能未响应或日志尚未写入）", "WARN")
            return False
    except Exception as e:
        log(f"检查日志异常: {e}", "FAIL")
        return False

def stop_preview(session_id):
    """停止预览"""
    if not session_id:
        return
        
    log("停止预览...")
    try:
        subprocess.run(
            [
                "curl", "-s", "-X", "POST",
                f"{BASE_URL}/api/cameras/{CAMERA_ID}/preview/stop",
                "-H", "Content-Type: application/json",
                "-d", json.dumps({"sessionId": session_id})
            ],
            capture_output=True, text=True, timeout=5
        )
    except:
        pass

def check_invite_message_structure():
    """检查 INVITE 消息结构（通过日志）"""
    time.sleep(1)
    try:
        result = subprocess.run(
            ["grep", "【sendPlayInvite】SIP INVITE消息", LOG_FILE],
            capture_output=True, text=True, timeout=5
        )
        
        if result.returncode == 0 and result.stdout:
            # 提取 INVITE 消息
            lines = result.stdout.strip().split("\n")
            if lines:
                # 检查是否包含 Contact 头
                if "Contact:" in result.stdout or "m=" in result.stdout:
                    log("INVITE 消息包含 Contact 头或 SDP", "PASS")
                    return True
        
        log("需要检查 INVITE 消息是否包含 Contact 头", "WARN")
        return False
    except:
        return False

def main():
    """主测试流程"""
    print("=" * 60)
    print("SIP INVITE/ACK 自动测试")
    print("=" * 60)
    
    # 1. 健康检查
    print("\n[步骤 1] 服务健康检查")
    if not check_health():
        log("服务未运行，测试终止", "FAIL")
        sys.exit(1)
    
    # 2. 触发预览
    print("\n[步骤 2] 触发预览（发送 INVITE）")
    session_id = trigger_preview()
    if not session_id:
        log("预览启动失败，但继续检查日志", "WARN")
    
    # 3. 检查 INVITE 发送
    print("\n[步骤 3] 检查 INVITE 发送记录")
    check_logs_for_invite()
    
    # 4. 检查 INVITE 消息结构
    print("\n[步骤 4] 检查 INVITE 消息结构")
    check_invite_message_structure()
    
    # 5. 等待并检查 ACK
    print("\n[步骤 5] 检查 200 OK 和 ACK（等待 5 秒）")
    time.sleep(5)
    ack_ok = check_logs_for_ack()
    
    # 6. 停止预览
    print("\n[步骤 6] 清理测试会话")
    stop_preview(session_id)
    
    # 7. 输出测试摘要
    print("\n" + "=" * 60)
    print("测试摘要")
    print("=" * 60)
    
    if ack_ok:
        log("✓ 测试通过：INVITE -> 200 OK -> ACK 流程正常", "PASS")
        return 0
    else:
        log("✗ 测试失败或需要进一步验证", "WARN")
        log("请手动检查抓包是否看到 ACK 消息", "WARN")
        log("请检查后端日志: tail -f /tmp/gb_service.log | grep -E 'INVITE|ACK'", "INFO")
        return 1

if __name__ == "__main__":
    sys.exit(main())
