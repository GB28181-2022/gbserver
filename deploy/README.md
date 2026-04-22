# GB28181 国标服务部署包

## 部署说明

此部署包用于在 **192.168.1.2** 服务器上部署国标 GB28181 服务。

## 目录结构

```
.
├── install.sh              # 一键部署脚本
├── README.md               # 本说明文件
├── backend/
│   └── gb_service          # 后端可执行文件
├── frontend/
│   ├── index.html          # 前端页面
│   └── assets/             # 前端静态资源
├── sql/
│   └── schema.sql          # 数据库表结构
└── config/
    ├── gb_service.service  # systemd 服务文件
    ├── gb_service.conf     # Nginx 配置文件
    └── gb_service.ini      # （可选）数据库连接配置
```

## 前置条件

1. 目标服务器 IP: **192.168.1.2**
2. 已有 **ZLMediaKit** 流媒体服务运行
3. ZLM Secret: `sF3auJDv6AqFUs8BfrlLYmDt2H7S9MGC`
4. 系统: Ubuntu 20.04+ / Debian 10+
5. 有 root/sudo 权限

## 快速部署

### 方式一：一键自动部署（推荐）

1. 将部署包上传到 192.168.1.2:

```bash
scp -r gb28181-deploy user@192.168.1.2:/tmp/
```

1. SSH 登录目标服务器并执行:

```bash
ssh user@192.168.1.2
cd /tmp/gb28181-deploy
sudo ./install.sh
```

部署脚本会自动完成以下操作：

- 安装系统依赖（PostgreSQL、Nginx、运行时库）
- 初始化数据库和表结构
- 配置 ZLM Hook 接口
- 配置 Nginx 反向代理
- 创建 systemd 服务
- 启动服务并验证

### 方式二：手动部署

如果需要手动控制每一步，请参考以下步骤：

#### 1. 安装依赖

```bash
sudo apt-get update
sudo apt-get install -y libpq5 libcurl4 libssl1.1 libuuid1 postgresql nginx
```

#### 2. 初始化数据库

```bash
sudo -u postgres psql -c "CREATE USER user WITH PASSWORD 'gb28181_pass';"
sudo -u postgres psql -c "CREATE DATABASE gb28181 OWNER user;"
sudo -u postgres psql -d gb28181 -f sql/schema.sql
```

#### 3. 部署应用

```bash
sudo mkdir -p /opt/gb_service2022/{backend,frontend,logs}
sudo cp backend/gb_service /opt/gb_service2022/backend/
sudo cp -r frontend/* /opt/gb_service2022/frontend/
sudo chmod +x /opt/gb_service2022/backend/gb_service
```

#### 4. 配置 Nginx

```bash
sudo cp config/gb_service.conf /etc/nginx/sites-available/
sudo ln -sf /etc/nginx/sites-available/gb_service.conf /etc/nginx/sites-enabled/
sudo nginx -t
sudo systemctl reload nginx
```

#### 5. 创建服务

```bash
sudo cp config/gb_service.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable gb_service
sudo systemctl start gb_service
```

## 验证部署

部署完成后，访问以下地址验证：

1. **前端页面**: [http://192.168.1.2/](http://192.168.1.2/)
2. **API 健康检查**: [http://192.168.1.2:8080/api/health](http://192.168.1.2:8080/api/health)
3. **设备缓存状态**: [http://192.168.1.2:8080/api/stats/device-cache](http://192.168.1.2:8080/api/stats/device-cache)

## 服务管理

```bash
# 查看服务状态
sudo systemctl status gb_service

# 启动/停止/重启
sudo systemctl start gb_service
sudo systemctl stop gb_service
sudo systemctl restart gb_service

# 查看日志
sudo tail -f /var/log/gb_service.log
sudo journalctl -u gb_service -f

# 查看 Nginx 日志
sudo tail -f /var/log/nginx/gb_service_error.log
```

## 端口说明


| 服务         | 端口   | 协议      | 说明      |
| ---------- | ---- | ------- | ------- |
| Nginx      | 80   | TCP     | HTTP 入口 |
| 国标服务 HTTP  | 8080 | TCP     | API 服务  |
| 国标服务 SIP   | 5060 | UDP/TCP | 信令服务    |
| ZLM HTTP   | 880  | TCP     | ZLM API |
| PostgreSQL | 5432 | TCP     | 数据库     |


## 配置文件

### 数据库配置

- 数据库: `gb28181`
- 用户: `user`
- 密码: `gb28181_pass`

后端会优先读取 `config/gb_service.ini`（部署后对应 `/opt/gb_service2022/config/gb_service.ini`）。
若文件不存在、格式不合法或缺少必填项，会回退默认连接参数：
`host=/var/run/postgresql dbname=gb28181 user=user`。

配置示例：

```ini
host=/var/run/postgresql
port=5432
dbname=gb28181
user=user
password=
```

### 国标平台配置

- 国标ID: `34020000002000000001`
- 域: `3402000000`
- SIP IP: `192.168.1.2`
- SIP 端口: `5060`

### ZLM Hook 配置

在 ZLMediaKit 配置文件 `config.ini` 中设置：

```ini
[hook]
enable=1
on_stream_changed=http://127.0.0.1:8080/api/zlm/hook/on_stream_changed
on_stream_none_reader=http://127.0.0.1:8080/api/zlm/hook/on_stream_none_reader
on_rtp_server_timeout=http://127.0.0.1:8080/api/zlm/hook/on_rtp_server_timeout
```

## 故障排查

### 服务无法启动

```bash
# 检查日志
sudo journalctl -u gb_service -n 100

# 检查端口占用
sudo netstat -tulnp | grep 8080
sudo netstat -tulnp | grep 5060
```

### 数据库连接失败

```bash
# 检查 PostgreSQL 状态
sudo systemctl status postgresql

# 检查数据库和用户
sudo -u postgres psql -c "\l"
sudo -u postgres psql -c "\du"
```

### ZLM 连接失败

```bash
# 测试 ZLM API
curl "http://127.0.0.1:880/index/api/getMediaList?secret=sF3auJDv6AqFUs8BfrlLYmDt2H7S9MGC"
```

## 安全建议

1. 修改默认数据库密码
2. 修改 ZLM Secret
3. 配置防火墙规则
4. 启用 HTTPS（使用 SSL 证书）

## 联系支持

如有问题，请查看日志文件或联系技术支持。