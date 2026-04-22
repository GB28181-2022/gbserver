#!/usr/bin/env python3
"""
GB28181 Catalog 设备目录查询功能测试脚本
模拟下级平台响应 Catalog 查询请求
"""

import socket
import uuid
import random
import time
from datetime import datetime

# 配置
LOCAL_IP = "192.168.1.9"
LOCAL_PORT = 5061  # 测试用端口
SERVER_IP = "192.168.1.9"
SERVER_PORT = 5060
DEVICE_GBID = "41010200112007999148"  # 测试设备ID

# 生成 SN
sn_counter = 1

def generate_sn():
    global sn_counter
    sn = sn_counter
    sn_counter += 1
    return sn

def generate_nonce():
    """生成32位随机 nonce"""
    return ''.join(random.choices('0123456789abcdef', k=32))

def make_catalog_response(to_gb_id, from_gb_id, sn, device_list):
    """生成 Catalog 响应 XML"""
    total = len(device_list)
    items_xml = ""
    
    for dev in device_list:
        items_xml += f"""    <Item>
      <DeviceID>{dev['id']}</DeviceID>
      <Name>{dev['name']}</Name>
      <Manufacturer>{dev.get('manufacturer', 'Test')}</Manufacturer>
      <Model>{dev.get('model', 'Camera')}</Model>
      <Owner>{dev.get('owner', '0')}</Owner>
      <CivilCode>{dev.get('civilCode', '410102')}</CivilCode>
      <Address>{dev.get('address', 'Test Address')}</Address>
      <Parental>0</Parental>
      <ParentID>{from_gb_id}</ParentID>
      <SafetyWay>0</SafetyWay>
      <RegisterWay>1</RegisterWay>
      <Secrecy>0</Secrecy>
      <Status>{dev.get('status', 'ON')}</Status>
    </Item>
"""
    
    xml = f"""<?xml version="1.0" encoding="GB2312"?>
<Notify>
  <CmdType>Notify</CmdType>
  <SN>{sn}</SN>
  <DeviceID>{from_gb_id}</DeviceID>
  <DeviceList Num="{total}">
{items_xml}  </DeviceList>
</Notify>"""
    return xml

def make_sip_message(to_uri, from_uri, body, cseq=1):
    """生成 SIP MESSAGE 请求"""
    call_id = str(uuid.uuid4())
    
    msg = f"""MESSAGE {to_uri} SIP/2.0
Via: SIP/2.0/UDP {LOCAL_IP}:{LOCAL_PORT};rport;branch=z9hG4bK{random.randint(100000, 999999)}
From: <{from_uri}>;tag={random.randint(100000, 999999)}
To: <{to_uri}>
Call-ID: {call_id}
CSeq: {cseq} MESSAGE
Content-Type: Application/MANSCDP+xml
Max-Forwards: 70
User-Agent: GB28181 Test Client
Content-Length: {len(body.encode('utf-8'))}

{body}"""
    return msg

def send_catalog_response(sock, server_addr, sn, device_list):
    """发送 Catalog 响应"""
    to_uri = f"sip:34020000002000000001@{SERVER_IP}:{SERVER_PORT}"  # 服务器URI
    from_uri = f"sip:{DEVICE_GBID}@{LOCAL_IP}:{LOCAL_PORT}"  # 本机URI
    
    body = make_catalog_response(
        to_gb_id="34020000002000000001",
        from_gb_id=DEVICE_GBID,
        sn=sn,
        device_list=device_list
    )
    
    msg = make_sip_message(to_uri, from_uri, body)
    
    print(f"[发送 Catalog 响应] SN={sn}, 设备数={len(device_list)}")
    print(f"目标: {server_addr}")
    print("-" * 50)
    
    sock.sendto(msg.encode('utf-8'), server_addr)
    return True

def parse_catalog_query(data):
    """解析收到的 Catalog 查询请求"""
    try:
        text = data.decode('utf-8', errors='ignore')
        
        # 检查是否是 MESSAGE 请求
        if not text.startswith("MESSAGE"):
            return None
        
        # 提取 SN
        sn_start = text.find("<SN>")
        sn_end = text.find("</SN>")
        if sn_start > 0 and sn_end > sn_start:
            sn = text[sn_start+4:sn_end]
        else:
            sn = "1"
        
        # 提取 DeviceID
        dev_start = text.find("<DeviceID>")
        dev_end = text.find("</DeviceID>")
        device_id = ""
        if dev_start > 0 and dev_end > dev_start:
            device_id = text[dev_start+10:dev_end]
        
        # 检查是否是 Catalog 查询
        if "<CmdType>Catalog</CmdType>" in text or "<CmdType>Query</CmdType>" in text:
            return {
                'sn': sn,
                'device_id': device_id,
                'is_catalog': True
            }
        
        return None
    except Exception as e:
        print(f"解析错误: {e}")
        return None

def send_200_ok(sock, addr, cseq):
    """发送 200 OK 响应"""
    response = f"""SIP/2.0 200 OK
Via: SIP/2.0/UDP {addr[0]}:{addr[1]};rport={addr[1]}
From: <sip:{DEVICE_GBID}@{LOCAL_IP}:{LOCAL_PORT}>
To: <sip:34020000002000000001@{SERVER_IP}:{SERVER_PORT}>
Call-ID: {uuid.uuid4()}
CSeq: {cseq} MESSAGE
Content-Length: 0

"""
    sock.sendto(response.encode('utf-8'), addr)

def test_catalog_flow():
    """测试 Catalog 查询流程"""
    print("=" * 60)
    print("GB28181 Catalog 设备目录查询功能测试")
    print("=" * 60)
    print(f"本地地址: {LOCAL_IP}:{LOCAL_PORT}")
    print(f"服务器地址: {SERVER_IP}:{SERVER_PORT}")
    print(f"测试设备ID: {DEVICE_GBID}")
    print("-" * 60)
    
    # 创建 UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LOCAL_IP, LOCAL_PORT))
    sock.settimeout(30)  # 30秒超时
    
    server_addr = (SERVER_IP, SERVER_PORT)
    
    # 定义测试设备列表
    test_devices = [
        {
            'id': f'{DEVICE_GBID}01',
            'name': '测试摄像头01',
            'manufacturer': 'TestCorp',
            'model': 'TC-IPC-001',
            'status': 'ON'
        },
        {
            'id': f'{DEVICE_GBID}02',
            'name': '测试摄像头02',
            'manufacturer': 'TestCorp',
            'model': 'TC-IPC-002',
            'status': 'ON'
        },
        {
            'id': f'{DEVICE_GBID}03',
            'name': '测试摄像头03',
            'manufacturer': 'TestCorp',
            'model': 'TC-IPC-003',
            'status': 'OFF'
        }
    ]
    
    print("\n等待服务器发送 Catalog 查询请求...")
    print("请在30秒内在前端页面点击'查询设备目录'按钮")
    print("-" * 60)
    
    catalog_received = False
    
    try:
        while True:
            try:
                data, addr = sock.recvfrom(4096)
                text = data.decode('utf-8', errors='ignore')
                
                # 解析收到的 MESSAGE 请求
                if text.startswith("MESSAGE"):
                    # 提取 CSeq
                    cseq_start = text.find("CSeq:")
                    cseq = "1"
                    if cseq_start > 0:
                        cseq_line = text[cseq_start:cseq_start+20]
                        parts = cseq_line.split()
                        if len(parts) >= 2:
                            cseq = parts[1]
                    
                    result = parse_catalog_query(data)
                    
                    if result and result['is_catalog']:
                        sn = result['sn']
                        print(f"\n[收到 Catalog 查询] SN={sn}, DeviceID={result['device_id']}")
                        
                        # 发送 200 OK 响应（对查询请求的应答）
                        send_200_ok(sock, addr, cseq)
                        print("[已发送 200 OK 响应]")
                        
                        # 等待一下再发送 Notify
                        time.sleep(0.5)
                        
                        # 发送 Catalog Notify（主动推送设备列表）
                        send_catalog_response(sock, server_addr, sn, test_devices)
                        print("[已发送 Catalog Notify]")
                        catalog_received = True
                        
                    else:
                        # 其他 MESSAGE，直接回 200 OK
                        send_200_ok(sock, addr, cseq)
                        
                # 检查是否收到服务器的 200 OK（对我们发送的 Notify 的响应）
                elif "SIP/2.0 200" in text and catalog_received:
                    print("[收到服务器 200 OK] Catalog Notify 已成功接收")
                    break
                    
            except socket.timeout:
                if catalog_received:
                    print("\n[完成] Catalog 流程已完成")
                else:
                    print("\n[超时] 等待 Catalog 查询请求超时")
                    print("请确保:")
                    print("1. 后端服务已启动 (./gb_service)")
                    print("2. 在前端页面点击了'查询设备目录'按钮")
                    print("3. 平台配置了正确的独立媒体地址 (192.168.1.9:5061)")
                break
                
    except KeyboardInterrupt:
        print("\n\n用户中断测试")
    finally:
        sock.close()
        print("-" * 60)
        print("测试结束")
        print("=" * 60)

if __name__ == "__main__":
    test_catalog_flow()
