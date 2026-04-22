<template>
  <div class="page">
    <header class="page-header">
      <div>
        <h2 class="page-title">修改密码</h2>
        <p class="page-subtitle">
          更新当前账号的登录密码，提升系统安全性。
        </p>
      </div>
    </header>

    <section class="page-section">
      <el-card shadow="hover">
        <el-form
          ref="formRef"
          :model="form"
          :rules="rules"
          label-width="90px"
          class="form"
        >
          <el-form-item label="旧密码" prop="oldPassword">
            <el-input
              v-model="form.oldPassword"
              type="password"
              show-password
              autocomplete="off"
            />
          </el-form-item>
          <el-form-item label="新密码" prop="newPassword">
            <el-input
              v-model="form.newPassword"
              type="password"
              show-password
              autocomplete="off"
            />
          </el-form-item>
          <el-form-item label="确认密码" prop="confirmPassword">
            <el-input
              v-model="form.confirmPassword"
              type="password"
              show-password
              autocomplete="off"
            />
          </el-form-item>
          <el-form-item>
            <div class="form-actions">
              <span class="hint">
                修改成功后需使用新密码重新登录。
              </span>
              <div>
                <el-button @click="onReset">重置</el-button>
                <el-button type="primary" :loading="loading" @click="onSubmit">
                  保存修改
                </el-button>
              </div>
            </div>
          </el-form-item>
        </el-form>
      </el-card>
    </section>
  </div>
</template>

<script setup>
import { reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import { authFetch, setToken } from '../router'

const router = useRouter()
const formRef = ref(null)
const loading = ref(false)

const initial = {
  oldPassword: '',
  newPassword: '',
  confirmPassword: '',
}

const form = reactive({ ...initial })

const rules = {
  oldPassword: [{ required: true, message: '请输入旧密码', trigger: 'blur' }],
  newPassword: [
    { required: true, message: '请输入新密码', trigger: 'blur' },
    { min: 6, message: '新密码不少于 6 位', trigger: 'blur' },
  ],
  confirmPassword: [
    { required: true, message: '请再次输入新密码', trigger: 'blur' },
    {
      validator: (_rule, value, callback) => {
        if (!value) return callback()
        if (value !== form.newPassword) {
          callback(new Error('两次输入的密码不一致'))
        } else {
          callback()
        }
      },
      trigger: 'blur',
    },
  ],
}

function onReset() {
  Object.assign(form, initial)
}

async function onSubmit() {
  if (!formRef.value) return
  await formRef.value.validate(async (valid) => {
    if (!valid) return
    loading.value = true
    try {
      const origin = window.location.origin
      const res = await authFetch(`${origin}/api/auth/change-password`, {
        method: 'POST',
        body: JSON.stringify({
          oldPassword: form.oldPassword,
          newPassword: form.newPassword,
        }),
      })
      const data = await res.json().catch(() => ({}))
      if (res.ok && data.code === 0) {
        ElMessage.success('密码已修改，请使用新密码重新登录')
        setToken(null)
        onReset()
        router.push('/login')
      } else {
        ElMessage.error((data && data.message) || '修改失败')
      }
    } catch (e) {
      ElMessage.error(e?.message || '请求失败')
    } finally {
      loading.value = false
    }
  })
}
</script>

<style scoped>
.form {
  max-width: 480px;
}

.form-actions {
  width: 100%;
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  row-gap: 8px;
}

.hint {
  font-size: 12px;
  color: var(--gb-text-soft);
}

.form-actions > div:last-child {
  display: inline-flex;
  align-items: center;
  gap: 8px;
}
</style>

