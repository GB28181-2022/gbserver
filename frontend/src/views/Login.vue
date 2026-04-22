<template>
  <div class="login-shell">
    <div class="login-noise"></div>
    <div class="login-grid"></div>
    <div class="login-panel">
      <header class="login-header">
        <div class="login-badge">GB28181</div>
        <h1 class="login-title">国标服务器系统</h1>
        <p class="login-subtitle">统一管理平台接入、目录编组与实时视频联动</p>
      </header>

      <el-card class="login-card" shadow="never">
        <el-form
          ref="formRef"
          :model="form"
          :rules="rules"
          label-position="top"
          @submit.prevent="onSubmit"
        >
          <el-form-item label="账号" prop="username">
            <el-input
              v-model="form.username"
              placeholder="默认 admin"
              autofocus
              clearable
            />
          </el-form-item>
          <el-form-item label="密码" prop="password">
            <el-input
              v-model="form.password"
              type="password"
              placeholder="默认 admin"
              show-password
              clearable
            />
          </el-form-item>
          <div class="login-actions">
            <span class="login-hint">默认账号：admin / admin</span>
            <el-button
              type="primary"
              :loading="loading"
              @click="onSubmit"
            >
              进入控制台
            </el-button>
          </div>
        </el-form>
      </el-card>
    </div>
  </div>
</template>

<script setup>
import { ref, reactive, inject } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import { setToken, getToken, setUser, getUser } from '../router'

const router = useRouter()
const tokenRef = inject('tokenRef')
const userRef = inject('userRef')

const formRef = ref(null)
const loading = ref(false)

const form = reactive({
  username: 'admin',
  password: 'admin',
})

const rules = {
  username: [{ required: true, message: '请输入用户名', trigger: 'blur' }],
  password: [{ required: true, message: '请输入密码', trigger: 'blur' }],
}

async function onSubmit() {
  if (!formRef.value) return
  await formRef.value.validate(async (valid) => {
    if (!valid) return
    loading.value = true
    try {
      const origin = window.location.origin
      const healthRes = await fetch(`${origin}/api/health`)
      if (!healthRes.ok) {
        ElMessage.error('后端不可达，请检查服务是否启动')
        return
      }
      const health = await healthRes.json()
      const status = (health && health.data && health.data.status) ? health.data.status : health.status
      if (status !== 'ok') {
        ElMessage.warning('健康检查异常')
        return
      }
      // 占位：后续对接 POST /api/auth/login
      const loginRes = await fetch(`${origin}/api/auth/login`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          username: form.username,
          password: form.password,
        }),
      })
      const data = await loginRes.json().catch(() => ({}))
      if (loginRes.ok && data.code === 0 && data.data) {
        setToken(data.data.token || '')
        setUser((data.data.user && data.data.user.username) ? data.data.user.username : form.username)
        if (tokenRef) tokenRef.value = getToken()
        if (userRef) userRef.value = getUser()
        ElMessage.success('登录成功')
        router.push('/')
      } else {
        const msg = (data && data.message) ? data.message : '用户名或密码错误'
        ElMessage.error(msg)
      }
    } catch (e) {
      ElMessage.error(e?.message || '登录失败')
    } finally {
      loading.value = false
    }
  })
}
</script>

<style scoped>
.login-shell {
  position: relative;
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 32px 16px;
  overflow: hidden;
}

.login-noise {
  position: absolute;
  inset: -40px;
  background-image: radial-gradient(
      circle at 0 0,
      rgba(56, 189, 248, 0.2),
      transparent 55%
    ),
    radial-gradient(circle at 100% 100%, rgba(37, 99, 235, 0.16), transparent 55%);
  opacity: 0.85;
  mix-blend-mode: screen;
  pointer-events: none;
}

.login-grid {
  position: absolute;
  inset: 0;
  background-image: linear-gradient(
      rgba(15, 23, 42, 0.4) 1px,
      transparent 1px
    ),
    linear-gradient(
      90deg,
      rgba(15, 23, 42, 0.4) 1px,
      transparent 1px
    );
  background-size: 32px 32px;
  opacity: 0.32;
  mask-image: radial-gradient(circle at center, black 0, transparent 75%);
  pointer-events: none;
}

.login-panel {
  position: relative;
  width: 100%;
  max-width: 460px;
  z-index: 1;
}

.login-header {
  margin-bottom: 18px;
}

.login-badge {
  display: inline-flex;
  align-items: center;
  padding: 2px 10px;
  border-radius: 999px;
  font-size: 11px;
  letter-spacing: 0.18em;
  text-transform: uppercase;
  color: #93c5fd;
  background: rgba(15, 23, 42, 0.85);
  box-shadow: 0 0 0 1px rgba(59, 130, 246, 0.65);
}

.login-title {
  margin: 10px 0 4px 0;
  font-size: 22px;
  letter-spacing: 0.06em;
}

.login-subtitle {
  margin: 0;
  font-size: 13px;
  color: var(--gb-text-soft);
}

.login-card {
  border-radius: 18px;
  padding: 16px 18px 18px 18px;
  backdrop-filter: blur(32px);
}

.login-actions {
  margin-top: 6px;
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.login-hint {
  font-size: 12px;
  color: var(--gb-text-soft);
}
</style>
