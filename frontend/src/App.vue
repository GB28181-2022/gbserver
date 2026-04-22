<template>
  <el-container class="app-shell" direction="vertical">
    <template v-if="showLayout">
      <el-header class="app-header">
        <div class="header-inner">
          <div class="header-left">
            <div class="brand-mark"></div>
            <div class="brand-text">
              <span class="brand-title">GB28181 控制台</span>
              <span class="brand-subtitle">国标服务器系统</span>
            </div>
          </div>
          <div class="header-right">
            <el-button
              v-if="!token"
              size="small"
              type="primary"
              class="login-btn"
              @click="goLogin"
            >
              登录
            </el-button>
            <el-dropdown v-else trigger="click">
              <span class="user-chip">
                <span class="user-dot"></span>
                <span class="user-label">当前用户</span>
                <span class="user-name">{{ username }}</span>
              </span>
              <template #dropdown>
                <el-dropdown-menu>
                  <el-dropdown-item @click="goHome">返回概览</el-dropdown-item>
                  <el-dropdown-item @click="goChangePassword">
                    修改密码
                  </el-dropdown-item>
                  <el-dropdown-item divided @click="onLogout">
                    退出登录
                  </el-dropdown-item>
                </el-dropdown-menu>
              </template>
            </el-dropdown>
          </div>
        </div>
      </el-header>

      <el-container class="app-body" direction="horizontal">
        <el-aside
          :width="sidebarWidth"
          class="app-sidebar"
        >
          <div class="sidebar-inner">
            <div class="sidebar-top-row">
              <div class="sidebar-top-spacer"></div>
              <el-tooltip
                :content="isSidebarCollapsed ? '展开侧边栏' : '收起侧边栏'"
                placement="right"
              >
                <el-button
                  circle
                  size="small"
                  class="sidebar-toggle-btn"
                  @click="toggleSidebar"
                >
                  <el-icon>
                    <component :is="isSidebarCollapsed ? Expand : Fold" />
                  </el-icon>
                </el-button>
              </el-tooltip>
            </div>
            <el-menu
              :default-active="activeMenu"
              :collapse="isSidebarCollapsed"
              router
              class="app-menu"
            >
              <el-menu-item index="/">
                <el-icon><House /></el-icon>
                <span>概览</span>
              </el-menu-item>

              <el-sub-menu index="server">
                <template #title>
                  <el-icon><Monitor /></el-icon>
                  <span class="menu-group-label">服务端功能</span>
                </template>
                <el-menu-item index="/devices">
                  <el-icon><Platform /></el-icon>
                  <span>平台管理</span>
                </el-menu-item>
                <el-menu-item index="/preview-wall">
                  <el-icon><VideoPlay /></el-icon>
                  <span>实时预览</span>
                </el-menu-item>
                <el-menu-item index="/live">
                  <el-icon><VideoCamera /></el-icon>
                  <span>摄像头管理</span>
                </el-menu-item>
                <el-menu-item v-if="false" index="/replay">
                  <span>录像回放</span>
                </el-menu-item>
                <el-menu-item index="/alarms">
                  <el-icon><Bell /></el-icon>
                  <span>告警</span>
                </el-menu-item>
              </el-sub-menu>

              <el-sub-menu index="client">
                <template #title>
                  <el-icon><Cpu /></el-icon>
                  <span class="menu-group-label">客户端功能</span>
                </template>
                <el-menu-item index="/catalog">
                  <el-icon><Collection /></el-icon>
                  <span>目录编组</span>
                </el-menu-item>
                <el-menu-item index="/platforms">
                  <el-icon><Link /></el-icon>
                  <span>平台接入</span>
                </el-menu-item>
              </el-sub-menu>

              <el-menu-item index="/config">
                <el-icon><Setting /></el-icon>
                <span>系统配置</span>
              </el-menu-item>
            </el-menu>
          </div>
        </el-aside>

        <el-main class="app-main">
          <transition name="view-fade" mode="out-in">
            <router-view />
          </transition>
        </el-main>
      </el-container>
    </template>

    <template v-else>
      <router-view />
    </template>
  </el-container>
</template>

<script setup>
import { computed, inject, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Bell, Collection, Cpu, Fold, House, Link, Monitor, Platform, Setting, VideoCamera, VideoPlay, Expand } from '@element-plus/icons-vue'
import { getToken, setToken, setUser, authFetch } from './router'

const route = useRoute()
const router = useRouter()

const tokenRef = inject('tokenRef')
const userRef = inject('userRef')
const token = computed(() => (tokenRef ? tokenRef.value : null))
const username = computed(() => (userRef && userRef.value) ? userRef.value : 'admin')

const showLayout = computed(() => route.path !== '/login')
const activeMenu = computed(() => route.meta?.activeMenu || route.path || '/')

const isSidebarCollapsed = ref(false)
const sidebarWidth = computed(() => (isSidebarCollapsed.value ? '64px' : '220px'))

function goLogin() {
  router.push('/login')
}

function goHome() {
  router.push('/')
}

function goChangePassword() {
  router.push('/account/password')
}

async function onLogout() {
  try {
    const origin = typeof window !== 'undefined' ? window.location.origin : ''
    await authFetch(`${origin}/api/auth/logout`, { method: 'POST' })
  } catch (_) { /* 忽略网络错误，仍清除本地 */ }
  setToken(null)
  setUser(null)
  if (tokenRef) tokenRef.value = null
  if (userRef) userRef.value = null
  router.push('/login')
}

function toggleSidebar() {
  isSidebarCollapsed.value = !isSidebarCollapsed.value
}

onMounted(async () => {
  if (!getToken() || !userRef) return
  try {
    const origin = typeof window !== 'undefined' ? window.location.origin : ''
    const res = await authFetch(`${origin}/api/auth/me`)
    const data = await res.json().catch(() => ({}))
    if (res.ok && data.code === 0 && data.data && data.data.user) {
      const name = data.data.user.username
      userRef.value = name
      setUser(name)
    } else if (res.status === 401) {
      setToken(null)
      tokenRef && (tokenRef.value = null)
      userRef.value = null
      if (route.path !== '/login') router.push('/login')
    }
  } catch (_) { /* 忽略网络错误，保留本地 token */ }
})
</script>

<style scoped>
.app-shell {
  height: 100vh;
  display: flex;
  flex-direction: column;
  background: transparent;
}

.app-header {
  height: var(--gb-layout-header-height);
  padding: 0 20px;
  border-bottom: 1px solid rgba(209, 213, 219, 0.9);
  background: #ffffff;
  box-shadow: 0 10px 30px rgba(15, 23, 42, 0.06);
}

.header-inner {
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.header-left {
  display: flex;
  align-items: center;
  gap: 12px;
}

.brand-mark {
  width: 22px;
  height: 22px;
  border-radius: 6px;
  background: conic-gradient(
    from 220deg,
    #22c55e,
    #3b82f6,
    #0ea5e9,
    #22c55e
  );
  box-shadow: 0 0 0 1px rgba(15, 23, 42, 0.9),
    0 0 30px rgba(56, 189, 248, 0.45);
}

.brand-text {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.brand-title {
  font-size: 13px;
  letter-spacing: 0.16em;
  text-transform: uppercase;
  color: #6b7280;
}

.brand-subtitle {
  font-size: 15px;
  font-weight: 500;
  letter-spacing: 0.04em;
}

.header-right {
  display: flex;
  align-items: center;
  gap: 12px;
}

.login-btn {
  border-radius: 999px;
}

.user-chip {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 4px 14px;
  border-radius: 999px;
  background: rgba(243, 244, 246, 0.9);
  box-shadow: 0 0 0 1px rgba(148, 163, 184, 0.65);
  cursor: pointer;
  font-size: 13px;
  color: var(--gb-text-muted);
}

.user-chip:hover {
  box-shadow: 0 0 0 1px rgba(148, 163, 184, 0.7);
  background: #ffffff;
}

.user-dot {
  width: 8px;
  height: 8px;
  border-radius: 999px;
  background: var(--gb-success);
  box-shadow: 0 0 12px rgba(52, 211, 153, 0.8);
}

.user-name {
  font-weight: 500;
  letter-spacing: 0.08em;
  text-transform: uppercase;
}

.user-label {
  font-size: 11px;
  color: var(--gb-text-soft);
}

.app-body {
  flex: 1;
  min-height: 0;
  overflow: hidden;
  background: radial-gradient(circle at 0 0, #e5e7eb 0, #f9fafb 55%, #ffffff 100%);
}

.app-sidebar {
  height: 100%;
  border-right: 1px solid rgba(209, 213, 219, 0.9);
  background: linear-gradient(180deg, #f3f4f6, #ffffff);
  transition: width 0.2s ease;
}

.sidebar-inner {
  height: 100%;
  padding: 12px 8px 16px 8px;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.sidebar-top-row {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  margin-bottom: 8px;
}

.sidebar-top-spacer {
  flex: 1;
}

.sidebar-toggle-btn {
  border-radius: 999px;
  padding: 4px;
}

.app-menu {
  flex: 1;
  border-right: none;
  background: transparent;
}

.menu-group-label {
  font-size: 11px;
  letter-spacing: 0.18em;
  text-transform: uppercase;
  color: #9ca3af;
}

.app-main {
  padding: 16px 12px 16px 12px;
  overflow: auto;
}

/* 优化页面内容区域 */
.page {
  max-width: 100%;
  margin: 0 auto;
}

/* 优化卡片布局 */
.el-card {
  margin-bottom: 12px;
}

/* 表格自适应 */
.el-table {
  width: 100%;
}

/* 在小屏幕上进一步减少padding */
@media (max-width: 1200px) {
  .app-main {
    padding: 12px 8px 12px 8px;
  }
}
</style>
