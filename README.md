# 国标服务器系统（gb_service2022）

基于 GB/T 28181-2022，同时作为服务端（平台）与客户端（设备）。详见《国标服务器系统需求规格说明书》《功能清单》《架构设计》。

## 克隆与依赖（Submodule）

默认开发分支为 `**master**`。第三方库在 `third_party/`，请使用子模块一并拉取：

```bash
git clone --recurse-submodules https://github.com/GB28181-2022/gbserver.git
cd gbserver
# 若已 clone 未带子模块：
git submodule update --init
```

说明见 [third_party/README.md](third_party/README.md)。

## 项目骨架（当前阶段）

- **后端**：`backend/`，C++ 单进程，监听 8080，提供 `GET /api/health`。
- **前端**：`frontend/`，Vue 3 + Vite 4，构建输出 `frontend/dist`，由 Nginx 托管。
- **部署**：Nginx 80 → 前端静态 + `/api/` 反代 8080；见 `nginx/`。

### 快速测试

```bash
# 1. 编译并启动后端
cd backend && mkdir -p build && cd build && cmake .. && make && ./gb_service
# 2. 另开终端：健康检查
curl -s http://127.0.0.1:8080/api/health   # 应返回 {"status":"ok"}

# 3. 前端构建（需 Node 14+，推荐 18+）
cd frontend && npm install && npm run build
# 4. 经 Nginx 访问（需已部署 nginx 配置）
curl -s http://127.0.0.1/api/health
# 浏览器打开 http://127.0.0.1/ 查看前端
```

完整验收步骤见 **docs/骨架测试说明.md**。骨架测试通过后再进入下一小周期（如用户登录）。