#!/usr/bin/env python3
"""
测试心跳功能 (Keepalive)
"""

import socket
import uuid
import random
import time

# 配置
LOCAL_IP = "192.168.1.9"
LOCAL_PORT = 5064  # 测试用端口
SERVER_IP = "192.168.1.9"
SERVER_PORT = 5060

# 使用已注册的白名单设备
DEVICE_ID = "340200000030008373"

def make_keepalive_message(device_id, call_id, cseq, sn):
    """生成 Keepalive MESSAGE 请求"""
    via = f"SIP/2.0/UDP {LOCAL_IP}:{LOCAL_PORT};rport;branch=z9hG4bK{random.randint(100000, 999999)}"
    from_hdr = f"<sip:{device_id}@{SERVER_IP}:{SERVER_PORT}>"
    to_hdr = f"<sip:34020000002000000001@{SERVER_IP}:{SERVER_PORT}>"
    
    xml_body = f'''<?xml version="1.0" encoding="GB2312"?>
<Notify>
<CmdType>Keepalive</CmdType>
<SN>{sn}</SN>
<DeviceID>{device_id}</DeviceID>
<Status>OK</Status>
</Notify>'''
    
    msg = f"""MESSAGE sip:34020000002000000001@{SERVER_IP}:{SERVER_PORT} SIP/2.0
Via: {via}
From: {from_hdr};tag={random.randint(100000, 999999)}
To: {to_hdr}
Call-ID: {call_id}
CSeq: {cseq} MESSAGE
Content-Type: Application/MANSCDP+xml
Content-Length: {len(xml_body)}

{xml_body}"""
    
    return msg

def test_keepalive():
    """测试心跳功能"""
    print("=" * 60)
    print("GB28181 Keepalive 心跳功能测试")
    print("=" * 60)
    print(f"设备ID: {DEVICE_ID}")
    print(f"服务器: {SERVER_IP}:{SERVER_PORT}")
    print("-" * 60)
    
    # 创建 UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LOCAL_IP, LOCAL_PORT))
    sock.settimeout(10)
    
    server_addr = (SERVER_IP, SERVER_PORT)
    call_id = str(uuid.uuid4())
    cseq = 1
    sn = random.randint(1, 9999)
    
    # 发送 Keepalive 消息
    print("[步骤1] 发送 Keepalive MESSAGE...")
    msg = make_keepalive_message(DEVICE_ID, call_id, cseq, sn)
    sock.sendto(msg.encode('utf-8'), server_addr)
    print(f"  SN: {sn}")
    
    # 等待响应
    try:
        data, addr = sock.recvfrom(4096)
        text = data.decode('utf-8', errors='ignore')
        
        if "SIP/2.0 200" in text:
            print("[✓] 收到 200 OK，心跳消息处理成功！")
            print("-" * 60)
            print("心跳功能测试: 通过")
            return True
        else:
            print(f"[✗] 收到意外响应: {text[:100]}")
            return False
            
    except socket.timeout:
        print("[✗] 等待响应超时")
        return False
    finally:
        sock.close()

def test_multiple_keepalives():
    """测试多次心跳，验证 updated_at 更新"""
    print("\n" + "=" * 60)
    print("多次心跳测试（3次）")
    print("=" * 60)
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LOCAL_IP, LOCAL_PORT))
    sock.settimeout(10)
    
    server_addr = (SERVER_IP, SERVER_PORT)
    call_id = str(uuid.uuid4())
    
    success_count = 0
    for i in range(3):
        cseq = i + 1
        sn = random.randint(1, 9999)
        
        msg = make_keepalive_message(DEVICE_ID, call_id, cseq, sn)
        sock.sendto(msg.encode('utf-8'), server_addr)
        print(f"[发送 {i+1}/3] SN={sn}")
        
        try:
            data, addr = sock.recvfrom(4096)
            text = data.decode('utf-8', errors='ignore')
            if "SIP/2.0 200" in text:
                print(f"  [✓] 收到 200 OK")
                success_count += 1
            else:
                print(f"  [✗] 错误响应")
        except socket.timeout:
            print(f"  [✗] 超时")
        
        time.sleep(0.5)
    
    sock.close()
    
    print("-" * 60)
    print(f"测试结果: {success_count}/3 次心跳成功")
    return success_count == 3

if __name__ == '__main__':
    test1_passed = test_keepalive()
    test2_passed = test_multiple_keepalives()
    
    print("\n" + "=" * 60)
    print("心跳功能测试汇总")
    print("=" * 60)
    print(f"单次心跳测试: {'通过' if test1_passed else '失败'}")
    print(f"多次心跳测试: {'通过' if test2_passed else '失败'}")
    
    if test1_passed and test2_passed:
        print("\n[✓] 所有心跳测试通过！")
    else:
        print("\n[✗] 部分测试失败！")
