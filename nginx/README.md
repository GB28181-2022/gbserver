# Nginx 配置说明

本目录为国标服务器系统的 Nginx 反向代理配置。

## 已生成/修改的配置

- **gb_service.conf**：本目录下完整配置，已包含：
  - 前端静态根目录：`/home/user/coder/gb_service2022/frontend/dist`
  - `location /`：SPA 静态 + try_files  fallback 到 index.html
  - `location /api/`：反代到本系统 8080（含 WebSocket）
  - `location /zlm/`：反代到 ZLM 880
- **frontend/dist**：已创建目录并放入占位 `index.html`，Vue 打包后覆盖即可。
- **ZLMediaKit**：`config.ini` 中 http 端口已改为 880/8443，与 Nginx 80/443 无冲突。

## 部署（在服务器上执行）

在终端执行下面整段即可完成备份、替换、校验、重载：

```bash
sudo cp /etc/nginx/sites-available/default /etc/nginx/sites-available/default.bak && \
sudo cp /home/user/coder/gb_service2022/nginx/gb_service.conf /etc/nginx/sites-available/default && \
sudo nginx -t && \
sudo systemctl reload nginx
```

若希望保留原 default、仅新增国标站点：

```bash
sudo cp /home/user/coder/gb_service2022/nginx/gb_service.conf /etc/nginx/sites-available/gb_service
sudo ln -sf /etc/nginx/sites-available/gb_service /etc/nginx/sites-enabled/
# 若 80 仍被 default 占用，可禁用 default：sudo rm /etc/nginx/sites-enabled/default
sudo nginx -t && sudo systemctl reload nginx
```

## 端口一览


| 用途              | 端口   | 说明                      |
| --------------- | ---- | ----------------------- |
| Nginx           | 80   | 对外入口，前端 + /api/ + /zlm/ |
| 本系统 C++ 后端      | 8080 | /api/ 反代目标              |
| ZLMediaKit HTTP | 880  | /zlm/ 反代目标              |


