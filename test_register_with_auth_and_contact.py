#!/usr/bin/env python3
"""
测试带鉴权和 Contact 地址的注册功能
"""

import socket
import uuid
import random
import hashlib
import re

# 配置
LOCAL_IP = "192.168.1.9"
LOCAL_PORT = 5071
SERVER_IP = "192.168.1.9"
SERVER_PORT = 5060

# 测试设备ID
DEVICE_ID = "52000000112007000012"
PASSWORD = "admin"  # 系统配置的密码
REALM = "gb_service"

def make_register_request(device_id, contact_ip, contact_port, call_id, cseq, 
                          expires=3600, auth_header=None):
    """生成 SIP REGISTER 请求"""
    via = f"SIP/2.0/UDP {contact_ip}:{contact_port};rport;branch=z9hG4bK{random.randint(100000, 999999)}"
    from_hdr = f"<sip:{device_id}@{SERVER_IP}:{SERVER_PORT}>"
    to_hdr = f"<sip:{device_id}@{SERVER_IP}:{SERVER_PORT}>"
    contact = f"<sip:{device_id}@{contact_ip}:{contact_port}>"
    request_uri = f"sip:{device_id}@{SERVER_IP}:{SERVER_PORT}"
    
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
    
    # 查找 realm
    realm_match = re.search(r'realm="([^"]+)"', response)
    if realm_match:
        realm = realm_match.group(1)
    
    # 查找 nonce
    nonce_match = re.search(r'nonce="([^"]+)"', response)
    if nonce_match:
        nonce = nonce_match.group(1)
    
    return realm, nonce

def compute_digest_response(username, realm, password, method, uri, nonce):
    """计算 Digest 响应值"""
    # HA1 = MD5(username:realm:password)
    ha1 = hashlib.md5(f"{username}:{realm}:{password}".encode()).hexdigest()
    
    # HA2 = MD5(method:uri)
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    
    # response = MD5(HA1:nonce:HA2)
    response = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()
    
    return response

def test_register():
    """测试带鉴权和 Contact 的注册"""
    print("=" * 60)
    print("测试带鉴权和 Contact 的注册功能")
    print("=" * 60)
    print(f"设备ID: {DEVICE_ID}")
    print(f"Contact: {LOCAL_IP}:{LOCAL_PORT}")
    print(f"密码: {PASSWORD}")
    print("-" * 60)
    
    # 创建 UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LOCAL_IP, LOCAL_PORT))
    sock.settimeout(10)
    
    server_addr = (SERVER_IP, SERVER_PORT)
    call_id = str(uuid.uuid4())
    cseq = 1
    
    # 步骤1：发送不带鉴权的 REGISTER
    print("\n[步骤1] 发送 REGISTER（不带鉴权）...")
    msg = make_register_request(DEVICE_ID, LOCAL_IP, LOCAL_PORT, call_id, cseq)
    sock.sendto(msg.encode('utf-8'), server_addr)
    
    try:
        data, addr = sock.recvfrom(4096)
        text = data.decode('utf-8', errors='ignore')
        
        if "SIP/2.0 401" in text:
            print("[✓] 收到 401 Unauthorized")
            
            # 解析 WWW-Authenticate
            realm, nonce = parse_www_authenticate(text)
            if realm and nonce:
                print(f"  realm: {realm}")
                print(f"  nonce: {nonce[:16]}...")
                
                # 步骤2：计算 Digest 并发送带鉴权的 REGISTER
                print("\n[步骤2] 发送带鉴权的 REGISTER...")
                cseq += 1
                
                uri = f"sip:{DEVICE_ID}@{SERVER_IP}:{SERVER_PORT}"
                response = compute_digest_response(DEVICE_ID, realm, PASSWORD, "REGISTER", uri, nonce)
                
                auth_header = (f'Digest username="{DEVICE_ID}", '
                              f'realm="{realm}", '
                              f'nonce="{nonce}", '
                              f'uri="{uri}", '
                              f'response="{response}"')
                
                msg = make_register_request(DEVICE_ID, LOCAL_IP, LOCAL_PORT, call_id, cseq, 
                                         expires=3600, auth_header=auth_header)
                sock.sendto(msg.encode('utf-8'), server_addr)
                
                # 等待响应
                try:
                    data, addr = sock.recvfrom(4096)
                    text = data.decode('utf-8', errors='ignore')
                    
                    if "SIP/2.0 200" in text:
                        print("[✓] 收到 200 OK，注册成功！")
                        print(f"  Contact 地址 {LOCAL_IP}:{LOCAL_PORT} 应该已保存到数据库")
                    elif "SIP/2.0 401" in text:
                        print("[✗] 鉴权失败，收到 401")
                    elif "SIP/2.0 403" in text:
                        print("[✗] 收到 403 Forbidden")
                    else:
                        print(f"[?] 收到响应: {text[:200]}")
                        
                except socket.timeout:
                    print("[✗] 等待响应超时")
            else:
                print("[✗] 无法解析 WWW-Authenticate")
        elif "SIP/2.0 200" in text:
            print("[✓] 直接收到 200 OK（可能已配置为白名单）")
        elif "SIP/2.0 403" in text:
            print("[✗] 收到 403 Forbidden（可能在黑名单）")
        else:
            print(f"[?] 收到响应: {text[:200]}")
            
    except socket.timeout:
        print("[✗] 等待响应超时")
    finally:
        sock.close()
    
    # 等待数据库写入
    print("\n[等待] 等待数据库写入...")
    import time
    time.sleep(1)
    
    # 查询数据库验证
    print("\n[验证] 查询数据库...")
    import subprocess
    result = subprocess.run([
        'psql', '-U', 'user', '-d', 'gb28181', '-c',
        f"SELECT gb_id, name, contact_ip, contact_port, online, list_type FROM device_platforms WHERE gb_id = '{DEVICE_ID}';"
    ], capture_output=True, text=True)
    print(result.stdout)
    
    # 验证 Contact 是否正确保存
    if LOCAL_IP in result.stdout and str(LOCAL_PORT) in result.stdout:
        print("[✓] Contact 地址已成功保存到数据库！")
        return True
    else:
        print("[✗] Contact 地址未正确保存")
        return False

if __name__ == '__main__':
    success = test_register()
    print("\n" + "=" * 60)
    print("测试结果:", "通过" if success else "失败")
    print("=" * 60)
