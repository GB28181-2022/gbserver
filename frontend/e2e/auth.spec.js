/**
 * 认证流程 E2E：登录、修改密码、新密码登录、退出
 *
 * 前置条件：
可用以下命令恢复为 admin：
 *   echo -n 'gb_svc_2022admin' | openssl dgst -sha256 -binary | xxd -p -c 256 * - 后端已启动（8080），且数据库中 admin 密码为与后端 hashPasswordDefault("admin") 一致的哈希。
 * - 若之前跑过本用例，admin 密码会变成 admin123，
 *   PGPASSWORD=root psql -U user -h 127.0.0.1 -d gb_service2022 -c "UPDATE users SET password_hash='<上面输出的哈希>' WHERE username='admin';"
 */
import { test, expect } from '@playwright/test'

const INITIAL_PASSWORD = 'admin'
const NEW_PASSWORD = 'admin123'

test.describe('认证流程', () => {
  test('未登录访问 / 应重定向到登录页', async ({ page }) => {
    await page.goto('/')
    await expect(page).toHaveURL(/\/login/)
    await expect(page.getByRole('heading', { name: '国标服务器系统' })).toBeVisible()
    await expect(page.getByRole('button', { name: '进入控制台' })).toBeVisible()
  })

  test('错误密码登录应提示失败', async ({ page }) => {
    await page.goto('/login')
    await page.getByPlaceholder('默认 admin').first().fill('admin')
    await page.getByPlaceholder('默认 admin').last().fill('wrong')
    await page.getByRole('button', { name: '进入控制台' }).click()
    await expect(page.getByText(/用户名或密码错误|登录失败/)).toBeVisible({ timeout: 5000 })
    await expect(page).toHaveURL(/\/login/)
  })

  test('正确密码登录后进入首页并显示当前用户', async ({ page }) => {
    await page.goto('/login')
    await page.getByPlaceholder('默认 admin').first().fill('admin')
    await page.getByPlaceholder('默认 admin').last().fill(INITIAL_PASSWORD)
    await page.getByRole('button', { name: '进入控制台' }).click()
    await expect(page).toHaveURL('/')
    await expect(page.getByRole('heading', { name: '概览' })).toBeVisible()
    await expect(page.getByRole('button', { name: /当前用户\s*admin/ })).toBeVisible()
  })

  test('修改密码成功后跳转登录页并可新密码登录', async ({ page }) => {
    // 先登录（假设当前 admin 密码为 INITIAL_PASSWORD；若上次跑完是 admin123 则需先改回或改此处）
    await page.goto('/login')
    await page.getByPlaceholder('默认 admin').first().fill('admin')
    await page.getByPlaceholder('默认 admin').last().fill(INITIAL_PASSWORD)
    await page.getByRole('button', { name: '进入控制台' }).click()
    await expect(page).toHaveURL('/')

    // 进入修改密码
    await page.getByRole('button', { name: /当前用户\s*admin/ }).click()
    await page.getByRole('menuitem', { name: '修改密码' }).click()
    await expect(page).toHaveURL(/\/account\/password/)
    await expect(page.getByRole('heading', { name: '修改密码' })).toBeVisible()

    await page.getByRole('textbox', { name: '旧密码' }).fill(INITIAL_PASSWORD)
    await page.getByRole('textbox', { name: '新密码' }).fill(NEW_PASSWORD)
    await page.getByRole('textbox', { name: '确认密码' }).fill(NEW_PASSWORD)
    await page.getByRole('button', { name: '保存修改' }).click()

    await expect(page).toHaveURL(/\/login/, { timeout: 10000 })
    await expect(page.getByRole('button', { name: '进入控制台' })).toBeVisible()

    // 用新密码登录
    await page.getByPlaceholder('默认 admin').first().fill('admin')
    await page.getByPlaceholder('默认 admin').last().fill(NEW_PASSWORD)
    await page.getByRole('button', { name: '进入控制台' }).click()
    await expect(page).toHaveURL('/')
    await expect(page.getByRole('button', { name: /当前用户\s*admin/ })).toBeVisible()
  })

  test('退出登录后跳转登录页且再访 / 重定向登录', async ({ page }) => {
    // 使用新密码登录（若上一条用例已跑过则当前密码为 admin123）
    await page.goto('/login')
    await page.getByPlaceholder('默认 admin').first().fill('admin')
    await page.getByPlaceholder('默认 admin').last().fill(NEW_PASSWORD)
    await page.getByRole('button', { name: '进入控制台' }).click()
    await expect(page).toHaveURL('/')

    await page.getByRole('button', { name: /当前用户\s*admin/ }).click()
    await page.getByRole('menuitem', { name: '退出登录' }).click()

    await expect(page).toHaveURL(/\/login/, { timeout: 5000 })
    await expect(page.getByRole('button', { name: '进入控制台' })).toBeVisible()

    await page.goto('/')
    await expect(page).toHaveURL(/\/login/)
  })
})
