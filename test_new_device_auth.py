#!/usr/bin/env python3
"""
测试新设备注册是否需要鉴权
"""

import socket
import uuid
import random

# 配置
LOCAL_IP = "192.168.1.9"
LOCAL_PORT = 5062  # 测试用端口
SERVER_IP = "192.168.1.9"
SERVER_PORT = 5060
# 使用全新的设备ID
NEW_DEVICE_ID = "44000000112000000001"  # 全新的设备

def make_register_request(to_uri, from_uri, call_id, cseq, expires=3600, auth_header=None):
    """生成 SIP REGISTER 请求"""
    via = f"SIP/2.0/UDP {LOCAL_IP}:{LOCAL_PORT};rport;branch=z9hG4bK{random.randint(100000, 999999)}"
    from_hdr = f"<sip:{NEW_DEVICE_ID}@{SERVER_IP}:{SERVER_PORT}>"
    to_hdr = f"<sip:{NEW_DEVICE_ID}@{SERVER_IP}:{SERVER_PORT}>"
    contact = f"<sip:{NEW_DEVICE_ID}@{LOCAL_IP}:{LOCAL_PORT}>"
    
    # 使用正确的Request-URI格式，包含设备ID
    request_uri = f"sip:{NEW_DEVICE_ID}@{SERVER_IP}:{SERVER_PORT}"
    
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

def parse_www_authenticate(data):
    """解析 401 响应中的 WWW-Authenticate 头"""
    text = data.decode('utf-8', errors='ignore')
    
    # 检查是否是 401
    if "SIP/2.0 401" not in text:
        return None
    
    # 提取 WWW-Authenticate
    auth_start = text.find("WWW-Authenticate:")
    if auth_start == -1:
        return None
    
    auth_end = text.find("\n", auth_start)
    if auth_end == -1:
        auth_end = len(text)
    
    auth_line = text[auth_start:auth_end].strip()
    
    # 解析 realm, nonce, opaque
    result = {}
    for key in ['realm', 'nonce', 'opaque']:
        key_start = auth_line.find(f'{key}="')
        if key_start != -1:
            val_start = key_start + len(f'{key}="')
            val_end = auth_line.find('"', val_start)
            if val_end != -1:
                result[key] = auth_line[val_start:val_end]
    
    return result

def compute_digest_response(username, realm, password, method, uri, nonce):
    """计算 Digest 响应（简化版，MD5）"""
    import hashlib
    
    # HA1 = MD5(username:realm:password)
    ha1 = hashlib.md5(f"{username}:{realm}:{password}".encode()).hexdigest()
    
    # HA2 = MD5(method:uri)
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    
    # response = MD5(HA1:nonce:HA2)
    response = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()
    
    return response

def test_new_device_auth():
    """测试新设备注册鉴权流程"""
    print("=" * 60)
    print("测试新设备注册鉴权流程")
    print("=" * 60)
    print(f"新设备ID: {NEW_DEVICE_ID}")
    print(f"系统密码: admin")
    print("-" * 60)
    
    # 创建 UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LOCAL_IP, LOCAL_PORT))
    sock.settimeout(10)
    
    server_addr = (SERVER_IP, SERVER_PORT)
    call_id = str(uuid.uuid4())
    cseq = 1
    
    # 步骤1：发送不带鉴权信息的 REGISTER
    print("\n[步骤1] 发送不带鉴权信息的 REGISTER...")
    msg = make_register_request(
        f"sip:{NEW_DEVICE_ID}@{SERVER_IP}:{SERVER_PORT}",
        f"sip:{NEW_DEVICE_ID}@{LOCAL_IP}:{LOCAL_PORT}",
        call_id, cseq
    )
    sock.sendto(msg.encode('utf-8'), server_addr)
    
    # 等待响应
    try:
        data, addr = sock.recvfrom(4096)
        text = data.decode('utf-8', errors='ignore')
        
        if "SIP/2.0 401" in text:
            print("[✓] 收到 401 Unauthorized，服务器要求鉴权")
            
            # 解析 WWW-Authenticate
            auth_info = parse_www_authenticate(data)
            if auth_info:
                print(f"[✓] 获取到鉴权参数: realm={auth_info.get('realm')}, nonce={auth_info.get('nonce')[:16]}...")
                
                # 步骤2：发送带鉴权信息的 REGISTER
                print("\n[步骤2] 发送带鉴权信息的 REGISTER...")
                cseq += 1
                
                # 计算 Digest 响应
                username = NEW_DEVICE_ID
                realm = auth_info.get('realm', 'gb_service')
                password = "admin"  # 系统密码
                method = "REGISTER"
                uri = f"sip:{SERVER_IP}:{SERVER_PORT}"
                nonce = auth_info.get('nonce', '')
                opaque = auth_info.get('opaque', '')
                
                response = compute_digest_response(username, realm, password, method, uri, nonce)
                
                # 构建 Authorization 头
                auth_header = f'Digest username="{username}", realm="{realm}", nonce="{nonce}", uri="{uri}", response="{response}", algorithm=MD5, opaque="{opaque}"'
                
                msg = make_register_request(
                    f"sip:{NEW_DEVICE_ID}@{SERVER_IP}:{SERVER_PORT}",
                    f"sip:{NEW_DEVICE_ID}@{LOCAL_IP}:{LOCAL_PORT}",
                    call_id, cseq, auth_header=auth_header
                )
                sock.sendto(msg.encode('utf-8'), server_addr)
                
                # 等待响应
                try:
                    data, addr = sock.recvfrom(4096)
                    text = data.decode('utf-8', errors='ignore')
                    
                    if "SIP/2.0 200" in text:
                        print("[✓] 收到 200 OK，鉴权成功，设备注册成功！")
                        return True
                    elif "SIP/2.0 401" in text:
                        print("[✗] 收到 401，鉴权失败，密码错误")
                        return False
                    else:
                        print(f"[?] 收到意外响应: {text[:100]}")
                        return False
                        
                except socket.timeout:
                    print("[✗] 等待鉴权响应超时")
                    return False
            else:
                print("[✗] 无法解析 WWW-Authenticate 头")
                return False
                
        elif "SIP/2.0 200" in text:
            print("[✗] 收到 200 OK，新设备没有走鉴权流程！（不符合预期）")
            return False
        else:
            print(f"[?] 收到意外响应: {text[:100]}")
            return False
            
    except socket.timeout:
        print("[✗] 等待响应超时")
        return False
    finally:
        sock.close()

if __name__ == "__main__":
    success = test_new_device_auth()
    print("\n" + "=" * 60)
    if success:
        print("测试通过: 新设备注册需要鉴权")
    else:
        print("测试失败")
    print("=" * 60)
