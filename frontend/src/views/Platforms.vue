<template>
  <div class="page platforms">
    <header class="page-header">
      <div>
        <h2 class="page-title">平台接入</h2>
        <p class="page-subtitle">管理本机向上级平台注册与主动目录上报。</p>
      </div>
    </header>
    <el-card shadow="hover">
      <template #header>
        <div class="header-row">
          <span>上级平台列表</span>
          <el-button type="primary" size="small" @click="onAdd">新增平台</el-button>
        </div>
      </template>
      <el-table :data="rows" :loading="loading" style="width: 100%">
        <el-table-column prop="name" label="名称" min-width="120" show-overflow-tooltip />
        <el-table-column prop="gbId" label="上级国标 ID" min-width="160" show-overflow-tooltip />
        <el-table-column prop="sipDomain" label="上级平台域" min-width="110" show-overflow-tooltip />
        <el-table-column label="信令地址" min-width="140">
          <template #default="{ row }">
            {{ row.sipIp }}:{{ row.sipPort }}
          </template>
        </el-table-column>
        <el-table-column prop="transport" label="传输" width="72" />
        <el-table-column label="启用" width="88">
          <template #default="{ row }">
            <el-switch
              :model-value="row.enabled"
              :disabled="switchLoadingId === row.id"
              @change="(v) => onEnabledSwitch(row, v)"
            />
          </template>
        </el-table-column>
        <el-table-column label="状态" width="88">
          <template #default="{ row }">
            <template v-if="!row.enabled">
              <el-tag type="info" size="small">未启用</el-tag>
            </template>
            <template v-else>
              <el-tag :type="row.online ? 'success' : 'danger'" size="small">
                {{ row.online ? '在线' : '离线' }}
              </el-tag>
            </template>
          </template>
        </el-table-column>
        <el-table-column label="操作" min-width="260" fixed="right">
          <template #default="{ row }">
            <el-button type="primary" link size="small" @click="onEdit(row)">编辑</el-button>
            <el-button type="primary" link size="small" @click="onPushConfig(row)">配置</el-button>
            <el-button
              type="success"
              link
              size="small"
              :disabled="!row.enabled"
              @click="onCatalogNotify(row)"
            >
              目录上报
            </el-button>
            <el-button type="danger" link size="small" @click="onDelete(row)">删除</el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <el-dialog v-model="dialogVisible" :title="editingId ? '编辑平台' : '新增平台'" width="620px">
      <el-form
        ref="formRef"
        :model="form"
        :rules="rules"
        label-width="168px"
        class="platform-form"
      >
        <div class="form-section">
          <div class="form-section-title">基本信息</div>
          <el-form-item label="名称" prop="name">
            <el-input v-model="form.name" placeholder="如：某市公安上级平台" />
          </el-form-item>
        </div>
        <div class="form-section">
          <div class="form-section-title">上级平台连接（信令）</div>
          <el-form-item label="上级平台国标 ID" prop="gbId">
            <el-input
              v-model="form.gbId"
              placeholder="20 位国标编码"
              maxlength="20"
              show-word-limit
              @blur="syncSipDomainFromGbId"
              @input="onGbIdInput"
            />
          </el-form-item>
          <el-form-item label="上级平台域" prop="sipDomain">
            <el-input v-model="form.sipDomain" placeholder="通常为国标 ID 前 10 位，可修改" />
          </el-form-item>
          <el-form-item label="上级信令 IP" prop="sipIp">
            <el-input v-model="form.sipIp" placeholder="IP 或域名" />
          </el-form-item>
          <el-form-item label="上级信令端口" prop="sipPort">
            <el-input-number
              v-model="form.sipPort"
              :min="1"
              :max="65535"
              controls-position="right"
              style="width: 100%"
            />
          </el-form-item>
          <el-form-item label="传输方式" prop="transport">
            <el-select v-model="form.transport" placeholder="请选择" style="width: 100%">
              <el-option label="UDP" value="udp" />
              <el-option label="TCP" value="tcp" />
            </el-select>
          </el-form-item>
        </div>
        <div class="form-section">
          <div class="form-section-title">本机在该上级的注册身份（鉴权）</div>
          <el-form-item label="注册用户名" prop="regUsername">
            <el-input v-model="form.regUsername" placeholder="Digest 鉴权用，默认可留空使用本机国标 ID" />
          </el-form-item>
          <el-form-item label="鉴权密码" prop="regPassword">
            <el-input
              v-model="form.regPassword"
              type="password"
              placeholder="新增必填；编辑时从服务器回填，不改可原样保存"
              show-password
            />
            <span class="form-item-hint">列表加载时会回填；留空保存可清空密码</span>
          </el-form-item>
        </div>
        <div class="form-section">
          <div class="form-section-title">可选</div>
          <el-form-item label="注册有效期(秒)" prop="registerExpires">
            <el-input-number
              v-model="form.registerExpires"
              :min="60"
              :max="86400"
              controls-position="right"
              style="width: 100%"
            />
            <span class="form-item-hint">对应 REGISTER 的 Expires，默认 3600</span>
          </el-form-item>
          <el-form-item label="心跳间隔(秒)" prop="heartbeatInterval">
            <el-input-number
              v-model="form.heartbeatInterval"
              :min="10"
              :max="3600"
              controls-position="right"
              style="width: 100%"
            />
          </el-form-item>
          <el-form-item label="是否启用" prop="enabled">
            <el-switch v-model="form.enabled" />
            <span class="form-item-hint">关闭后将向上级注销并拒绝该上级信令（403）</span>
          </el-form-item>
        </div>
      </el-form>
      <template #footer>
        <el-button @click="dialogVisible = false">取消</el-button>
        <el-button type="primary" @click="onSave">保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup>
import { reactive, ref, onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { api } from '@/api/client'

const router = useRouter()

const formRef = ref(null)
const rows = ref([])
const loading = ref(false)
const dialogVisible = ref(false)
const editingId = ref(null)
const switchLoadingId = ref(null)

const form = reactive({
  name: '',
  sipDomain: '',
  gbId: '',
  sipIp: '',
  sipPort: 5060,
  transport: 'udp',
  regUsername: '',
  regPassword: '',
  registerExpires: 3600,
  heartbeatInterval: 60,
  enabled: true,
})

const rules = {
  name: [{ required: true, message: '请输入名称', trigger: 'blur' }],
  sipDomain: [{ required: true, message: '请输入上级平台域', trigger: 'blur' }],
  gbId: [
    { required: true, message: '请输入上级平台国标 ID', trigger: 'blur' },
    { len: 20, message: '国标 ID 为 20 位', trigger: 'blur' },
  ],
  sipIp: [{ required: true, message: '请输入上级信令 IP', trigger: 'blur' }],
  sipPort: [
    { required: true, message: '请输入上级信令端口', trigger: 'blur' },
    { type: 'number', min: 1, max: 65535, message: '端口范围 1～65535', trigger: 'blur' },
  ],
  transport: [{ required: true, message: '请选择传输方式', trigger: 'change' }],
}

async function loadList() {
  loading.value = true
  try {
    const res = await api.listUpstreamPlatforms()
    const items = res?.data?.items ?? []
    rows.value = items.map((item) => ({
      id: item.id,
      name: item.name ?? '',
      sipDomain: item.sipDomain ?? '',
      gbId: item.gbId ?? '',
      sipIp: item.sipIp ?? '',
      sipPort: item.sipPort ?? 5060,
      transport: item.transport ?? 'udp',
      regUsername: item.regUsername ?? '',
      regPassword: item.regPassword ?? '',
      registerExpires: item.registerExpires ?? 3600,
      heartbeatInterval: item.heartbeatInterval ?? 60,
      enabled: item.enabled !== false,
      online: !!item.online,
      lastHeartbeatAt: item.lastHeartbeatAt ?? '',
    }))
  } catch (e) {
    ElMessage.error('加载列表失败：' + (e.message || '网络错误'))
    rows.value = []
  } finally {
    loading.value = false
  }
}

function resetForm() {
  form.name = ''
  form.sipDomain = ''
  form.gbId = ''
  form.sipIp = ''
  form.sipPort = 5060
  form.transport = 'udp'
  form.regUsername = ''
  form.regPassword = ''
  form.registerExpires = 3600
  form.heartbeatInterval = 60
  form.enabled = true
}

/** 国标 ID 至少 10 位时，取前 10 位填入上级平台域（与新增/编辑一致） */
function syncSipDomainFromGbId() {
  const id = String(form.gbId ?? '').trim()
  if (id.length >= 10) {
    form.sipDomain = id.slice(0, 10)
  }
}

function onGbIdInput() {
  if (String(form.gbId ?? '').trim().length === 20) {
    syncSipDomainFromGbId()
  }
}

function onAdd() {
  editingId.value = null
  resetForm()
  dialogVisible.value = true
  formRef.value?.clearValidate()
}

function onEdit(row) {
  editingId.value = row.id
  form.name = row.name ?? ''
  form.sipDomain = row.sipDomain ?? ''
  form.gbId = row.gbId ?? ''
  form.sipIp = row.sipIp ?? ''
  form.sipPort = row.sipPort ?? 5060
  form.transport = row.transport ?? 'udp'
  form.regUsername = row.regUsername ?? ''
  form.regPassword = row.regPassword ?? ''
  form.registerExpires = row.registerExpires ?? 3600
  form.heartbeatInterval = row.heartbeatInterval ?? 60
  form.enabled = row.enabled !== false
  dialogVisible.value = true
  formRef.value?.clearValidate()
}

async function onEnabledSwitch(row, val) {
  switchLoadingId.value = row.id
  try {
    const body = {
      name: row.name,
      sipDomain: row.sipDomain,
      gbId: row.gbId,
      sipIp: row.sipIp,
      sipPort: row.sipPort,
      transport: row.transport,
      regUsername: row.regUsername,
      registerExpires: row.registerExpires ?? 3600,
      heartbeatInterval: row.heartbeatInterval,
      enabled: val,
    }
    await api.putPlatform(row.id, body)
    ElMessage.success(val ? '已启用' : '已关闭')
    await loadList()
  } catch (e) {
    ElMessage.error('更新失败：' + (e.message || '网络错误'))
    await loadList()
  } finally {
    switchLoadingId.value = null
  }
}

function onPushConfig(row) {
  router.push({
    path: '/platforms/push-config',
    query: {
      upstreamId: String(row.id),
      name: row.name ?? '',
    },
  })
}

async function onCatalogNotify(row) {
  try {
    await api.postUpstreamCatalogNotify(row.id)
    ElMessage.success('目录上报已入队，将由信令线程发送')
  } catch (e) {
    ElMessage.error('上报失败：' + (e.message || '网络错误'))
  }
}

async function onDelete(row) {
  try {
    await ElMessageBox.confirm(`确定删除上级平台「${row.name}」？`, '删除确认', {
      type: 'warning',
    })
  } catch {
    return
  }
  try {
    await api.deletePlatform(row.id)
    ElMessage.success('已删除')
    await loadList()
  } catch (e) {
    ElMessage.error('删除失败：' + (e.message || '网络错误'))
  }
}

function onSave() {
  formRef.value?.validate(async (valid) => {
    if (!valid) return
    try {
      if (editingId.value) {
        const putBody = {
          name: form.name,
          sipDomain: form.sipDomain,
          gbId: form.gbId,
          sipIp: form.sipIp,
          sipPort: form.sipPort,
          transport: form.transport,
          regUsername: form.regUsername,
          regPassword: form.regPassword ?? '',
          registerExpires: form.registerExpires,
          heartbeatInterval: form.heartbeatInterval,
          enabled: form.enabled,
        }
        await api.putPlatform(editingId.value, putBody)
        ElMessage.success('已更新')
      } else {
        await api.postPlatform({
          name: form.name,
          sipDomain: form.sipDomain,
          gbId: form.gbId,
          sipIp: form.sipIp,
          sipPort: form.sipPort,
          transport: form.transport,
          regUsername: form.regUsername,
          regPassword: form.regPassword,
          registerExpires: form.registerExpires,
          heartbeatInterval: form.heartbeatInterval,
          enabled: form.enabled,
        })
        ElMessage.success('已添加')
      }
      dialogVisible.value = false
      await loadList()
    } catch (e) {
      ElMessage.error('保存失败：' + (e.message || '网络错误'))
    }
  })
}

onMounted(loadList)
</script>

<style scoped>
.page-header {
  margin-bottom: 16px;
}

.header-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.platform-form {
  padding-right: 8px;
}

.platform-form :deep(.el-form-item__label) {
  white-space: nowrap;
  line-height: 1.35;
}

.form-section {
  margin-bottom: 16px;
}

.form-section:last-child {
  margin-bottom: 0;
}

.form-section-title {
  font-size: 13px;
  color: var(--gb-text-muted, #6b7280);
  margin-bottom: 8px;
  padding-bottom: 4px;
}

.form-item-hint {
  margin-left: 8px;
  font-size: 12px;
  color: var(--gb-text-muted, #6b7280);
}
</style>
