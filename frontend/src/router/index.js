/**
 * 国标服务器系统 - 前端路由
 * /login 登录页，/ 首页；未登录访问非 /login 重定向到登录页。
 */
import { createRouter, createWebHistory } from 'vue-router'

const AUTH_TOKEN_KEY = 'gb_service_token'
const AUTH_USER_KEY = 'gb_service_user'

export function getToken() {
  return localStorage.getItem(AUTH_TOKEN_KEY)
}

export function setToken(value) {
  if (value) localStorage.setItem(AUTH_TOKEN_KEY, value)
  else localStorage.removeItem(AUTH_TOKEN_KEY)
}

export function getUser() {
  return localStorage.getItem(AUTH_USER_KEY)
}

export function setUser(value) {
  if (value) localStorage.setItem(AUTH_USER_KEY, value)
  else localStorage.removeItem(AUTH_USER_KEY)
}

/** 带认证头的 fetch，未登录时无 token */
export function authFetch(url, options = {}) {
  const token = getToken()
  const headers = { ...(options.headers || {}), 'Content-Type': 'application/json' }
  if (token) headers.Authorization = `Bearer ${token}`
  return fetch(url, { ...options, headers })
}

const routes = [
  { path: '/login', name: 'Login', component: () => import('../views/Login.vue'), meta: { public: true } },
  { path: '/', name: 'Home', component: () => import('../views/Home.vue') },
  { path: '/account/password', name: 'ChangePassword', component: () => import('../views/ChangePassword.vue') },
  { path: '/config', name: 'Config', component: () => import('../views/Config.vue') },
  { path: '/devices', name: 'Devices', component: () => import('../views/Devices.vue') },
  { path: '/live', name: 'Live', component: () => import('../views/Live.vue') },
  { path: '/preview-wall', name: 'PreviewWall', component: () => import('../views/PreviewWall.vue') },
  { path: '/live/replay', name: 'CameraReplay', component: () => import('../views/CameraReplay.vue') },
  { path: '/replay', name: 'Replay', component: () => import('../views/Replay.vue') },
  { path: '/alarms', name: 'Alarms', component: () => import('../views/Alarms.vue') },
  { path: '/catalog', name: 'Catalog', component: () => import('../views/Catalog.vue') },
  { path: '/platforms', name: 'Platforms', component: () => import('../views/Platforms.vue') },
  {
    path: '/platforms/push-config',
    name: 'UpstreamCatalogPushConfig',
    component: () => import('../views/UpstreamCatalogPushConfig.vue'),
    meta: { activeMenu: '/platforms' },
  },
]

const router = createRouter({
  history: createWebHistory(),
  routes,
})

router.beforeEach((to, _from, next) => {
  const token = getToken()
  if (to.meta.public) {
    if (token && to.path === '/login') return next('/')
    return next()
  }
  if (!token) return next('/login')
  next()
})

export default router
