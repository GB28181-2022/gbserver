#!/usr/bin/env python3
"""
测试修复后的注册功能 - 验证设备ID从From头正确提取
场景：
- Request-URI: sip:34020000002000000001@192.168.1.9:5060 (服务器ID)
- From: sip:42000000112007000011@192.168.1.133 (真实设备ID)
- Contact: sip:42000000112007000011@192.168.1.133:5060

期望：数据库中保存的设备ID应该是 42000000112007000011，而不是 34020000002000000001
"""

import socket
import uuid
import random
import hashlib
import re
import time

# 配置
LOCAL_IP = "192.168.1.9"  # 使用本地IP测试
LOCAL_PORT = 5072
SERVER_IP = "192.168.1.9"
SERVER_PORT = 5060

# 模拟用户的真实场景
DEVICE_ID = "42000000112007000011"  # From/Contact 中的真实设备ID
SERVER_ID = "34020000002000000001"  # Request-URI 中的服务器ID
PASSWORD = "admin"
REALM = "gb_service"

def make_register_request(device_id, server_id, contact_ip, contact_port, 
                          call_id, cseq, expires=3600, auth_header=None):
    """生成 SIP REGISTER 请求
    
    Args:
        device_id: From/Contact 中的真实设备ID
        server_id: Request-URI 中的服务器ID（目标）
    """
    via = f"SIP/2.0/UDP {contact_ip}:{contact_port};rport;branch=z9hG4bK{random.randint(100000, 999999)}"
    from_hdr = f"<sip:{device_id}@{contact_ip}>"  # From 使用真实设备ID
    to_hdr = f"<sip:{server_id}@{SERVER_IP}:{SERVER_PORT}>"  # To 使用服务器ID
    contact = f"<sip:{device_id}@{contact_ip}:{contact_port}>"  # Contact 使用真实设备ID
    # Request-URI 指向服务器
    request_uri = f"sip:{server_id}@{SERVER_IP}:{SERVER_PORT}"
    
    msg = f"""REGISTER {request_uri} SIP/2.0
Via: {via}
From: {from_hdr};tag={random.randint(100000, 999999)}
To: {to_hdr}
Call-ID: {call_id}
CSeq: {cseq} REGISTER
Contact: {contact}
Max-Forwards: 70
User-Agent: GBuser/v3
Expires: {expires}
Content-Length: 0
"""
    
    if auth_header:
        msg = msg.replace("Content-Length: 0", 
                         f"Authorization: {auth_header}\nContent-Length: 0")
    
    return msg

def parse_www_authenticate(response):
    """解析 WWW-Authenticate 头"""
    realm = None
    nonce = None
    
    realm_match = re.search(r'realm="([^"]+)"', response)
    if realm_match:
        realm = realm_match.group(1)
    
    nonce_match = re.search(r'nonce="([^"]+)"', response)
    if nonce_match:
        nonce = nonce_match.group(1)
    
    return realm, nonce

def compute_digest_response(username, realm, password, method, uri, nonce):
    """计算 Digest 响应值"""
    ha1 = hashlib.md5(f"{username}:{realm}:{password}".encode()).hexdigest()
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    response = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()
    return response

def query_database(device_id):
    """查询数据库"""
    import subprocess
    result = subprocess.run([
        'psql', '-U', 'user', '-d', 'gb28181', '-c',
        f"SELECT gb_id, name, contact_ip, contact_port, online, list_type FROM device_platforms WHERE gb_id = '{device_id}';"
    ], capture_output=True, text=True)
    return result.stdout

def test_register_correct_id():
    """测试修复后的注册功能 - 验证正确的设备ID提取"""
    print("=" * 70)
    print("测试修复后的注册功能 - 验证设备ID从From头正确提取")
    print("=" * 70)
    print(f"Request-URI (目标服务器ID): {SERVER_ID}")
    print(f"From/Contact (真实设备ID): {DEVICE_ID}")
    print(f"Contact地址: {LOCAL_IP}:{LOCAL_PORT}")
    print("-" * 70)
    print("\n预期结果:")
    print("  - 数据库中保存的 gb_id 应该是: 42000000112007000011")
    print("  - 而不是 Request-URI 中的: 34020000002000000001")
    print("-" * 70)
    
    # 创建 UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LOCAL_IP, LOCAL_PORT))
    sock.settimeout(10)
    
    server_addr = (SERVER_IP, SERVER_PORT)
    call_id = str(uuid.uuid4())
    cseq = 1
    
    # 步骤1：发送不带鉴权的 REGISTER
    print("\n[步骤1] 发送 REGISTER（不带鉴权）...")
    print(f"  Request-URI: sip:{SERVER_ID}@{SERVER_IP}:{SERVER_PORT}")
    print(f"  From: sip:{DEVICE_ID}@{LOCAL_IP}")
    
    msg = make_register_request(DEVICE_ID, SERVER_ID, LOCAL_IP, LOCAL_PORT, call_id, cseq)
    sock.sendto(msg.encode('utf-8'), server_addr)
    
    try:
        data, addr = sock.recvfrom(4096)
        text = data.decode('utf-8', errors='ignore')
        
        if "SIP/2.0 401" in text:
            print("[✓] 收到 401 Unauthorized")
            
            realm, nonce = parse_www_authenticate(text)
            if realm and nonce:
                print(f"  realm: {realm}")
                print(f"  nonce: {nonce[:16]}...")
                
                # 步骤2：发送带鉴权的 REGISTER
                print("\n[步骤2] 发送带鉴权的 REGISTER...")
                cseq += 1
                
                # 注意：Authorization 中的 username 应该是真实设备ID
                uri = f"sip:{SERVER_ID}@{SERVER_IP}:{SERVER_PORT}"
                response = compute_digest_response(DEVICE_ID, realm, PASSWORD, "REGISTER", uri, nonce)
                
                auth_header = (f'Digest username="{DEVICE_ID}", '
                              f'realm="{realm}", '
                              f'nonce="{nonce}", '
                              f'uri="{uri}", '
                              f'response="{response}"')
                
                msg = make_register_request(DEVICE_ID, SERVER_ID, LOCAL_IP, LOCAL_PORT, 
                                         call_id, cseq, expires=3600, auth_header=auth_header)
                sock.sendto(msg.encode('utf-8'), server_addr)
                
                try:
                    data, addr = sock.recvfrom(4096)
                    text = data.decode('utf-8', errors='ignore')
                    
                    if "SIP/2.0 200" in text:
                        print("[✓] 收到 200 OK，注册成功！")
                    elif "SIP/2.0 401" in text:
                        print("[✗] 鉴权失败，收到 401")
                        print(f"  响应: {text[:300]}")
                    elif "SIP/2.0 403" in text:
                        print("[✗] 收到 403 Forbidden")
                    else:
                        print(f"[?] 收到响应: {text[:200]}")
                        
                except socket.timeout:
                    print("[✗] 等待响应超时")
            else:
                print("[✗] 无法解析 WWW-Authenticate")
        elif "SIP/2.0 200" in text:
            print("[✓] 直接收到 200 OK")
        elif "SIP/2.0 403" in text:
            print("[✗] 收到 403 Forbidden")
        else:
            print(f"[?] 收到响应: {text[:200]}")
            
    except socket.timeout:
        print("[✗] 等待响应超时")
    finally:
        sock.close()
    
    # 等待数据库写入
    print("\n[等待] 等待数据库写入...")
    time.sleep(1)
    
    # 验证数据库 - 查询真实设备ID
    print("\n[验证1] 查询数据库（使用真实设备ID）...")
    result = query_database(DEVICE_ID)
    print(result)
    
    # 验证数据库 - 查询服务器ID（应该不存在或不是刚插入的）
    print("\n[验证2] 查询数据库（使用服务器ID，应该不存在）...")
    result2 = query_database(SERVER_ID)
    print(result2)
    
    # 检查结果
    if DEVICE_ID in result and "1 row" in result:
        print("\n" + "=" * 70)
        print("[✓] 测试通过！设备ID已正确保存到数据库")
        print(f"  保存的设备ID: {DEVICE_ID}")
        print("=" * 70)
        return True
    elif SERVER_ID in result and "1 row" in result:
        print("\n" + "=" * 70)
        print("[✗] 测试失败！数据库中保存的是错误的设备ID（Request-URI中的ID）")
        print(f"  错误保存的ID: {SERVER_ID}")
        print("=" * 70)
        return False
    else:
        print("\n" + "=" * 70)
        print("[?] 测试结果不确定，请手动检查数据库")
        print("=" * 70)
        return False

if __name__ == '__main__':
    success = test_register_correct_id()
    print(f"\n最终结果: {'通过' if success else '失败'}")
