# E2E 测试（Playwright）

认证相关流程的端到端测试，覆盖：未登录重定向、错误密码、登录、修改密码、新密码登录、退出登录。

## 环境要求

- **Node.js**：14+（推荐 18+，新版 Playwright 需 18+）
- **后端**：`gb_service` 已启动并监听 8080，且数据库中 `admin` 密码与后端 `hashPasswordDefault("admin")` 一致（见下方「恢复 admin 密码」）。
- **系统依赖**（Linux 无头 Chromium）：若报错 `Host system is missing dependencies`，请执行：
  ```bash
  sudo npx playwright install-deps
  ```
  或按提示安装缺失包，例如：`sudo apt-get install libxdamage1` 等。

## 运行方式

```bash
# 安装前端依赖与 Playwright 浏览器（首次）
cd frontend && npm install && npx playwright install chromium

# 确保后端已启动（8080）
# 可选：已启动前端时可直接跑测，否则由 Playwright 自动启动
npm run test:e2e

# 带 UI 调试
npm run test:e2e:ui
```

## 恢复 admin 密码（供 E2E 或手动登录）

若跑过「修改密码」用例，`admin` 会变为 `admin123`。要恢复为 `admin`：

```bash
HASH=$(echo -n 'gb_svc_2022admin' | openssl dgst -sha256 -binary | xxd -p -c 256)
PGPASSWORD=root psql -U user -h 127.0.0.1 -d gb_service2022 -c "UPDATE users SET password_hash='$HASH' WHERE username='admin';"
```

## 用例说明


| 用例                    | 说明                                |
| --------------------- | --------------------------------- |
| 未登录访问 / 应重定向到登录页      | 访问 `/` 跳转到 `/login`，出现登录表单        |
| 错误密码登录应提示失败           | 错误密码时提示错误且停留在登录页                  |
| 正确密码登录后进入首页并显示当前用户    | admin/admin 登录后到首页，显示「当前用户 admin」 |
| 修改密码成功后跳转登录页并可新密码登录   | 改密后跳转登录，用新密码 admin123 可登录         |
| 退出登录后跳转登录页且再访 / 重定向登录 | 退出后到登录页，再次访问 `/` 仍重定向到登录          |


跑完全部用例后，`admin` 的密码为 `admin123`；若需继续用 `admin` 登录，请执行上面「恢复 admin 密码」。