#!/usr/bin/env python3
"""
抓取 1.9 平台的完整注册响应，用于对比分析
"""

import socket
import uuid
import random
import hashlib
import re

# 配置
LOCAL_IP = "192.168.1.9"
LOCAL_PORT = 5075
SERVER_IP = "192.168.1.9"
SERVER_PORT = 5060

# 测试设备
DEVICE_ID = "42000000112007000020"
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
    realm_match = re.search(r'realm="([^"]+)"', response)
    nonce_match = re.search(r'nonce="([^"]+)"', response)
    return realm_match.group(1) if realm_match else None, nonce_match.group(1) if nonce_match else None

def compute_digest_response(username, realm, password, method, uri, nonce):
    """计算 Digest 响应值"""
    ha1 = hashlib.md5(f"{username}:{realm}:{password}".encode()).hexdigest()
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    return hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()

def test_and_capture():
    """测试并完整捕获 1.9 平台的响应"""
    print("=" * 80)
    print("抓取 1.9 平台 (gb_service) 的完整注册响应")
    print("=" * 80)
    print(f"目标服务器: {SERVER_IP}:{SERVER_PORT}")
    print(f"测试设备ID: {DEVICE_ID}")
    print("-" * 80)
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LOCAL_IP, LOCAL_PORT))
    sock.settimeout(10)
    
    server_addr = (SERVER_IP, SERVER_PORT)
    call_id = str(uuid.uuid4())
    cseq = 1
    
    # 发送不带鉴权的 REGISTER
    print("\n[发送] REGISTER (无鉴权)")
    msg = make_register_request(DEVICE_ID, SERVER_ID, LOCAL_IP, LOCAL_PORT, call_id, cseq)
    sock.sendto(msg.encode('utf-8'), server_addr)
    
    responses = []
    
    try:
        data, addr = sock.recvfrom(4096)
        text = data.decode('utf-8', errors='ignore')
        responses.append(("401 响应", text))
        
        if "SIP/2.0 401" in text:
            print("[收到] 401 Unauthorized")
            print("\n完整 401 响应:")
            print("-" * 80)
            print(text)
            print("-" * 80)
            
            realm, nonce = parse_www_authenticate(text)
            if realm and nonce:
                cseq += 1
                uri = f"sip:{SERVER_ID}@{SERVER_IP}:{SERVER_PORT}"
                response = compute_digest_response(DEVICE_ID, realm, "admin", "REGISTER", uri, nonce)
                
                auth_header = (f'Digest username="{DEVICE_ID}", '
                              f'realm="{realm}", '
                              f'nonce="{nonce}", '
                              f'uri="{uri}", '
                              f'response="{response}"')
                
                msg = make_register_request(DEVICE_ID, SERVER_ID, LOCAL_IP, LOCAL_PORT, 
                                         call_id, cseq, expires=3600, auth_header=auth_header)
                
                print("\n[发送] REGISTER (带鉴权)")
                sock.sendto(msg.encode('utf-8'), server_addr)
                
                try:
                    data, addr = sock.recvfrom(4096)
                    text = data.decode('utf-8', errors='ignore')
                    responses.append(("200 OK 响应", text))
                    
                    if "SIP/2.0 200" in text:
                        print("[收到] 200 OK")
                        print("\n完整 200 OK 响应:")
                        print("=" * 80)
                        print(text)
                        print("=" * 80)
                        
                        # 分析响应头
                        print("\n[响应头分析]")
                        headers = text.split('\n')
                        for line in headers:
                            if ':' in line and not line.startswith('SIP/2.0'):
                                print(f"  {line.strip()}")
                        
                        # 检查关键头
                        has_contact = 'Contact:' in text or 'contact:' in text.lower()
                        has_expires = 'Expires:' in text or 'expires:' in text.lower()
                        has_date = 'Date:' in text or 'date:' in text.lower()
                        
                        print("\n[关键头检查]")
                        print(f"  Contact: {'✓' if has_contact else '✗'}")
                        print(f"  Expires: {'✓' if has_expires else '✗'}")
                        print(f"  Date: {'✓' if has_date else '✗'}")
                        
                except socket.timeout:
                    print("[✗] 等待 200 OK 超时")
                    
    except socket.timeout:
        print("[✗] 等待 401 响应超时")
    finally:
        sock.close()
    
    return responses

if __name__ == '__main__':
    responses = test_and_capture()
    print("\n" + "=" * 80)
    print("抓取完成，请将以上 200 OK 响应与 1.107 平台的响应对比")
    print("=" * 80)
