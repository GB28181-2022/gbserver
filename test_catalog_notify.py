#!/usr/bin/env python3
"""
测试 Catalog Notify 上报功能
模拟下级平台发送 Catalog Notify 消息
"""

import socket
import uuid
import random
import time

# 配置 - 使用实际在线的平台
LOCAL_IP = "192.168.1.9"
LOCAL_PORT = 5063  # 测试用端口
SERVER_IP = "192.168.1.9"
SERVER_PORT = 5060
# 使用在线的平台 42000000112007000011
PLATFORM_GBID = "42000000112007000011"

def make_catalog_notify_xml(from_gb_id, sn, device_list):
    """生成 Catalog Notify XML"""
    total = len(device_list)
    items_xml = ""
    
    for dev in device_list:
        items_xml += f"""    <Item>
      <DeviceID>{dev['id']}</DeviceID>
      <Name>{dev['name']}</Name>
      <Manufacturer>{dev.get('manufacturer', 'Hikvision')}</Manufacturer>
      <Model>{dev.get('model', 'IPC')}</Model>
      <Owner>{dev.get('owner', '0')}</Owner>
      <CivilCode>{dev.get('civilCode', '420000')}</CivilCode>
      <Address>{dev.get('address', 'Default')}</Address>
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

def test_catalog_notify():
    """测试 Catalog Notify 上报"""
    print("=" * 60)
    print("测试 Catalog Notify 设备目录上报")
    print("=" * 60)
    print(f"平台ID: {PLATFORM_GBID}")
    print(f"目标服务器: {SERVER_IP}:{SERVER_PORT}")
    print("-" * 60)
    
    # 定义测试设备列表（模拟下级平台上报）
    test_devices = [
        {
            'id': f'{PLATFORM_GBID}001',
            'name': '摄像头01',
            'manufacturer': 'TestCorp',
            'model': 'IPC-001',
            'status': 'ON',
            'civilCode': '420000'
        },
        {
            'id': f'{PLATFORM_GBID}002',
            'name': '摄像头02',
            'manufacturer': 'TestCorp',
            'model': 'IPC-002',
            'status': 'ON',
            'civilCode': '420000'
        },
        {
            'id': f'{PLATFORM_GBID}003',
            'name': '摄像头03',
            'manufacturer': 'TestCorp',
            'model': 'IPC-003',
            'status': 'OFF',
            'civilCode': '420000'
        }
    ]
    
    # 创建 UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(10)
    
    server_addr = (SERVER_IP, SERVER_PORT)
    sn = int(time.time())
    
    # 构建目标 URI（服务器）
    to_uri = f"sip:34020000002000000001@{SERVER_IP}:{SERVER_PORT}"
    from_uri = f"sip:{PLATFORM_GBID}@{LOCAL_IP}:{LOCAL_PORT}"
    
    # 生成 Catalog Notify XML
    body = make_catalog_notify_xml(PLATFORM_GBID, sn, test_devices)
    
    # 生成 SIP MESSAGE
    msg = make_sip_message(to_uri, from_uri, body)
    
    print(f"\n[发送 Catalog Notify]")
    print(f"SN: {sn}")
    print(f"设备数: {len(test_devices)}")
    print(f"目标: {server_addr}")
    print("-" * 60)
    
    # 发送消息
    sock.sendto(msg.encode('utf-8'), server_addr)
    
    # 等待响应
    try:
        data, addr = sock.recvfrom(4096)
        text = data.decode('utf-8', errors='ignore')
        
        if "SIP/2.0 200" in text:
            print("[✓] 收到 200 OK，Catalog Notify 已接收")
            print("\n等待 3 秒后检查数据库...")
            time.sleep(3)
            
            # 这里可以添加数据库检查
            return True
        else:
            print(f"[✗] 收到非 200 响应: {text[:100]}")
            return False
            
    except socket.timeout:
        print("[✗] 等待响应超时")
        return False
    finally:
        sock.close()

if __name__ == "__main__":
    success = test_catalog_notify()
    print("\n" + "=" * 60)
    if success:
        print("测试完成，Catalog Notify 已发送")
    else:
        print("测试失败")
    print("=" * 60)
