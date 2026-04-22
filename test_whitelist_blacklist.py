#!/usr/bin/env python3
"""
测试白名单和黑名单设备注册
"""

import socket
import uuid
import random

# 配置
LOCAL_IP = "192.168.1.9"
LOCAL_PORT = 5063  # 测试用端口
SERVER_IP = "192.168.1.9"
SERVER_PORT = 5060

# 测试设备ID
WHITELIST_DEVICE_ID = "340200000030008373"  # 白名单设备
BLACKLIST_DEVICE_ID = "340200000030009340"  # 黑名单设备

def make_register_request(device_id, to_uri, from_uri, call_id, cseq, expires=3600, auth_header=None, server_ip=None, server_port=None):
    """生成 SIP REGISTER 请求"""
    via = f"SIP/2.0/UDP {LOCAL_IP}:{LOCAL_PORT};rport;branch=z9hG4bK{random.randint(100000, 999999)}"
    from_hdr = f"<sip:{device_id}@{SERVER_IP}:{SERVER_PORT}>"
    to_hdr = f"<sip:{device_id}@{SERVER_IP}:{SERVER_PORT}>"
    contact = f"<sip:{device_id}@{LOCAL_IP}:{LOCAL_PORT}>"
    
    # 使用正确的Request-URI格式，包含设备ID
    request_uri = f"sip:{device_id}@{SERVER_IP}:{SERVER_PORT}"
    
    msg = f"""REGISTER {request_uri} SIP/2.0
Via: {via}
From: {from_hdr};tag={random.randint(100000, 999999)}
To: {to_hdr}
Call-ID: {call_id}
CSeq: {cseq} REGISTER
Contact: {contact}
Max-Forwards: 70
User-Agent: GB28181 Test Client
Expires: {expires}
Content-Length: 0
"""
    
    if auth_header:
        msg = msg.replace("Content-Length: 0", f"Authorization: {auth_header}\nContent-Length: 0")
    
    return msg

def test_device_register(device_id, device_type):
    """测试设备注册"""
    print(f"\n测试 {device_type} 设备注册")
    print("-" * 60)
    print(f"设备ID: {device_id}")
    
    # 创建 UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LOCAL_IP, LOCAL_PORT))
    sock.settimeout(10)
    
    server_addr = (SERVER_IP, SERVER_PORT)
    call_id = str(uuid.uuid4())
    cseq = 1
    
    # 发送不带鉴权信息的 REGISTER
    print(f"[步骤1] 发送不带鉴权信息的 REGISTER...")
    msg = make_register_request(
        device_id,
        f"sip:{device_id}@{SERVER_IP}:{SERVER_PORT}",
        f"sip:{device_id}@{LOCAL_IP}:{LOCAL_PORT}",
        call_id, cseq,
        server_ip=SERVER_IP,
        server_port=SERVER_PORT
    )
    sock.sendto(msg.encode('utf-8'), server_addr)
    
    # 等待响应
    try:
        data, addr = sock.recvfrom(4096)
        text = data.decode('utf-8', errors='ignore')
        
        if "SIP/2.0 200" in text:
            if device_type == "白名单":
                print("[✓] 收到 200 OK，白名单设备不需要鉴权，注册成功！")
                return True
            else:
                print("[✗] 收到 200 OK，黑名单设备应该被拒绝！")
                return False
        elif "SIP/2.0 403" in text:
            if device_type == "黑名单":
                print("[✓] 收到 403 Forbidden，黑名单设备被正确拒绝！")
                return True
            else:
                print("[✗] 收到 403 Forbidden，白名单设备应该被允许！")
                return False
        elif "SIP/2.0 401" in text:
            print("[?] 收到 401 Unauthorized，要求鉴权")
            # 对于白名单设备，不应该要求鉴权
            if device_type == "白名单":
                print("[✗] 白名单设备不应该要求鉴权！")
                return False
            else:
                print("[?] 黑名单设备收到 401，继续测试...")
                # 尝试带鉴权注册
                cseq += 1
                # 简单构建一个鉴权头
                auth_header = 'Digest username="test", realm="gb_service", nonce="test", uri="sip:test", response="test"'
                msg = make_register_request(
                    device_id,
                    f"sip:{device_id}@{SERVER_IP}:{SERVER_PORT}",
                    f"sip:{device_id}@{LOCAL_IP}:{LOCAL_PORT}",
                    call_id, cseq, auth_header=auth_header
                )
                sock.sendto(msg.encode('utf-8'), server_addr)
                
                try:
                    data, addr = sock.recvfrom(4096)
                    text = data.decode('utf-8', errors='ignore')
                    if "SIP/2.0 403" in text:
                        print("[✓] 收到 403 Forbidden，黑名单设备被正确拒绝！")
                        return True
                    else:
                        print(f"[✗] 黑名单设备收到意外响应: {text[:100]}")
                        return False
                except socket.timeout:
                    print("[✗] 等待响应超时")
                    return False
        else:
            print(f"[?] 收到意外响应: {text[:100]}")
            return False
            
    except socket.timeout:
        print("[✗] 等待响应超时")
        return False
    finally:
        sock.close()

def main():
    print("=" * 60)
    print("测试白名单和黑名单设备注册")
    print("=" * 60)
    
    # 测试白名单设备
    whitelist_result = test_device_register(WHITELIST_DEVICE_ID, "白名单")
    
    # 测试黑名单设备
    blacklist_result = test_device_register(BLACKLIST_DEVICE_ID, "黑名单")
    
    print("\n" + "=" * 60)
    print("测试结果汇总")
    print("=" * 60)
    print(f"白名单设备测试: {'通过' if whitelist_result else '失败'}")
    print(f"黑名单设备测试: {'通过' if blacklist_result else '失败'}")
    
    if whitelist_result and blacklist_result:
        print("\n[✓] 所有测试通过！")
    else:
        print("\n[✗] 部分测试失败！")

if __name__ == "__main__":
    main()
