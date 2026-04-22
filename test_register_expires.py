#!/usr/bin/env python3
"""
测试注册响应是否包含 Expires 头
"""

import socket
import uuid
import random
import re

# 配置
LOCAL_IP = "192.168.1.9"
LOCAL_PORT = 5073
SERVER_IP = "192.168.1.9"
SERVER_PORT = 5060

DEVICE_ID = "42000000112007000015"
SERVER_ID = "34020000002000000001"

def make_register_request(device_id, server_id, contact_ip, contact_port, 
                          call_id, cseq, expires=3600, auth_header=None):
    """生成 SIP REGISTER 请求"""
    via = f"SIP/2.0/UDP {contact_ip}:{contact_port};rport;branch=z9hG4bK{random.randint(100000, 999999)}"
    from_hdr = f"<sip:{device_id}@{contact_ip}>"
    to_hdr = f"<sip:{server_id}@{SERVER_IP}:{SERVER_PORT}>"
    contact = f"<sip:{device_id}@{contact_ip}:{contact_port}>"
    request_uri = f"sip:{server_id}@{SERVER_IP}:{SERVER_PORT}"
    
    msg = f"""REGISTER {request_uri} SIP/2.0
Via: {via}
From: {from_hdr};tag={random.randint(100000, 999999)}
To: {to_hdr}
Call-ID: {call_id}
CSeq: {cseq} REGISTER
Contact: {contact}
Max-Forwards: 70
User-Agent: GBXuean/v3
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
    import hashlib
    ha1 = hashlib.md5(f"{username}:{realm}:{password}".encode()).hexdigest()
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    response = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()
    return response

def test_register_with_expires():
    """测试注册响应包含 Expires 头"""
    print("=" * 70)
    print("测试注册响应是否包含 Expires 头")
    print("=" * 70)
    print(f"设备ID: {DEVICE_ID}")
    print(f"请求 Expires: 3600 秒")
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
    msg = make_register_request(DEVICE_ID, SERVER_ID, LOCAL_IP, LOCAL_PORT, call_id, cseq, expires=3600)
    sock.sendto(msg.encode('utf-8'), server_addr)
    
    try:
        data, addr = sock.recvfrom(4096)
        text = data.decode('utf-8', errors='ignore')
        
        if "SIP/2.0 401" in text:
            print("[✓] 收到 401 Unauthorized")
            
            realm, nonce = parse_www_authenticate(text)
            if realm and nonce:
                # 步骤2：发送带鉴权的 REGISTER
                print("\n[步骤2] 发送带鉴权的 REGISTER...")
                cseq += 1
                
                import hashlib
                uri = f"sip:{SERVER_ID}@{SERVER_IP}:{SERVER_PORT}"
                response = compute_digest_response(DEVICE_ID, realm, "admin", "REGISTER", uri, nonce)
                
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
                    
                    print("\n[收到响应]")
                    print("-" * 70)
                    # 打印响应头
                    headers = text.split('\n')[:15]  # 前15行
                    for line in headers:
                        if line.strip():
                            print(line)
                    print("-" * 70)
                    
                    if "SIP/2.0 200" in text:
                        # 检查是否包含 Expires 头
                        expires_match = re.search(r'Expires:\s*(\d+)', text, re.IGNORECASE)
                        date_match = re.search(r'Date:\s*(.+)', text, re.IGNORECASE)
                        
                        print("\n[验证结果]")
                        if expires_match:
                            expires_val = expires_match.group(1)
                            print(f"[✓] Expires 头存在: {expires_val} 秒")
                        else:
                            print("[✗] Expires 头不存在")
                        
                        if date_match:
                            date_val = date_match.group(1).strip()
                            print(f"[✓] Date 头存在: {date_val}")
                        else:
                            print("[✗] Date 头不存在")
                        
                        if expires_match:
                            print("\n" + "=" * 70)
                            print("[✓] 测试通过！注册响应包含 Expires 头")
                            print("  设备应该根据此值计算下次注册时间")
                            print("=" * 70)
                            return True
                        else:
                            print("\n" + "=" * 70)
                            print("[✗] 测试失败！注册响应缺少 Expires 头")
                            print("  这会导致设备频繁重新注册")
                            print("=" * 70)
                            return False
                            
                except socket.timeout:
                    print("[✗] 等待响应超时")
                    return False
        else:
            print(f"[?] 收到意外响应: {text[:200]}")
            return False
            
    except socket.timeout:
        print("[✗] 等待响应超时")
        return False
    finally:
        sock.close()

if __name__ == '__main__':
    success = test_register_with_expires()
    print(f"\n最终结果: {'通过' if success else '失败'}")
